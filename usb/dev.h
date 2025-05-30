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
#include <stdbool.h>

#include "usbhost.h"


enum usb_speed { usb_full_speed = 0,
	usb_low_speed,
	usb_high_speed };


typedef struct {
	unsigned int len;
	char *str;
} usb_lenStr_t;


typedef struct {
	usb_interface_desc_t *desc;
	usb_endpoint_desc_t *eps;
	void *classDesc;
	usb_lenStr_t name;

	struct usb_drvpriv *driver;
} usb_iface_t;


typedef struct usb_dev_oid {
	struct usb_dev_oid *next, *prev;
	oid_t oid;
} usb_dev_oids_t;


typedef struct _usb_dev {
	struct _usb_dev *next, *prev;

	enum usb_speed speed;
	usb_device_desc_t desc;
	usb_configuration_desc_t *conf;

	usb_lenStr_t manufacturer;
	usb_lenStr_t product;
	usb_lenStr_t serialNumber;
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
	usb_transfer_t *statusTransfer;
	usb_pipe_t *irqPipe;
	int nports;
} usb_dev_t;


usb_dev_t *usb_devFind(usb_dev_t *hub, int locationID);


int usb_devCtrl(usb_dev_t *dev, usb_dir_t dir, usb_setup_packet_t *setup, char *buf, size_t len);


usb_dev_t *usb_devAlloc(void);


int usb_devEnumerate(usb_dev_t *dev);


void usb_devDisconnected(usb_dev_t *dev, bool silent);


int usb_devInit(void);


void usb_devSetChild(usb_dev_t *parent, int port, usb_dev_t *child);


int usb_isRoothub(usb_dev_t *dev);


void usb_devSignal(void);


int usb_devFindDescFromOid(oid_t oid, usb_devinfo_desc_t *desc);


#endif /* _USB_DEV_H_ */
