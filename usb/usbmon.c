/*
 * Phoenix-RTOS
 *
 * USB bus monitor - PCAPng trace output
 *
 * Captures URB submissions and completions and writes them to a PCAPng file
 * using LINKTYPE_USB_LINUX_MMAPPED (220) format, compatible with Wireshark.
 *
 * Copyright 2026 Phoenix Systems
 * Author: Adam Greloch
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <sys/threads.h>
#include <sys/minmax.h>
#include <time.h>
#include <unistd.h>

#include "usbmon.h"
#include "hcd.h"
#include "log.h"

#undef USB_LOG_TAG
#define USB_LOG_TAG "usbmon"


/* PCAPng block types */
#define PCAPNG_BT_SHB 0x0A0D0D0Au /* Section Header Block */
#define PCAPNG_BT_IDB 0x00000001u /* Interface Description Block */
#define PCAPNG_BT_EPB 0x00000006u /* Enhanced Packet Block */

/* PCAPng constants */
#define PCAPNG_BOM          0x1A2B3C4Du
#define PCAPNG_VERSION_MAJ  1
#define PCAPNG_VERSION_MIN  0
#define PCAPNG_LINKTYPE_USB 220 /* LINKTYPE_USB_LINUX_MMAPPED */

/* Size of the USB monitor packet header (matches Linux struct usbmon_packet) */
#define USBMON_PKT_SIZE 64

/* Default maximum payload bytes captured per event */
#define USBMON_MAX_DATA 256

/* Maximum number of concurrently tracked URBs */
#define USBMON_MAX_TRACKED 64

/* EPB fixed overhead: block_type(4) + block_total_len(4) + iface_id(4) +
 * ts_high(4) + ts_low(4) + cap_len(4) + orig_len(4) + trailing_len(4) = 32 */
#define USBMON_EPB_OVERHEAD 32

/* Transfer type values in PCAP/PCAPng USB format */
enum {
	PCAP_XFER_ISO = 0,
	PCAP_XFER_INTR = 1,
	PCAP_XFER_CTRL = 2,
	PCAP_XFER_BULK = 3,
};


/* PCAPng option codes */
#define PCAPNG_OPT_ENDOFOPT     0
#define PCAPNG_OPT_IF_NAME      2
#define PCAPNG_OPT_IF_DESC      3
#define PCAPNG_OPT_SHB_USERAPPL 4
#define PCAPNG_OPT_IF_TSRESOL   9


/* PCAPng Enhanced Packet Block - fixed header portion */
struct pcapng_epb_hdr {
	uint32_t blockType;
	uint32_t blockTotalLength;
	uint32_t interfaceId;
	uint32_t timestampHigh;
	uint32_t timestampLow;
	uint32_t capturedPacketLength;
	uint32_t originalPacketLength;
} __attribute__((packed));


/*
 * USB monitor packet header - LINKTYPE_USB_LINUX_MMAPPED.
 * Layout matches Linux struct usbmon_packet (64 bytes total).
 */
struct usbmon_packet {
	uint64_t id;          /*  0: URB ID (transfer pointer) */
	uint8_t type;         /*  8: 'S' submit, 'C' complete, 'E' error */
	uint8_t xferType;     /*  9: 0=iso, 1=intr, 2=ctrl, 3=bulk */
	uint8_t epnum;        /* 10: endpoint number | direction (bit 7) */
	uint8_t devnum;       /* 11: device address */
	uint16_t busnum;      /* 12: bus number */
	uint8_t flagSetup;    /* 14: 0 if setup data present */
	uint8_t flagData;     /* 15: 0 if payload data present */
	int64_t tsSec;        /* 16: timestamp seconds */
	int32_t tsUsec;       /* 24: timestamp microseconds */
	int32_t status;       /* 28: URB status (negative errno on error) */
	uint32_t length;      /* 32: data length (requested or actual) */
	uint32_t lenCap;      /* 36: header + captured data length */
	union {               /* 40: */
		uint8_t setup[8]; /* setup packet for control S-type */
		struct {
			int32_t errorCount;
			int32_t numdesc;
		} iso;
	} s;
	int32_t interval;   /* 48: polling interval (intr/iso) */
	int32_t startFrame; /* 52: ISO start frame */
	uint32_t xferFlags; /* 56: transfer flags */
	uint32_t ndesc;     /* 60: number of ISO descriptors */
} __attribute__((packed));


