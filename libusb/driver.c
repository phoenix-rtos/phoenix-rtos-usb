/*
 * Phoenix-RTOS
 *
 * libusb/driver.c
 *
 * Copyright 2021 Phoenix Systems
 * Author: Maciej Purski
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include <errno.h>
#include <usbdriver.h>
#include <sys/msg.h>
#include <sys/mman.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>


static struct {
	unsigned port;
} usbdrv_common;


int usbdrv_connect(const usbdrv_devid_t *filters, int nfilters, unsigned drvport)
{
	msg_t msg = { 0 };
	usbdrv_in_msg_t *umsg = (usbdrv_in_msg_t *)&msg.i.raw;
	oid_t oid;

	while (lookup("/dev/usb", NULL, &oid) < 0)
		usleep(1000000);

	msg.type = mtDevCtl;
	msg.i.size = sizeof(*filters) * nfilters;
	msg.i.data = (void *)filters;

	umsg->type = usbdrv_msg_connect;
	umsg->connect.port = drvport;
	umsg->connect.nfilters = nfilters;

	if (msgSend(oid.port, &msg) < 0)
		return -1;

	usbdrv_common.port = oid.port;

	return oid.port;
}


int usbdrv_eventsWait(int port, msg_t *msg)
{
	unsigned long rid;
	int err;

	do {
		err = msgRecv(port, msg, &rid);
		if (err < 0 && err != -EINTR)
			return -1;
	} while (err == -EINTR);

	if (msgRespond(port, msg, rid) < 0)
		return -1;

	return 0;
}


static void usbdrv_freeMsg(addr_t physaddr, size_t size)
{
	msg_t msg = { 0 };
	usbdrv_in_msg_t *imsg = (usbdrv_in_msg_t *)msg.i.raw;

	msg.type = mtDevCtl;
	imsg->type = usbdrv_msg_free;
	imsg->free.physaddr = physaddr;
	imsg->free.size = size;
	msgSend(usbdrv_common.port, &msg);
}


void usbdrv_free(void *ptr, size_t size)
{
	usbdrv_freeMsg(va2pa(ptr), size);

	/* size should be a multiple of a page size */
	size = (size + (_PAGE_SIZE - 1)) & ~(_PAGE_SIZE - 1);

	/* address must be aligned to a page size */
	ptr = (void *)((addr_t)ptr & ~(_PAGE_SIZE - 1));

	munmap(ptr, size);
}


void *usbdrv_alloc(size_t size)
{
	msg_t msg = { 0 };
	usbdrv_in_msg_t *imsg = (usbdrv_in_msg_t *)msg.i.raw;
	usbdrv_out_msg_t *omsg = (usbdrv_out_msg_t *)msg.o.raw;
	addr_t physaddr, offset;
	void *vaddr;
	int ret;

	msg.type = mtDevCtl;
	imsg->type = usbdrv_msg_alloc;
	imsg->alloc.size = size;

	if ((ret = msgSend(usbdrv_common.port, &msg)) != 0) {
		return NULL;
	}

	if (omsg->err != 0) {
		return NULL;
	}

	physaddr = omsg->alloc.physaddr;
	offset = physaddr % _PAGE_SIZE;

	/* size should be a multiple of a page size */
	size = (size + (_PAGE_SIZE - 1)) & ~(_PAGE_SIZE - 1);

	/* physical address must be aligned to a page size */
	physaddr -= offset;

	/* TODO: Check if we have already mapped this page */
	vaddr = mmap(NULL, size, PROT_WRITE | PROT_READ, MAP_UNCACHED, OID_PHYSMEM, physaddr);
	if (vaddr == MAP_FAILED) {
		usbdrv_freeMsg(omsg->alloc.physaddr, size);
		return NULL;
	}

	vaddr = (void *)((addr_t)vaddr + offset);

	return vaddr;
}


static void usbdrv_pipeCloseMsg(int pipeid)
{

}


void usbdrv_pipeClose(usbdrv_pipe_t *pipe)
{
	free(pipe);
	/* TODO: Send close message */
}


usbdrv_pipe_t *usbdrv_pipeOpen(usbdrv_dev_t *dev, usb_transfer_type_t type, usb_dir_t dir)
{
	msg_t msg = { 0 };
	usbdrv_in_msg_t *imsg = (usbdrv_in_msg_t *)msg.i.raw;
	usbdrv_out_msg_t *omsg = (usbdrv_out_msg_t *)msg.o.raw;
	usbdrv_pipe_t *pipe;
	int ret;

	msg.type = mtDevCtl;
	imsg->type = usbdrv_msg_open;

	imsg->open.bus = dev->bus;
	imsg->open.dev = dev->dev;
	imsg->open.iface = dev->interface;
	imsg->open.type = type;
	imsg->open.dir = dir;
	imsg->open.locationID = dev->locationID;

	ret = msgSend(usbdrv_common.port, &msg);
	if (ret != 0 || omsg->err < 0) {
		return NULL;
	}

	pipe = malloc(sizeof(usbdrv_pipe_t));
	if (pipe == NULL) {
		usbdrv_pipeCloseMsg(omsg->open.id);
		return NULL;
	}

	pipe->bus = dev->bus;
	pipe->dev = dev->dev;
	pipe->iface = dev->interface;
	pipe->locationID = dev->locationID;
	pipe->id = omsg->open.id;

	return pipe;
}


