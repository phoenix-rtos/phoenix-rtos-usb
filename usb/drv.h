/*
 * Phoenix-RTOS
 *
 * USB Host Driver
 *
 * Copyright 2021, 2024 Phoenix Systems
 * Author: Maciej Purski, Adam Greloch
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */


#ifndef _USB_DRV_H_
#define _USB_DRV_H_

#include <usbdriver.h>
#include <stdbool.h>
#include "dev.h"
#include "hcd.h"


#define PORT_INTERNAL (-1)


typedef struct usb_drvpriv {
	struct usb_drvpriv *next, *prev;

	usb_drvType_t type;
	union {
		struct {
			handle_t transferLock;
			handle_t finishedCond;
		} intrn;

		struct {
			int id;
			unsigned port;
		} extrn;
	};

	usb_driver_t driver;
	idtree_t pipes;
	idtree_t urbs;
} usb_drvpriv_t;


usb_drvpriv_t *usb_drvFind(int id);


int usb_libDrvInit(usb_driver_t *drv);


void usb_libDrvDestroy(usb_driver_t *drv);


void usb_drvAdd(usb_drvpriv_t *drv);


int usb_drvBind(usb_dev_t *dev, usb_event_insertion_t *event, int *iface);


int usb_drvUnbind(usb_drvpriv_t *drv, usb_dev_t *dev, int iface);


int usb_drvInit(void);


int usb_drvPipeOpen(usb_drvpriv_t *drv, hcd_t *hcd, int locationID, int iface, int dir, int type);


usb_pipe_t *usb_pipeOpen(usb_dev_t *dev, int iface, int dir, int type);


void usb_drvPipeFree(usb_drvpriv_t *drv, usb_pipe_t *pipe);


int usb_drvTransfer(usb_drvpriv_t *drv, usb_transfer_t *t, int pipeId);


int usb_urbAllocHandle(msg_t *msg, unsigned int port, unsigned long rid);


int usb_drvTransferAsync(usb_drvpriv_t *drv, int urbid, int pipeid);


int usb_handleUrbcmd(msg_t *msg);


int usb_handleUrb(msg_t *msg, unsigned int port, unsigned long rid);

#endif /* _USB_DRV_H_ */
