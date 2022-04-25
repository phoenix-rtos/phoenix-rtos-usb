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

enum { urb_idle, urb_completed, urb_ongoing };

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


static inline int usb_pipeid(usb_pipe_t *pipe)
{
	return pipe->linkage.id;
}

/* Used to handle both internal and external transfers */
typedef struct usb_transfer {
	struct usb_transfer *next, *prev;
	usb_setup_packet_t *setup;

	unsigned async;
	volatile int finished;
	volatile int error;

	char *buffer;
	size_t size;
	size_t transferred;
	int type;
	int direction;
	int pipeid;

	/* Used only for URBs handling */
	idnode_t linkage;
	int state;
	int refcnt;
	unsigned long rid;
	unsigned int port;
	pid_t pid;

	struct _usb_dev *hub;

	void *hcdpriv;
} usb_transfer_t;


#define USB_LOG(fmt, ...) do { printf(fmt, ##__VA_ARGS__); } while (0);


int usb_memInit(void);


void usb_pipeFree(usb_pipe_t *pipe);


void *usb_alloc(size_t size);


void usb_free(void *addr, size_t size);


void *usb_allocAligned(size_t size, size_t alignment);


void usb_freeAligned(void *addr, size_t size);


void usb_transferFinished(usb_transfer_t *t, int status);


int usb_transferCheck(usb_transfer_t *t);


int usb_transferSubmit(usb_transfer_t *t, usb_pipe_t *pipe, handle_t *cond);


void usb_transferFree(usb_transfer_t *t);


void usb_transferPut(usb_transfer_t *t);

#endif
