
/*
 * Phoenix-RTOS
 *
 * Libusb driver interface
 *
 * libusb/usbdriver.h
 *
 * Copyright 2021 Phoenix Systems
 * Author: Maciej Purski
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#ifndef _USBDRIVER_H_
#define _USBDRIVER_H_

#include <usb.h>
#include <stddef.h>
#include <sys/msg.h>
#include <posix/idtree.h>

#define USBDRV_ANY ((unsigned)-1)


enum {
	usbdrv_nomatch = 0x0,
	usbdrv_match = 0x01,
	usbdrv_class_match = 0x02,
	usbdrv_subclass_match = 0x04,
	usbdrv_protocol_match = 0x08,
	usbdrv_vid_match = 0x10,
	usbdrv_pid_match = 0x20
};


typedef struct {
	unsigned vid;
	unsigned pid;
	unsigned dclass;
	unsigned subclass;
	unsigned protocol;
} usbdrv_devid_t;


typedef struct {
	unsigned port;
	unsigned nfilters;
} usbdrv_connect_t;


typedef struct {
	int pipe;
	int size;
	usb_setup_packet_t setup;
	usb_dir_t dir;
	int type;
	int sync;
} usbdrv_urb_t;


typedef struct {
	int bus;
	int dev;
	int iface;
	unsigned locationID;
	usb_transfer_type_t type;
	usb_dir_t dir;
} usbdrv_open_t;


typedef struct {
	usb_device_desc_t descriptor;
	char manufacturer[32];
	char product[32];
	char serialNumber[32];
	int bus;
	int dev;
	int interface;
	unsigned locationID;
} usbdrv_devinfo_t;


typedef struct {
	int bus;
	int dev;
	int interface;
} usbdrv_deletion_t;


typedef struct {
	int pipeid;
	int urbid;
	size_t size;
	usb_setup_packet_t setup;
	enum {
		urbcmd_submit,
		urbcmd_cancel,
		urbcmd_free } cmd;
} usbdrv_urbcmd_t;


typedef struct {
	size_t size;
} usbdrv_in_alloc_t;


typedef struct {
	addr_t physaddr;
} usbdrv_out_alloc_t;


typedef struct {
	size_t size;
	addr_t physaddr;
} usbdrv_in_free_t;


typedef struct {
	int pipeid;
	int urbid;
	size_t transferred;
	int err;
} usbdrv_completion_t;


typedef struct {
	enum {
		usbdrv_msg_alloc,
		usbdrv_msg_free,
		usbdrv_msg_connect,
		usbdrv_msg_insertion,
		usbdrv_msg_deletion,
		usbdrv_msg_urb,
		usbdrv_msg_open,
		usbdrv_msg_urbcmd,
		usbdrv_msg_completion } type;

	union {
		usbdrv_in_alloc_t alloc;
		usbdrv_in_free_t free;
		usbdrv_connect_t connect;
		usbdrv_urb_t urb;
		usbdrv_urbcmd_t urbcmd;
		usbdrv_open_t open;
		usbdrv_devinfo_t insertion;
		usbdrv_deletion_t deletion;
		usbdrv_completion_t completion;
	};
} usbdrv_msg_t;


typedef struct {
	uint16_t vid;
	uint16_t pid;
	uint8_t msg[31];
	int scsiresp;
} usbdrv_modeswitch_t;


/* Allocate memory used for usb transfers */
void *usbdrv_alloc(size_t size);

/* Frees memory used for usb transfers. The caller must ensure there is no ongoing URB using this memory chunk */
void usbdrv_free(void *ptr, size_t size);


int usbdrv_modeswitchHandle(usbdrv_devinfo_t *dev, const usbdrv_modeswitch_t *mode);


const usbdrv_modeswitch_t *usbdrv_modeswitchFind(uint16_t vid, uint16_t pid, const usbdrv_modeswitch_t *modes, int nmodes);


int usbdrv_connect(const usbdrv_devid_t *filters, int nfilters, unsigned drvport);


int usbdrv_eventsWait(int port, msg_t *msg);


int usbdrv_open(usbdrv_devinfo_t *dev, usb_transfer_type_t type, usb_dir_t dir);


int usbdrv_transferControl(unsigned pipe, usb_setup_packet_t *setup, void *data, size_t size, usb_dir_t dir);


int usbdrv_transferBulk(unsigned pipe, void *data, size_t size, usb_dir_t dir);


int usbdrv_transferAsync(unsigned pipe, unsigned urbid, size_t size, usb_setup_packet_t *setup);


int usbdrv_setConfiguration(unsigned pipe, int conf);


int usbdrv_urbAlloc(unsigned pipe, void *data, usb_dir_t dir, size_t size, int type);


int usbdrv_urbFree(unsigned pipe, unsigned urb);


int usbdrv_clearFeatureHalt(unsigned pipe, int ep);


void usbdrv_dumpDeviceDescriptor(FILE *stream, usb_device_desc_t *descr);


void usbdrv_dumpConfigurationDescriptor(FILE *stream, usb_configuration_desc_t *descr);


void usbdrv_dumpInferfaceDesc(FILE *stream, usb_interface_desc_t *descr);


void usbdrv_dumpEndpointDesc(FILE *stream, usb_endpoint_desc_t *descr);


void usbdrv_dumpStringDesc(FILE *stream, usb_string_desc_t *descr);


#endif /* _USBDRIVER_H_ */
