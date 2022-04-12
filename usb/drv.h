/*
 * Phoenix-RTOS
 *
 * USB Host Driver
 *
 * Copyright 2021 Phoenix Systems
 * Author: Maciej Purski
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */


#ifndef _USB_DRV_H_
#define _USB_DRV_H_

#include <usbdriver.h>
#include "dev.h"
#include "hcd.h"

typedef struct _usb_drv {
	struct _usb_drv *next, *prev;
	pid_t pid;
	unsigned port;
	unsigned nfilters;
	usb_device_id_t *filters;
	idtree_t pipes;
	idtree_t urbs;
} usb_drv_t;


usb_drv_t *usb_drvFind(int pid);


void usb_drvAdd(usb_drv_t *drv);


int usb_drvBind(usb_dev_t *dev);


int usb_drvUnbind(usb_drv_t *drv, usb_dev_t *dev, int iface);


int usb_drvInit(void);


int usb_drvPipeOpen(usb_drv_t *drv, hcd_t *hcd, int locationID, int iface, int dir, int type);


usb_pipe_t *usb_pipeOpen(usb_dev_t *dev, int iface, int dir, int type);


void usb_drvPipeFree(usb_drv_t *drv, usb_pipe_t *pipe);


int usb_drvTransfer(usb_drv_t *drv, usb_transfer_t *t, int pipeId);


int usb_urbAllocHandle(msg_t *msg, unsigned int port, unsigned long rid);


int usb_drvTransferAsync(usb_drv_t *drv, int urbid, int pipeid);


int usb_handleUrbcmd(msg_t *msg);


int usb_handleUrb(msg_t *msg, unsigned int port, unsigned long rid);

#endif /* _USB_DRV_H_ */
