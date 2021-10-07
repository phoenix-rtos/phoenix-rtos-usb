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


typedef struct {
	idnode_t linkage;
	struct _usb_drv *drv;

	usb_transfer_type_t type;
	usb_dir_t dir;

	int maxPacketLen;
	int interval;
	int num;
	struct _usb_dev *dev;
	void *hcdpriv;
} usb_pipe_t;


typedef struct usb_transfer {
	struct usb_transfer *next, *prev;
	usb_pipe_t *pipe;
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

	void *hcdpriv;
} usb_transfer_t;


int usb_memInit(void);


void usb_pipeFree(usb_pipe_t *pipe);


void *usb_alloc(size_t size);


void usb_free(void *addr, size_t size);


void *usb_allocAligned(size_t size, size_t alignment);


void usb_freeAligned(void *addr, size_t size);


void usb_transferFinished(usb_transfer_t *t);

#endif
