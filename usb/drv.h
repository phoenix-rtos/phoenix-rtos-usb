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

typedef struct usb_driver {
	struct usb_driver *next, *prev;
	unsigned pid;
	unsigned port;
	unsigned nfilters;
	usb_device_id_t *filters;
} usb_driver_t;

usb_driver_t *usb_drvFind(int pid);

void usb_drvAdd(usb_driver_t *drv);

int usb_drvBind(usb_dev_t *dev);

int usb_drvUnbind(usb_dev_t *dev);

int usb_drvInit(void);

#endif /* _USB_DRV_H_ */