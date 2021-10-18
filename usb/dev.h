/*
 * Phoenix-RTOS
 *
 * USB Host Device
 *
 * Copyright 2021 Phoenix Systems
 * Author: Maciej Purski
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */


#ifndef _USB_DEV_H_
#define _USB_DEV_H_

#include <usb.h>

#include "usbhost.h"


enum usb_speed { usb_full_speed = 0, usb_low_speed, usb_high_speed };

typedef struct {
	usb_interface_desc_t *desc;
	usb_endpoint_desc_t *eps;
	void *classDesc;
	char *str;

	struct _usb_drv *driver;
} usb_iface_t;


typedef struct _usb_dev {
	enum usb_speed speed;
	usb_device_desc_t desc;
	usb_configuration_desc_t *conf;
	char *manufacturer;
	char *product;
	char *serialNumber;
	uint16_t langId;

	int address;
	uint32_t locationID;
	usb_iface_t *ifs;
	int nifs;
	usb_pipe_t *ctrlPipe;

	struct hcd *hcd;
	struct _usb_dev *hub;
	int port;

	/* Hub fields */
	struct _usb_dev **devs;
	struct usb_transfer *statusTransfer;
	int nports;
	struct _usb_dev *prev, *next;
} usb_dev_t;


int usb_devCtrl(usb_dev_t *dev, usb_dir_t dir, usb_setup_packet_t *setup, char *buf, size_t len);


void usb_devCtrlFinished(usb_transfer_t *t);


usb_dev_t *usb_devAlloc(void);


void usb_devFree(usb_dev_t *dev);


int usb_devEnumerate(usb_dev_t *dev);


void usb_devDisconnected(usb_dev_t *dev);


int usb_devInit(void);

#endif /* _USB_DEV_H_ */