static int usbdrv_urbSubmitSync(usbdrv_urb_t *urb, void *data)
{
	msg_t msg = { 0 };
	usbdrv_in_msg_t *umsg = (usbdrv_in_msg_t *)msg.i.raw;
	int ret;

	msg.type = mtDevCtl;
	umsg->type = usbdrv_msg_urb;

	memcpy(&umsg->urb, urb, sizeof(usbdrv_urb_t));

	if (urb->dir == usb_dir_out) {
		msg.i.data = data;
		msg.i.size = urb->size;
	}
	else {
		msg.o.data = data;
		msg.o.size = urb->size;
	}

	if ((ret = msgSend(usbdrv_common.port, &msg)) != 0)
		return ret;

	return msg.o.io.err;
}


int usbdrv_transferControl(usbdrv_pipe_t *pipe, usb_setup_packet_t *setup, void *data, size_t size, usb_dir_t dir)
{
	usbdrv_urb_t urb = {
		.pipe = pipe->id,
		.setup = *setup,
		.dir = dir,
		.size = size,
		.type = usb_transfer_control,
		.sync = 1
	};

	return usbdrv_urbSubmitSync(&urb, data);
}


int usbdrv_transferBulk(usbdrv_pipe_t *pipe, void *data, size_t size, usb_dir_t dir)
{
	usbdrv_urb_t urb = {
		.pipe = pipe->id,
		.dir = dir,
		.size = size,
		.type = usb_transfer_bulk,
		.sync = 1
	};

	return usbdrv_urbSubmitSync(&urb, data);
}


int usbdrv_setConfiguration(usbdrv_pipe_t *pipe, int conf)
{
	usb_setup_packet_t setup = (usb_setup_packet_t) {
		.bmRequestType = REQUEST_DIR_HOST2DEV | REQUEST_TYPE_STANDARD | REQUEST_RECIPIENT_DEVICE,
		.bRequest = REQ_SET_CONFIGURATION,
		.wValue = conf,
		.wIndex = 0,
		.wLength = 0,
	};

	return usbdrv_transferControl(pipe, &setup, NULL, 0, usb_dir_out);
}


int usbdrv_clearFeatureHalt(usbdrv_pipe_t *pipe, int ep)
{
	usb_setup_packet_t setup = (usb_setup_packet_t) {
		.bmRequestType = REQUEST_DIR_HOST2DEV | REQUEST_TYPE_STANDARD | REQUEST_RECIPIENT_DEVICE,
		.bRequest = REQ_CLEAR_FEATURE,
		.wValue = USB_ENDPOINT_HALT,
		.wIndex = ep,
		.wLength = 0,
	};

	return usbdrv_transferControl(pipe, &setup, NULL, 0, usb_dir_out);
}


int usbdrv_urbAlloc(usbdrv_pipe_t *pipe, void *data, usb_dir_t dir, size_t size, int type)
{
	msg_t msg = { 0 };
	usbdrv_in_msg_t *umsg = (usbdrv_in_msg_t *)msg.i.raw;
	int ret;
	usbdrv_urb_t *urb = &umsg->urb;

	urb->pipe = pipe->id;
	urb->type = type;
	urb->dir = dir;
	urb->size = size;
	urb->sync = 0;

	msg.type = mtDevCtl;
	umsg->type = usbdrv_msg_urb;

	if ((ret = msgSend(usbdrv_common.port, &msg)) < 0)
		return ret;

	/* URB id */
	return *(int *)msg.o.raw;
}


int usbdrv_transferAsync(usbdrv_pipe_t *pipe, unsigned urbid, size_t size, usb_setup_packet_t *setup)
{
	msg_t msg = { 0 };
	usbdrv_in_msg_t *umsg = (usbdrv_in_msg_t *)msg.i.raw;
	int ret;
	usbdrv_urbcmd_t *urbcmd = &umsg->urbcmd;

	urbcmd->pipeid = pipe->id;
	urbcmd->size = size;
	urbcmd->urbid = urbid;
	urbcmd->cmd = urbcmd_submit;

	if (setup != NULL) {
		memcpy(&urbcmd->setup, setup, sizeof(*setup));
	}
	msg.type = mtDevCtl;
	umsg->type = usbdrv_msg_urbcmd;
	if ((ret = msgSend(usbdrv_common.port, &msg)) < 0)
		return ret;

	return 0;
}


int usbdrv_urbFree(usbdrv_pipe_t *pipe, unsigned urb)
{
	msg_t msg = { 0 };
	usbdrv_in_msg_t *umsg = (usbdrv_in_msg_t *)msg.i.raw;
	int ret;
	usbdrv_urbcmd_t *urbcmd = &umsg->urbcmd;

	urbcmd->pipeid = pipe->id;
	urbcmd->urbid = urb;
	urbcmd->cmd = urbcmd_free;

	msg.type = mtDevCtl;
	umsg->type = usbdrv_msg_urbcmd;
	if ((ret = msgSend(usbdrv_common.port, &msg)) < 0)
		return ret;

	return 0;
}


