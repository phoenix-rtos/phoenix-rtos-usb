/*
 * Phoenix-RTOS
 *
 * USB Host
 *
 * Copyright 2021 Phoenix Systems
 * Author: Maciej Purski
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#ifndef _USB_HOST_H_
#define _USB_HOST_H_

#include <sys/types.h>
#include <sys/msg.h>
#include <usbdriver.h>
#include <usb.h>


typedef struct usb_pipe {
	idnode_t linkage;
	struct usb_driver *drv;
	struct usb_endpoint *ep;
} usb_pipe_t;


typedef struct usb_transfer {
	struct usb_transfer *next, *prev;
	struct usb_endpoint *ep;
	usb_setup_packet_t *setup;

	unsigned async;
	unsigned id;
	volatile int finished;
	volatile int error;
	volatile int aborted;

	char *buffer;
	size_t size;
	size_t transferred;
	int type;
	int direction;

	unsigned long rid;
	msg_t *msg;
	unsigned int port;

	void (*handler)(struct usb_transfer *);

	void *hcdpriv;
} usb_transfer_t;

void usb_pipeFree(usb_pipe_t *pipe);


void *usb_alloc(size_t size);


void usb_free(void *addr, size_t size);


void *usb_allocAligned(size_t size, size_t alignment);


void usb_freeAligned(void *addr, size_t size);

#endif
