/*
 * Phoenix-RTOS
 *
 * USB library
 *
 * usb/libusb.c
 *
 * Copyright 2018 Phoenix Systems
 * Author: Kamil Amanowicz
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/threads.h>
#include <sys/msg.h>
#include <errno.h>
#include <string.h>

#include "libusb.h"

#define USB_HANDLE "/dev/usb"

#define LIBUSB_RUNNING 0x1
#define LIBUSB_CONNECTED 0x2

static struct {
	libusb_event_cb event_cb;
	handle_t cond;
	handle_t lock;
	u32 usbd_port;
	u32 port;
	int state;
} libusb_common;


static char event_loop_stack[4096] __attribute__((aligned(8)));


void libusb_event_loop(void *arg)
{
	msg_t msg;
	unsigned int rid;

	mutexLock(libusb_common.lock);
	while (libusb_common.state & LIBUSB_RUNNING) {
		mutexUnlock(libusb_common.lock);

		msgRecv(libusb_common.port, &msg, &rid);

		mutexLock(libusb_common.lock);
		if (!(libusb_common.state & LIBUSB_CONNECTED)) {
			libusb_common.state |= LIBUSB_CONNECTED;
			condSignal(libusb_common.cond);
		}

		if (libusb_common.event_cb != NULL)
			libusb_common.event_cb((usb_event_t *)&msg.i.raw, msg.i.data, msg.i.size);
		mutexUnlock(libusb_common.lock);

		msgRespond(libusb_common.port, &msg, rid);

		mutexLock(libusb_common.lock);
	}
	libusb_common.state &= ~LIBUSB_CONNECTED;
	mutexUnlock(libusb_common.lock);

	condSignal(libusb_common.cond);
	endthread();
}


int libusb_init(void)
{
	int ret = 0;
	oid_t oid;

	while (lookup(USB_HANDLE, NULL, &oid) < 0)
		usleep(1000000);

	libusb_common.usbd_port = oid.port;

	ret |= portCreate(&libusb_common.port);
	ret |= condCreate(&libusb_common.cond);
	ret |= mutexCreate(&libusb_common.lock);

	if (ret)
		return -1;

	libusb_common.state |= LIBUSB_RUNNING;

	beginthread(libusb_event_loop, 4, event_loop_stack, 4096, NULL);

	return 0;
}


int libusb_connect(usb_device_id_t *deviceId, libusb_event_cb event_cb)
{
	msg_t msg = { 0 };

	msg.type = mtDevCtl;	
	usb_msg_t *usb_msg = (usb_msg_t *)&msg.i.raw;
	usb_msg->type = usb_msg_connect;

	usb_msg->connect.port = libusb_common.port;
	memcpy(&usb_msg->connect.filter, deviceId, sizeof(usb_device_id_t));

	libusb_common.event_cb = event_cb;

	msgSend(libusb_common.usbd_port, &msg);

	mutexLock(libusb_common.lock);
	while (!(libusb_common.state & LIBUSB_CONNECTED))
		condWait(libusb_common.cond, libusb_common.lock, 0);
	mutexUnlock(libusb_common.lock);

	return 0;
}


int libusb_open(usb_open_t *open)
{
	msg_t msg = { 0 };
	int ret = 0;

	msg.type = mtDevCtl;	
	usb_msg_t *usb_msg = (usb_msg_t *)&msg.i.raw;
	usb_msg->type = usb_msg_open;

	memcpy(&usb_msg->open, open, sizeof(usb_open_t));

	ret = msgSend(libusb_common.usbd_port, &msg);
	if (ret)
		return ret;

	return msg.o.io.err;
}


int libusb_write(usb_urb_t *urb, void *data, size_t size)
{
	msg_t msg = { 0 };
	int ret = 0;	

	msg.type = mtDevCtl;	
	usb_msg_t *usb_msg = (usb_msg_t *)msg.i.raw;
	usb_msg->type = usb_msg_urb;

	memcpy(&usb_msg->urb, urb, sizeof(usb_urb_t));
	
	msg.i.data = data;
	msg.i.size = size;

	ret = msgSend(libusb_common.usbd_port, &msg);
	if (ret)
		return ret;

	return msg.o.io.err;
}


int libusb_read(usb_urb_t *urb, void *data, size_t size)
{
	msg_t msg = { 0 };
	int ret = 0;

	msg.type = mtDevCtl;	
	usb_msg_t *usb_msg = (usb_msg_t *)msg.i.raw;
	usb_msg->type = usb_msg_urb;

	memcpy(&usb_msg->urb, urb, sizeof(usb_urb_t));
	
	msg.o.data = data;
	msg.o.size = size;

	ret = msgSend(libusb_common.usbd_port, &msg);
	if (ret)
		return ret;

	return msg.o.io.err;
}


int libusb_exit(void)
{
	msg_t msg = { 0 };
	int ret = 0;

	mutexLock(libusb_common.lock);

	libusb_common.event_cb = NULL;
	libusb_common.state &= ~LIBUSB_RUNNING;
	mutexUnlock(libusb_common.lock);
	
	ret = msgSend(libusb_common.port, &msg);
	if (ret)
		return -1;

	mutexLock(libusb_common.lock);
	while(libusb_common.state & LIBUSB_CONNECTED)	
		condWait(libusb_common.cond, libusb_common.lock, 0);
	mutexUnlock(libusb_common.lock);
	
	ret |= resourceDestroy(libusb_common.cond);
	ret |= resourceDestroy(libusb_common.lock);
	
	return ret ? -1 : 0;
}

static void libusb_dumpDeviceDescriptor(FILE *stream, device_desc_t *descr)
{
	fprintf(stream, "DEVICE DESCRIPTOR:\n");
	fprintf(stream, "\tbLength: 0x%x\n", descr->bLength);
	fprintf(stream, "\tbDescriptorType: 0x%x\n", descr->bDescriptorType);
	fprintf(stream, "\tbcdUSB: 0x%x\n", descr->bcdUSB);
	fprintf(stream, "\tbDeviceClass: 0x%x\n", descr->bDeviceClass);
	fprintf(stream, "\tbDeviceSubClass: 0x%x\n", descr->bDeviceSubClass);
	fprintf(stream, "\tbDeviceProtocol: 0x%x\n", descr->bDeviceProtocol);
	fprintf(stream, "\tbMaxPacketSize0: 0x%x\n", descr->bMaxPacketSize0);
	fprintf(stream, "\tidVendor: 0x%x\n", descr->idVendor);
	fprintf(stream, "\tidProduct: 0x%x\n", descr->idProduct);
	fprintf(stream, "\tbcdDevice: 0x%x\n", descr->bcdDevice);
	fprintf(stream, "\tiManufacturer: 0x%x\n", descr->iManufacturer);
	fprintf(stream, "\tiProduct: 0x%x\n", descr->iProduct);
	fprintf(stream, "\tiSerialNumber: 0x%x\n", descr->iSerialNumber);
	fprintf(stream, "\tbNumConfigurations: 0x%x\n", descr->bNumConfigurations);
}


static void libusb_dumpConfigurationDescriptor(FILE *stream, configuration_desc_t *desc)
{
	fprintf(stream, "CONFIGURATION DESCRIPTOR:\n");
	fprintf(stream, "\tbLength: 0x%x\n", desc->bLength);
	fprintf(stream, "\tbDescriptorType: 0x%x\n", desc->bDescriptorType);
	fprintf(stream, "\twTotalLength: 0x%x\n", desc->wTotalLength);
	fprintf(stream, "\tbNumInterfaces: 0x%x\n", desc->bNumInterfaces);
	fprintf(stream, "\tbConfigurationValue: 0x%x\n", desc->bConfigurationValue);
	fprintf(stream, "\tiConfiguration: 0x%x\n", desc->iConfiguration);
	fprintf(stream, "\tbmAttributes: 0x%x\n", desc->bmAttributes);
	fprintf(stream, "\tbMaxPower: 0x%x\n", desc->bMaxPower);
}


static void libusb_dumpInterfaceDescriptor(FILE *stream, interface_desc_t *desc)
{
	fprintf(stream, "INTERFACE DESCRIPTOR:\n");
	fprintf(stream, "\tbLength: 0x%x\n", desc->bLength);
	fprintf(stream, "\tbDescriptorType: 0x%x\n", desc->bDescriptorType);
	fprintf(stream, "\tbInterfaceNumber: 0x%x\n", desc->bInterfaceNumber);
	fprintf(stream, "\tbAlternateSetting: 0x%x\n", desc->bAlternateSetting);
	fprintf(stream, "\tbNumEndpoints: 0x%x\n", desc->bNumEndpoints);
	fprintf(stream, "\tbInterfaceClass: 0x%x\n", desc->bInterfaceClass);
	fprintf(stream, "\tbInterfaceSubClass: 0x%x\n", desc->bInterfaceSubClass);
	fprintf(stream, "\tbInterfaceProtocol: 0x%x\n", desc->bInterfaceProtocol);
	fprintf(stream, "\tiInterface: 0x%x\n", desc->iInterface);
}


static void libusb_dumpEndpointDescriptor(FILE *stream, endpoint_desc_t *desc)
{
	fprintf(stream, "ENDPOINT DESCRIPTOR:\n");
	fprintf(stream, "\tbLength: 0x%x\n", desc->bLength);
	fprintf(stream, "\tbDescriptorType: 0x%x\n", desc->bDescriptorType);
	fprintf(stream, "\tbEndpointAddress: 0x%x\n", desc->bEndpointAddress);
	fprintf(stream, "\tbmAttributes: 0x%x\n", desc->bmAttributes);
	fprintf(stream, "\twMaxPacketSize: 0x%x\n", desc->wMaxPacketSize);
	fprintf(stream, "\tbInterval: 0x%x\n", desc->bInterval);
}


static void libusb_dumpInterfaceAssociationDescriptor(FILE *stream, interface_association_desc_t *desc)
{
	fprintf(stream, "INTERFACE ASSOCIATION DESCRIPTOR:\n");
	fprintf(stream, "\tbLength: 0x%x\n", desc->bLength);
	fprintf(stream, "\tbDescriptorType: 0x%x\n", desc->bDescriptorType);
	fprintf(stream, "\tbFirstInterfacce: 0x%x\n", desc->bFirstInterface);
	fprintf(stream, "\tbInterfaceCount: 0x%x\n", desc->bInterfaceCount);
	fprintf(stream, "\tbFunctionClass: 0x%x\n", desc->bFunctionClass);
	fprintf(stream, "\tbFunctionSubClass: 0x%x\n", desc->bFunctionSubClass);
	fprintf(stream, "\tbFunctionProtocol: 0x%x\n", desc->bFunctionProtocol);
	fprintf(stream, "\tiFunction: 0x%x\n", desc->iFunction);
}

static void libusb_dumpFunctionalDescriptor(FILE *stream, functional_desc_t *desc)
{
	fprintf(stream, "CLASS SPECIFIC %s FUNCTIONAL DESCRIPTOR:\n", desc->bDescriptorType == DESC_CS_INTERFACE ? "INTERFACE" : "ENDPOINT");
	fprintf(stream, "\tbFunctionLength: 0x%x\n", desc->bFunctionLength);
	fprintf(stream, "\tbDescriptorType: 0x%x\n", desc->bDescriptorType);
	fprintf(stream, "\tbDescriptorSubtype: 0x%x\n", desc->bDescriptorSubtype);
}

static void libusb_dumpDescriptor(FILE *stream, struct desc_header *desc)
{
	switch (desc->bDescriptorType) {
	case DESC_CONFIG:
		libusb_dumpConfigurationDescriptor(stream, (void *)desc);
		break;

	case DESC_DEVICE:
		libusb_dumpDeviceDescriptor(stream, (void *)desc);
		break;

	case DESC_INTERFACE:
		libusb_dumpInterfaceDescriptor(stream, (void *)desc);
		break;

	case DESC_ENDPOINT:
		libusb_dumpEndpointDescriptor(stream, (void *)desc);
		break;

	case DESC_INTERFACE_ASSOCIATION:
		libusb_dumpInterfaceAssociationDescriptor(stream, (void *)desc);
		break;

	case DESC_CS_INTERFACE:
	case DESC_CS_ENDPOINT:
		libusb_dumpFunctionalDescriptor(stream, (void *)desc);
		break;

	default:
		printf("UNRECOGNIZED DESCRIPTOR (0x%x)\n", desc->bDescriptorType);
		break;
	}
}


void libusb_dumpConfiguration(FILE *stream, configuration_desc_t *desc)
{
	int remaining_size = desc->wTotalLength;
	struct desc_header *header = (void *)desc;

	while (remaining_size > 0) {
		libusb_dumpDescriptor(stream, header);

		if (!header->bLength)
			break;

		remaining_size -= header->bLength;
		header = (struct desc_header *)((char *)header + header->bLength);
	}
}