const usbdrv_modeswitch_t *usbdrv_modeswitchFind(uint16_t vid, uint16_t pid, const usbdrv_modeswitch_t *modes, int nmodes)
{
	int i;

	for (i = 0; i < nmodes; i++) {
		if (vid == modes[i].vid && pid == modes[i].pid)
			return &modes[i];
	}

	return NULL;
}


void usbdrv_dumpDeviceDescriptor(FILE *stream, usb_device_desc_t *descr)
{
	fprintf(stream, "DEVICE DESCRIPTOR:\n");
	fprintf(stream, "\tbLength: %d\n", descr->bLength);
	fprintf(stream, "\tbDescriptorType: 0x%x\n", descr->bDescriptorType);
	fprintf(stream, "\tbcdUSB: %d\n", descr->bcdUSB);
	fprintf(stream, "\tbDeviceClass: %d\n", descr->bDeviceClass);
	fprintf(stream, "\tbDeviceSubClass: %d\n", descr->bDeviceSubClass);
	fprintf(stream, "\tbDeviceProtocol: %d\n", descr->bDeviceProtocol);
	fprintf(stream, "\tbMaxPacketSize0: %d\n", descr->bMaxPacketSize0);
	fprintf(stream, "\tidVendor: 0x%x\n", descr->idVendor);
	fprintf(stream, "\tidProduct: 0x%x\n", descr->idProduct);
	fprintf(stream, "\tbcdDevice: %d\n", descr->bcdDevice);
	fprintf(stream, "\tiManufacturer: %d\n", descr->iManufacturer);
	fprintf(stream, "\tiProduct: %d\n", descr->iProduct);
	fprintf(stream, "\tiSerialNumber: %d\n", descr->iSerialNumber);
	fprintf(stream, "\tbNumConfigurations: %d\n", descr->bNumConfigurations);
}


void usbdrv_dumpConfigurationDescriptor(FILE *stream, usb_configuration_desc_t *descr)
{
	fprintf(stream, "CONFIGURATION DESCRIPTOR:\n");
	fprintf(stream, "\tbLength: %d\n", descr->bLength);
	fprintf(stream, "\tbDescriptorType: 0x%x\n", descr->bDescriptorType);
	fprintf(stream, "\twTotalLength: %d\n", descr->wTotalLength);
	fprintf(stream, "\tbNumInterfaces: %d\n", descr->bNumInterfaces);
	fprintf(stream, "\tbConfigurationValue: %d\n", descr->bConfigurationValue);
	fprintf(stream, "\tiConfiguration %d\n", descr->iConfiguration);
	fprintf(stream, "\tbmAttributes: 0x%x\n", descr->bmAttributes);
	fprintf(stream, "\tbMaxPower: %d\n", descr->bMaxPower);
}





void usbdrv_dumpEndpointDesc(FILE *stream, usb_endpoint_desc_t *descr)
{
	fprintf(stream, "ENDPOINT DESCRIPTOR:\n");
	fprintf(stream, "\tbLength: %d\n", descr->bLength);
	fprintf(stream, "\tbDescriptorType: 0x%x\n", descr->bDescriptorType);
	fprintf(stream, "\tbEndpointAddress: %d\n", descr->bEndpointAddress);
	fprintf(stream, "\tbmAttributes: 0x%x\n", descr->bmAttributes);
	fprintf(stream, "\twMaxPacketSize: %d\n", descr->wMaxPacketSize);
	fprintf(stream, "\tbInterval: %d\n", descr->bInterval);
}


void usbdrv_dumpStringDesc(FILE *stream, usb_string_desc_t *descr)
{
	fprintf(stream, "STRING DESCRIPTOR:\n");
	fprintf(stream, "\tbLength: %d\n", descr->bLength);
	fprintf(stream, "\tbDescriptorType: 0x%x\n", descr->bDescriptorType);
	fprintf(stream, "\twData: %.*s\n", descr->bLength - 2, descr->wData);
}


int usbdrv_modeswitchHandle(usbdrv_dev_t *dev, const usbdrv_modeswitch_t *mode)
{
	char msg[sizeof(mode->msg)];
	usbdrv_pipe_t *pipeCtrl, *pipeIn, *pipeOut;

	if ((pipeCtrl = usbdrv_pipeOpen(dev, usb_transfer_control, 0)) < 0)
		return -EINVAL;

	if (usbdrv_setConfiguration(pipeCtrl, 1) != 0)
		return -EINVAL;

	if ((pipeIn = usbdrv_pipeOpen(dev, usb_transfer_bulk, usb_dir_in)) < 0)
		return -EINVAL;

	if ((pipeOut = usbdrv_pipeOpen(dev, usb_transfer_bulk, usb_dir_out)) < 0)
		return -EINVAL;

	memcpy(msg, mode->msg, sizeof(msg));
	if (usbdrv_transferBulk(pipeOut, msg, sizeof(msg), usb_dir_out) < 0)
		return -EINVAL;

	return 0;
}
