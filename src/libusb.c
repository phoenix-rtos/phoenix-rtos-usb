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

#define LIBUSB_RUN 0x1
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
	while (libusb_common.state & LIBUSB_RUN) {
		mutexUnlock(libusb_common.lock);

		msgRecv(libusb_common.port, &msg, &rid);

		mutexLock(libusb_common.lock);
		if (!(libusb_common.state & LIBUSB_CONNECTED)) {
			libusb_common.state |= LIBUSB_CONNECTED;
			condSignal(libusb_common.cond);
		}

		if (libusb_common.event_cb != NULL)
			libusb_common.event_cb((usb_event_t *)&msg.o.raw, msg.o.data, msg.o.size);
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
		sleep(1);

	libusb_common.usbd_port = oid.port;

	ret |= portCreate(&libusb_common.port);
	ret |= condCreate(&libusb_common.cond);
	ret |= mutexCreate(&libusb_common.lock);

	if (ret)
		return -1;

	libusb_common.state |= LIBUSB_RUN;

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
	usb_msg_t *usb_msg = (usb_msg_t *)&msg.i.raw;
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
	usb_msg_t *usb_msg = (usb_msg_t *)&msg.i.raw;
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
	libusb_common.state &= ~LIBUSB_RUN;
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