_Static_assert(sizeof(struct usbmon_packet) == USBMON_PKT_SIZE,
		"usbmon_packet must be exactly 64 bytes");


/* Tracking entry for correlating submission with completion */
typedef struct {
	uintptr_t id; /* Transfer pointer (0 = slot empty) */
	uint8_t epnum;
	uint8_t devnum;
	uint16_t busnum;
	int interval;
} usbmon_entry_t;


static struct {
	int fd;
	handle_t lock;
	uint32_t snaplen;
	usbmon_entry_t entries[USBMON_MAX_TRACKED];
	/* Scratch buffer for one EPB: overhead + USB header + max payload */
	uint8_t buf[USBMON_EPB_OVERHEAD + USBMON_PKT_SIZE + USBMON_MAX_DATA];
} usbmon_common = { .fd = -1 };


/* Map transfer type enum to PCAP transfer type */
static uint8_t usbmon_pcapXfer(int type)
{
	switch (type) {
		case usb_transfer_control: return PCAP_XFER_CTRL;
		case usb_transfer_isochronous: return PCAP_XFER_ISO;
		case usb_transfer_bulk: return PCAP_XFER_BULK;
		case usb_transfer_interrupt: return PCAP_XFER_INTR;
		default: return 0;
	}
}


/* Store pipe info so completion can look it up without having the pipe */
static void usbmon_track(uintptr_t id, uint8_t epnum, uint8_t devnum, uint16_t busnum, int interval)
{
	int i, empty = -1;
	for (i = 0; i < USBMON_MAX_TRACKED; i++) {
		if (usbmon_common.entries[i].id == id) {
			/* Reuse existing slot (re-submitted transfer) */
			usbmon_common.entries[i].epnum = epnum;
			usbmon_common.entries[i].devnum = devnum;
			usbmon_common.entries[i].busnum = busnum;
			usbmon_common.entries[i].interval = interval;
			return;
		}
		if ((empty < 0) && (usbmon_common.entries[i].id == 0)) {
			empty = i;
		}
	}

	if (empty >= 0) {
		usbmon_common.entries[empty].id = id;
		usbmon_common.entries[empty].epnum = epnum;
		usbmon_common.entries[empty].devnum = devnum;
		usbmon_common.entries[empty].busnum = busnum;
		usbmon_common.entries[empty].interval = interval;
	}
	/* else: table full - completion will use fallback values */
}


static usbmon_entry_t *usbmon_lookup(uintptr_t id)
{
	for (int i = 0; i < USBMON_MAX_TRACKED; i++) {
		if (usbmon_common.entries[i].id == id) {
			return &usbmon_common.entries[i];
		}
	}
	return NULL;
}


/*
 * Write one PCAPng Enhanced Packet Block: EPB header + usbmon_packet + payload.
 * Caller must hold usbmon_common.lock.
 */
