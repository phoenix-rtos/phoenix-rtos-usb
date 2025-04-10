/*
 * Phoenix-RTOS
 *
 * USB Host
 *
 * Copyright 2021, 2024 Phoenix Systems
 * Author: Maciej Purski, Adam Greloch
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#ifndef _USB_HOST_H_
#define _USB_HOST_H_

#include <sys/types.h>
#include <usbdriver.h>
#include <usb.h>


enum { urb_idle, urb_completed, urb_ongoing };

typedef struct {
	idnode_t linkage;
	struct usb_drvpriv *drv;

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


typedef struct usb_transfer_t usb_transfer_t;


typedef struct {
	void (*urbSyncCompleted)(usb_transfer_t *t);
	void (*urbAsyncCompleted)(usb_transfer_t *t);
} usb_transferOps_t;


const usb_transferOps_t *usblibdrv_transferOpsGet(void);


const usb_transferOps_t *usbprocdrv_transferOpsGet(void);


/* Used to handle both internal and external transfers */
struct usb_transfer_t {
	usb_transfer_t *next, *prev;
	usb_setup_packet_t *setup;

	unsigned async;
	volatile bool finished;
	volatile int error;

	char *buffer;
	size_t size;
	size_t transferred;
	int type;
	int direction;
	int pipeid;

	usb_drvType_t recipient;

	/* Used only for URBs handling (recipient other than hcd) */
	idnode_t linkage;
	int state;
	int refcnt;
	union {
		struct {
			size_t osize;
			void *odata;

			unsigned long rid;
			unsigned int port;
			pid_t pid;
		} extrn;
		struct {
			handle_t *finishedCond;
			usb_driver_t *drv;
		} intrn;
	};

	struct _usb_dev *hub;

	void *hcdpriv;

	/* URB transfer on completion callbacks */
	const usb_transferOps_t *ops;
};


int usb_memInit(void);


void usb_pipeFree(usb_pipe_t *pipe);


void *usb_alloc(size_t size);


void usb_free(void *addr, size_t size);


void *usb_allocAligned(size_t size, size_t alignment);


void usb_freeAligned(void *addr, size_t size);


void usb_transferFinished(usb_transfer_t *t, int status);


bool usb_transferCheck(usb_transfer_t *t);


int usb_transferSubmit(usb_transfer_t *t, usb_pipe_t *pipe, handle_t *cond);


void usb_transferFree(usb_transfer_t *t);


void usb_transferPut(usb_transfer_t *t);


int usblibdrv_open(usb_driver_t *drv, usb_devinfo_t *dev, usb_transfer_type_t type, usb_dir_t dir);


#endif
