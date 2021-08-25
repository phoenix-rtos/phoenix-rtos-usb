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

enum usb_speed { usb_full_speed = 0,
	usb_low_speed,
	usb_high_speed };

typedef struct usb_endpoint {
	struct usb_endpoint *prev, *next;

	usb_transfer_type_t type;
	usb_dir_t direction;

	int maxPacketLen;
	int number;
	int interval;
	int interface;
	struct usb_pipe *pipe;

	struct usb_dev *device;
	void *hcdpriv;
} usb_endpoint_t;


typedef struct usb_iface {
	usb_interface_desc_t *desc;
	void *classDesc;
	char *str;

	struct usb_driver *driver;
} usb_iface_t;


typedef struct usb_dev {
	enum usb_speed speed;
	usb_device_desc_t desc;
	usb_configuration_desc_t *conf;
	char *manufacturer;
	char *product;
	char *serialNumber;
	uint16_t langId;

	int address;
	uint32_t locationID;
	struct usb_iface *ifs;
	int nifs;
	usb_endpoint_t *eps;

	struct hcd *hcd;
	struct usb_dev *hub;
	int port;

	/* Hub fields */
	struct usb_dev **devs;
	struct usb_transfer *statusTransfer;
	int nports;
	struct usb_dev *prev, *next;
} usb_dev_t;


int usb_controlTransferSync(usb_dev_t *dev, usb_dir_t dir, usb_setup_packet_t *setup, char *buf, size_t len);

usb_dev_t *usb_devAlloc(void);

void usb_devFree(usb_dev_t *dev);

int usb_devEnumerate(usb_dev_t *dev);

void usb_devDisconnected(usb_dev_t *dev);

int usb_devInit(void);

#endif /* _USB_DEV_H_ */
