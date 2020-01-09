/*
 * Phoenix-RTOS
 *
 * USB Host Proxy
 *
 * host/hostproxy.h
 *
 * Copyright 2018, 2020 Phoenix Systems
 * Author: Kamil Amanowicz, Hubert Buczynski
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/threads.h>
#include <sys/msg.h>
#include <errno.h>
#include <string.h>

#include "hostproxy.h"

#define USB_HANDLE "/dev/usb"

#define HOSTPROXY_RUNNING 0x1
#define HOSTPROXY_CONNECTED 0x2

static struct {
	hostproxy_event_cb event_cb;
	handle_t cond;
	handle_t lock;
	uint32_t hostsrv_port;
	uint32_t port;
	int state;
} hostproxy_common;


static char event_loop_stack[4096] __attribute__((aligned(8)));


void hostproxy_event_loop(void *arg)
{
	msg_t msg;
	unsigned int rid;

	mutexLock(hostproxy_common.lock);
	while (hostproxy_common.state & HOSTPROXY_RUNNING) {
		mutexUnlock(hostproxy_common.lock);

		msgRecv(hostproxy_common.port, &msg, &rid);

		mutexLock(hostproxy_common.lock);
		if (!(hostproxy_common.state & HOSTPROXY_CONNECTED)) {
			hostproxy_common.state |= HOSTPROXY_CONNECTED;
			condSignal(hostproxy_common.cond);
		}

		if (hostproxy_common.event_cb != NULL)
			hostproxy_common.event_cb((usb_event_t *)&msg.i.raw, msg.i.data, msg.i.size);

		msgRespond(hostproxy_common.port, &msg, rid);
	}
	hostproxy_common.state &= ~HOSTPROXY_CONNECTED;
	mutexUnlock(hostproxy_common.lock);

	condSignal(hostproxy_common.cond);
	endthread();
}


int hostproxy_init(void)
{
	int ret = 0;
	oid_t oid;

	while (lookup(USB_HANDLE, NULL, &oid) < 0)
		usleep(1000000);

	hostproxy_common.hostsrv_port = oid.port;

	ret |= portCreate(&hostproxy_common.port);
	ret |= condCreate(&hostproxy_common.cond);
	ret |= mutexCreate(&hostproxy_common.lock);

	if (ret)
		return -1;

	hostproxy_common.state |= HOSTPROXY_RUNNING;

	beginthread(hostproxy_event_loop, 4, event_loop_stack, 4096, NULL);

	return 0;
}


int hostproxy_connect(usb_device_id_t *deviceId, hostproxy_event_cb event_cb)
{
	msg_t msg = { 0 };

	msg.type = mtDevCtl;
	usb_msg_t *usb_msg = (usb_msg_t *)&msg.i.raw;
	usb_msg->type = usb_msg_connect;

	usb_msg->connect.port = hostproxy_common.port;
	memcpy(&usb_msg->connect.filter, deviceId, sizeof(usb_device_id_t));

	hostproxy_common.event_cb = event_cb;

	msgSend(hostproxy_common.hostsrv_port, &msg);

	return 0;
}


int hostproxy_open(usb_open_t *open)
{
	msg_t msg = { 0 };
	int ret = 0;

	msg.type = mtDevCtl;
	usb_msg_t *usb_msg = (usb_msg_t *)&msg.i.raw;
	usb_msg->type = usb_msg_open;

	memcpy(&usb_msg->open, open, sizeof(usb_open_t));

	ret = msgSend(hostproxy_common.hostsrv_port, &msg);
	if (ret)
		return ret;

	return msg.o.io.err;
}


int hostproxy_open_(int device, usb_endpoint_desc_t endpoint)
{
	msg_t msg = { 0 };
	int ret = 0;

	msg.type = mtDevCtl;
	usb_msg_t *usb_msg = (usb_msg_t *)&msg.i.raw;
	usb_msg->type = usb_msg_open;
	usb_msg->open.device_id = device;
	usb_msg->open.endpoint = endpoint;

	if ((ret = msgSend(hostproxy_common.hostsrv_port, &msg)))
		return ret;

	return msg.o.io.err;
}



int hostproxy_write(usb_urb_t *urb, void *data, size_t size)
{
	msg_t msg = { 0 };
	int ret = 0;

	msg.type = mtDevCtl;
	usb_msg_t *usb_msg = (usb_msg_t *)msg.i.raw;
	usb_msg->type = usb_msg_urb;
	urb->transfer_size = size;
	urb->direction = usb_transfer_out;

	memcpy(&usb_msg->urb, urb, sizeof(usb_urb_t));

	msg.i.data = data;
	msg.i.size = size;

	ret = msgSend(hostproxy_common.hostsrv_port, &msg);
	if (ret)
		return ret;

	return msg.o.io.err;
}


int hostproxy_read(usb_urb_t *urb, void *data, size_t size)
{
	msg_t msg = { 0 };
	int ret = 0;

	msg.type = mtDevCtl;
	usb_msg_t *usb_msg = (usb_msg_t *)msg.i.raw;
	usb_msg->type = usb_msg_urb;
	urb->direction = usb_transfer_in;

	if (!urb->async)
		urb->transfer_size = size;

	memcpy(&usb_msg->urb, urb, sizeof(usb_urb_t));

	msg.o.data = data;
	msg.o.size = size;

	ret = msgSend(hostproxy_common.hostsrv_port, &msg);
	if (ret)
		return ret;

	return msg.o.io.err;
}


int hostproxy_reset(int deviceId)
{
	msg_t msg = { 0 };
	int ret = 0;

	msg.type = mtDevCtl;
	usb_msg_t *usb_msg = (usb_msg_t *)msg.i.raw;
	usb_msg->type = usb_msg_reset;
	usb_msg->reset.device_id = deviceId;

	ret = msgSend(hostproxy_common.hostsrv_port, &msg);

	if (ret)
		return ret;

	return msg.o.io.err;
}


int hostproxy_exit(void)
{
	msg_t msg = { 0 };
	int ret = 0;

	mutexLock(hostproxy_common.lock);

	hostproxy_common.event_cb = NULL;
	hostproxy_common.state &= ~HOSTPROXY_RUNNING;
	mutexUnlock(hostproxy_common.lock);

	ret = msgSend(hostproxy_common.port, &msg);
	if (ret)
		return -1;

	mutexLock(hostproxy_common.lock);
	while(hostproxy_common.state & HOSTPROXY_CONNECTED)
		condWait(hostproxy_common.cond, hostproxy_common.lock, 0);
	mutexUnlock(hostproxy_common.lock);

	ret |= resourceDestroy(hostproxy_common.cond);
	ret |= resourceDestroy(hostproxy_common.lock);

	return ret ? -1 : 0;
}

static void hostproxy_dumpDeviceDescriptor(FILE *stream, usb_device_desc_t *descr)
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


static void hostproxy_dumpConfigurationDescriptor(FILE *stream, usb_configuration_desc_t *desc)
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


static void hostproxy_dumpInterfaceDescriptor(FILE *stream, usb_interface_desc_t *desc)
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


static void hostproxy_dumpEndpointDescriptor(FILE *stream, usb_endpoint_desc_t *desc)
{
	fprintf(stream, "ENDPOINT DESCRIPTOR:\n");
	fprintf(stream, "\tbLength: 0x%x\n", desc->bLength);
	fprintf(stream, "\tbDescriptorType: 0x%x\n", desc->bDescriptorType);
	fprintf(stream, "\tbEndpointAddress: 0x%x\n", desc->bEndpointAddress);
	fprintf(stream, "\tbmAttributes: 0x%x\n", desc->bmAttributes);
	fprintf(stream, "\twMaxPacketSize: 0x%x\n", desc->wMaxPacketSize);
	fprintf(stream, "\tbInterval: 0x%x\n", desc->bInterval);
}


static void hostproxy_dumpInterfaceAssociationDescriptor(FILE *stream, usb_interface_association_desc_t *desc)
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

static void hostproxy_dumpFunctionalDescriptor(FILE *stream, usb_functional_desc_t *desc)
{
	fprintf(stream, "CLASS SPECIFIC %s FUNCTIONAL DESCRIPTOR:\n", desc->bDescriptorType == USB_DESC_CS_INTERFACE ? "INTERFACE" : "ENDPOINT");
	fprintf(stream, "\tbFunctionLength: 0x%x\n", desc->bFunctionLength);
	fprintf(stream, "\tbDescriptorType: 0x%x\n", desc->bDescriptorType);
	fprintf(stream, "\tbDescriptorSubtype: 0x%x\n", desc->bDescriptorSubtype);
}

static void hostproxy_dumpDescriptor(FILE *stream, struct usb_desc_header *desc)
{
	switch (desc->bDescriptorType) {
	case USB_DESC_CONFIG:
		hostproxy_dumpConfigurationDescriptor(stream, (void *)desc);
		break;

	case USB_DESC_DEVICE:
		hostproxy_dumpDeviceDescriptor(stream, (void *)desc);
		break;

	case USB_DESC_INTERFACE:
		hostproxy_dumpInterfaceDescriptor(stream, (void *)desc);
		break;

	case USB_DESC_ENDPOINT:
		hostproxy_dumpEndpointDescriptor(stream, (void *)desc);
		break;

	case USB_DESC_INTERFACE_ASSOCIATION:
		hostproxy_dumpInterfaceAssociationDescriptor(stream, (void *)desc);
		break;

	case USB_DESC_CS_INTERFACE:
	case USB_DESC_CS_ENDPOINT:
		hostproxy_dumpFunctionalDescriptor(stream, (void *)desc);
		break;

	default:
		printf("UNRECOGNIZED DESCRIPTOR (0x%x)\n", desc->bDescriptorType);
		break;
	}
}


void hostproxy_dumpConfiguration(FILE *stream, usb_configuration_desc_t *desc)
{
	int remaining_size = desc->wTotalLength;
	struct usb_desc_header *header = (void *)desc;

	while (remaining_size > 0) {
		hostproxy_dumpDescriptor(stream, header);

		if (!header->bLength)
			break;

		remaining_size -= header->bLength;
		header = (struct usb_desc_header *)((char *)header + header->bLength);
	}
}