static void usbmon_writeEvent(uint8_t eventType, usb_transfer_t *t, uint8_t epnum, uint8_t devnum,
		uint16_t busnum, int interval, const void *data, size_t dataLen)
{
	struct pcapng_epb_hdr *epb;
	struct usbmon_packet *pkt;
	struct timespec ts;
	uint64_t tsUs;
	size_t cap;
	size_t pktCap, pktOrig, pktPadded;
	uint32_t blockTotalLen;
	ssize_t wr;

	cap = (dataLen > usbmon_common.snaplen) ? usbmon_common.snaplen : dataLen;

	pktCap = USBMON_PKT_SIZE + cap;
	pktOrig = USBMON_PKT_SIZE + dataLen;
	pktPadded = (pktCap + 3u) & ~3u;
	blockTotalLen = (uint32_t)(USBMON_EPB_OVERHEAD + pktPadded);

	/* Zero entire record area */
	memset(usbmon_common.buf, 0, blockTotalLen);

	/* Timestamp */
	if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
		ts.tv_sec = 0;
		ts.tv_nsec = 0;
	}
	tsUs = (uint64_t)ts.tv_sec * 1000000u + (uint64_t)(ts.tv_nsec / 1000);

	/* EPB header */
	epb = (struct pcapng_epb_hdr *)usbmon_common.buf;
	epb->blockType = PCAPNG_BT_EPB;
	epb->blockTotalLength = blockTotalLen;
	epb->interfaceId = 0;
	epb->timestampHigh = (uint32_t)(tsUs >> 32);
	epb->timestampLow = (uint32_t)(tsUs & 0xFFFFFFFFu);
	epb->capturedPacketLength = (uint32_t)pktCap;
	epb->originalPacketLength = (uint32_t)pktOrig;

	/* USB monitor packet header */
	pkt = (struct usbmon_packet *)(usbmon_common.buf + sizeof(*epb));
	pkt->id = (uint64_t)(uintptr_t)t;
	pkt->type = eventType;
	pkt->xferType = usbmon_pcapXfer(t->type);
	pkt->epnum = epnum;
	pkt->devnum = devnum;
	pkt->busnum = busnum;

	/* Setup packet (only for control submissions) */
	if ((eventType == 'S') && (t->type == usb_transfer_control) &&
			(t->setup != NULL)) {
		pkt->flagSetup = 0; /* setup data present */
		memcpy(pkt->s.setup, t->setup, sizeof(pkt->s.setup));
	}
	else {
		pkt->flagSetup = '-';
	}

	/* Data flag: 0 = payload follows header, non-zero = no payload */
	if (cap > 0) {
		pkt->flagData = 0;
	}
	else {
		pkt->flagData = (t->direction == usb_dir_in) ? '<' : '>';
	}

	pkt->tsSec = (int64_t)ts.tv_sec;
	pkt->tsUsec = (int32_t)(ts.tv_nsec / 1000);

	if (eventType == 'C') {
		pkt->status = (t->error != 0) ? -(int32_t)t->error : 0;
	}

	pkt->length = (eventType == 'S') ? (uint32_t)t->size : (uint32_t)t->transferred;
	pkt->lenCap = (uint32_t)(USBMON_PKT_SIZE + cap);
	pkt->interval = (int32_t)interval;

	/* Copy payload data after usbmon header */
	if ((cap > 0) && (data != NULL)) {
		memcpy(usbmon_common.buf + sizeof(*epb) + USBMON_PKT_SIZE, data, cap);
	}

	/* Trailing Block Total Length (after padded packet data) */
	memcpy(usbmon_common.buf + sizeof(*epb) + pktPadded, &blockTotalLen, sizeof(blockTotalLen));

	/* Write entire EPB atomically */
	wr = write(usbmon_common.fd, usbmon_common.buf, blockTotalLen);
	if (wr != (ssize_t)blockTotalLen) {
		log_error("pcapng write failed: %d\n", (wr < 0) ? errno : EIO);
	}
}


/*
 * Write a PCAPng option (code + length + value padded to 4 bytes) into buf
 * at position off. Returns the new offset.
 */
static uint32_t usbmon_writeOpt(uint8_t *buf, uint32_t off, uint16_t code, const void *val, uint16_t len)
{
	memcpy(buf + off, &code, 2);
	off += 2;
	memcpy(buf + off, &len, 2);
	off += 2;

	if (len > 0) {
		memcpy(buf + off, val, len);
		off += len;
		while ((off & 3u) != 0) {
			buf[off++] = 0;
		}
	}

	return off;
}


/* Finish a PCAPng block: fill blockType and blockTotalLength fields. */
static uint32_t usbmon_finishBlock(uint8_t *buf, uint32_t off, uint32_t blockType)
{
	uint32_t blockLen = off + 4; /* +4 for trailing blockTotalLength */

	memcpy(buf, &blockType, 4);
	memcpy(buf + 4, &blockLen, 4);
	memcpy(buf + off, &blockLen, 4);

	return blockLen;
}


/* Open a PCAPng file and write the SHB + IDB. Caller must hold lock or ensure exclusivity. */
static int usbmon_open(const char *pcapPath, uint32_t snaplen)
{
	uint8_t hdr[128];
	uint32_t off, total;
	uint32_t val32;
	uint16_t val16;
	int64_t val64;
	uint8_t tsresol;

	if (pcapPath == NULL) {
		return -EINVAL;
	}

	usbmon_common.snaplen = snaplen == 0 ? USBMON_MAX_DATA : min(USBMON_MAX_DATA, snaplen);

	usbmon_common.fd = open(pcapPath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (usbmon_common.fd < 0) {
		log_error("cannot open %s: %d\n", pcapPath, errno);
		return -errno;
	}

	memset(usbmon_common.entries, 0, sizeof(usbmon_common.entries));

	/* --- Section Header Block --- */
	off = 8; /* skip block_type + block_total_length (filled by finish) */

	val32 = PCAPNG_BOM;
	memcpy(hdr + off, &val32, 4);
	off += 4;

	val16 = PCAPNG_VERSION_MAJ;
	memcpy(hdr + off, &val16, 2);
	off += 2;
	val16 = PCAPNG_VERSION_MIN;
	memcpy(hdr + off, &val16, 2);
	off += 2;

	val64 = -1; /* section length: unspecified */
	memcpy(hdr + off, &val64, 8);
	off += 8;

	off = usbmon_writeOpt(hdr, off, PCAPNG_OPT_SHB_USERAPPL, "phoenix-rtos-usbmon", 19);
	off = usbmon_writeOpt(hdr, off, PCAPNG_OPT_ENDOFOPT, NULL, 0);

	total = usbmon_finishBlock(hdr, off, PCAPNG_BT_SHB);

	if (write(usbmon_common.fd, hdr, total) != (ssize_t)total) {
		log_error("failed to write pcapng SHB\n");
		close(usbmon_common.fd);
		usbmon_common.fd = -1;
		return -EIO;
	}

	/* --- Interface Description Block --- */
	off = 8;

	val16 = PCAPNG_LINKTYPE_USB;
	memcpy(hdr + off, &val16, 2);
	off += 2;
	val16 = 0; /* reserved */
	memcpy(hdr + off, &val16, 2);
	off += 2;

	val32 = USBMON_PKT_SIZE + snaplen;
	memcpy(hdr + off, &val32, 4);
	off += 4;

	off = usbmon_writeOpt(hdr, off, PCAPNG_OPT_IF_NAME, "usbmon0", 7);
	off = usbmon_writeOpt(hdr, off, PCAPNG_OPT_IF_DESC, "Phoenix-RTOS USB Monitor", 24);
	tsresol = 6; /* 10^-6 = microseconds */
	off = usbmon_writeOpt(hdr, off, PCAPNG_OPT_IF_TSRESOL, &tsresol, 1);
	off = usbmon_writeOpt(hdr, off, PCAPNG_OPT_ENDOFOPT, NULL, 0);

	total = usbmon_finishBlock(hdr, off, PCAPNG_BT_IDB);

	if (write(usbmon_common.fd, hdr, total) != (ssize_t)total) {
		log_error("failed to write pcapng IDB\n");
		close(usbmon_common.fd);
		usbmon_common.fd = -1;
		return -EIO;
	}

	return 0;
}


int usbmon_init(const char *pcapPath)
{
	int ret;

	if (mutexCreate(&usbmon_common.lock) != 0) {
		return -ENOMEM;
	}

	if (pcapPath != NULL) {
		ret = usbmon_open(pcapPath, 0);
		if (ret < 0) {
			return ret;
		}
	}

	return 0;
}


void usbmon_destroy(void)
{
	if (usbmon_common.fd >= 0) {
		close(usbmon_common.fd);
		usbmon_common.fd = -1;
	}
}


int usbmon_start(const char *pcapPath, uint32_t snaplen)
{
	int ret;

	mutexLock(usbmon_common.lock);

	if (usbmon_common.fd >= 0) {
		mutexUnlock(usbmon_common.lock);
		return -EBUSY;
	}

	ret = usbmon_open(pcapPath, snaplen);

	mutexUnlock(usbmon_common.lock);

	return ret;
}


int usbmon_stop(void)
{
	mutexLock(usbmon_common.lock);

	if (usbmon_common.fd < 0) {
		mutexUnlock(usbmon_common.lock);
		return -ENOENT;
	}

	close(usbmon_common.fd);
	usbmon_common.fd = -1;

	mutexUnlock(usbmon_common.lock);

	return 0;
}


void usbmon_submit(usb_transfer_t *t, usb_pipe_t *pipe)
{
	const void *data = NULL;
	size_t dataLen = 0;
	int interval;
	uint16_t busnum;
	uint8_t epnum, devnum;

	if (usbmon_common.fd < 0) {
		return;
	}

	epnum = (uint8_t)(pipe->num & 0x7f);
	if (t->direction == usb_dir_in) {
		epnum |= 0x80;
	}

	devnum = (uint8_t)(pipe->dev->address & 0x7f);
	busnum = (uint16_t)(pipe->dev->hcd->num & 0xffff);

	interval = 0;
	if ((t->type == usb_transfer_interrupt) || (t->type == usb_transfer_isochronous)) {
		interval = pipe->interval;
	}

	/* For OUT transfers, capture the outgoing data */
	if ((t->direction == usb_dir_out) && (t->size > 0) && (t->buffer != NULL)) {
		data = t->buffer;
		dataLen = t->size;
	}

	mutexLock(usbmon_common.lock);

	if (usbmon_common.fd >= 0) {
		usbmon_track((uintptr_t)t, epnum, devnum, busnum, interval);
		usbmon_writeEvent('S', t, epnum, devnum, busnum, interval, data, dataLen);
	}

	mutexUnlock(usbmon_common.lock);
}


void usbmon_complete(usb_transfer_t *t)
{
	usbmon_entry_t *e;
	const void *data = NULL;
	size_t dataLen = 0;
	int interval = 0;
	uint16_t busnum = 0;
	uint8_t epnum, devnum = 0;

	if (usbmon_common.fd < 0) {
		return;
	}

	/* Fallback: use direction from transfer, endpoint 0 */
	epnum = (t->direction == usb_dir_in) ? 0x80 : 0x00;

	mutexLock(usbmon_common.lock);

	if (usbmon_common.fd >= 0) {
		e = usbmon_lookup((uintptr_t)t);
		if (e != NULL) {
			epnum = e->epnum;
			devnum = e->devnum;
			busnum = e->busnum;
			interval = e->interval;
			e->id = 0; /* Free tracking slot */
		}

		/* For IN transfers, capture the received data */
		if ((t->direction == usb_dir_in) && (t->transferred > 0) && (t->buffer != NULL)) {
			data = t->buffer;
			dataLen = t->transferred;
		}

		usbmon_writeEvent('C', t, epnum, devnum, busnum, interval, data, dataLen);
	}

	mutexUnlock(usbmon_common.lock);
}
