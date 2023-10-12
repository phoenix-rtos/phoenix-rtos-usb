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


#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/threads.h>
#include <sys/list.h>

#include <usb.h>

#include "usbhost.h"
#include "dev.h"
#include "drv.h"
#include "hcd.h"
#include "hub.h"

#define USBDEV_BUF_SIZE 0x200

struct {
	handle_t lock;
	handle_t cond;
	char *ctrlBuf;
	char *setupBuf;
} usbdev_common;


int usb_devCtrl(usb_dev_t *dev, usb_dir_t dir, usb_setup_packet_t *setup, char *buf, size_t len)
{
	usb_transfer_t t = (usb_transfer_t) {
		.type = usb_transfer_control,
		.direction = dir,
		.setup = (usb_setup_packet_t *)usbdev_common.setupBuf,
		.buffer = usbdev_common.ctrlBuf,
		.size = len,
	};
	int ret;

	if (len > USBDEV_BUF_SIZE)
		return -1;

	memcpy(usbdev_common.setupBuf, setup, sizeof(usb_setup_packet_t));
	if (dir == usb_dir_out && len > 0)
		memcpy(usbdev_common.ctrlBuf, buf, len);

	if ((ret = usb_transferSubmit(&t, dev->ctrlPipe, &usbdev_common.cond)) != 0) {
		mutexUnlock(usbdev_common.lock);
		return ret;
	}

	if (t.error == 0 && dir == usb_dir_in && len > 0)
		memcpy(buf, usbdev_common.ctrlBuf, len);

	return (t.error == 0) ? t.transferred : -t.error;
}


static int usb_getDescriptor(usb_dev_t *dev, int descriptor, int index, char *buffer, size_t len)
{
	usb_setup_packet_t setup = (usb_setup_packet_t) {
		.bmRequestType = REQUEST_DIR_DEV2HOST | REQUEST_TYPE_STANDARD | REQUEST_RECIPIENT_DEVICE,
		.bRequest = REQ_GET_DESCRIPTOR,
		.wValue = descriptor << 8 | index,
		.wIndex = (descriptor == USB_DESC_STRING) ? dev->langId : 0,
		.wLength = len,
	};

	return usb_devCtrl(dev, usb_dir_in, &setup, buffer, len);
}


static int usb_setAddress(usb_dev_t *dev, int address)
{
	usb_setup_packet_t setup = (usb_setup_packet_t) {
		.bmRequestType = REQUEST_DIR_HOST2DEV | REQUEST_TYPE_STANDARD | REQUEST_RECIPIENT_DEVICE,
		.bRequest = REQ_SET_ADDRESS,
		.wValue = address,
		.wIndex = 0,
		.wLength = 0
	};

	if (usb_devCtrl(dev, usb_dir_out, &setup, NULL, 0) < 0)
		return -1;

	dev->address = address;

	return 0;
}


usb_dev_t *usb_devAlloc(void)
{
	usb_dev_t *dev;
	usb_pipe_t *ctrlPipe;

	if ((dev = calloc(1, sizeof(usb_dev_t))) == NULL)
		return NULL;

	/* Create control endpoint */
	if ((ctrlPipe = calloc(1, sizeof(usb_pipe_t))) == NULL) {
		free(dev);
		return NULL;
	}

	ctrlPipe->maxPacketLen = 64;
	ctrlPipe->num = 0;
	ctrlPipe->dev = dev;
	ctrlPipe->type = usb_transfer_control;
	dev->ctrlPipe = ctrlPipe;

	return dev;
}


void usb_devFree(usb_dev_t *dev)
{
	int i;

	free(dev->manufacturer);
	free(dev->product);
	free(dev->serialNumber);
	free(dev->conf);

	for (i = 0; i < dev->nifs; i++)
		free(dev->ifs[i].str);

	usb_drvPipeFree(NULL, dev->ctrlPipe);
	if (dev->statusTransfer != NULL) {
		usb_drvPipeFree(NULL, dev->irqPipe);
		usb_free(dev->statusTransfer->buffer, sizeof(uint32_t));
		free(dev->statusTransfer);
	}

	free(dev->ifs);
	free(dev->devs);
	free(dev);
}


void usb_devDestroy(usb_dev_t *dev)
{
	int i;

	for (i = 0; i < dev->nports; i++) {
		if (dev->devs[i] != NULL)
			usb_devDestroy(dev->devs[i]);
	}

	if (dev->address != 0)
		hcd_addrFree(dev->hcd, dev->address);

	usb_devFree(dev);
}


static int usb_genLocationID(usb_dev_t *dev)
{
	usb_dev_t *hub = dev->hub;
	int tier = 1;

	if (usb_isRoothub(dev)) {
		dev->locationID = dev->hcd->num & 0xf;
		return 0;
	}

	/* Allocate locationID */
	dev->locationID = hub->locationID;
	while (hub->hub != NULL) {
		hub = hub->hub;
		tier++;
	}

	if (tier > 7)
		return -1;

	dev->locationID |= dev->port << (4 * tier);

	return 0;
}


static int usb_getDevDesc(usb_dev_t *dev)
{
	if (usb_getDescriptor(dev, USB_DESC_DEVICE, 0, (char *)&dev->desc, sizeof(usb_device_desc_t)) < 0)
		return -1;

	return 0;
}


static int usb_getConfiguration(usb_dev_t *dev)
{
	usb_configuration_desc_t pre, *conf;
	char *ptr;
	int size, nifs;
	int ret = 0;

	/* Get first nine bytes to get to know configuration len */
	if (usb_getDescriptor(dev, USB_DESC_CONFIG, 0, (char *)&pre, sizeof(pre)) < 0) {
		USB_LOG("usb: Fail to get configuration descriptor\n");
		return -1;
	}

	/* TODO: check descriptor correctness */
	if ((conf = malloc(pre.wTotalLength)) == NULL)
		return -ENOMEM;

	/* TODO: Handle multiple configuration devices */
	if (usb_getDescriptor(dev, USB_DESC_CONFIG, 0, (char *)conf, pre.wTotalLength) < 0) {
		USB_LOG("usb: Fail to get configuration descriptor\n");
		free(conf);
		return -1;
	}

	dev->nifs = conf->bNumInterfaces;
	if ((dev->ifs = calloc(dev->nifs, sizeof(usb_iface_t))) == NULL) {
		free(conf);
		return -ENOMEM;
	}

	ptr = (char *)conf + sizeof(usb_configuration_desc_t);
	size = pre.wTotalLength - sizeof(usb_configuration_desc_t);
	nifs = 0;

	if (usb_isRoothub(dev)) {
		dev->ifs[0].desc = (usb_interface_desc_t *)ptr;
		dev->ifs[0].eps = (usb_endpoint_desc_t *)(ptr + 9);
		dev->conf = conf;
		return 0;
	}

	while (size >= (int)sizeof(struct usb_desc_header)) {
		uint8_t len = ((struct usb_desc_header *)ptr)->bLength;

		if ((len < sizeof(struct usb_desc_header)) || (len > size)) {
			USB_LOG("usb: Invalid descriptor size: %u\n", len);
			break;
		}

		switch (((struct usb_desc_header *)ptr)->bDescriptorType) {
			case USB_DESC_INTERFACE:
				if (len == sizeof(usb_interface_desc_t)) {
					if (nifs < dev->nifs) {
						dev->ifs[nifs].desc = (usb_interface_desc_t *)ptr;
						nifs += 1;
					}
					else {
						ret = -1;
					}
				}
				else {
					USB_LOG("usb: Interface descriptor with invalid size\n");
					ret = -1;
				}
				break;

			case USB_DESC_ENDPOINT:
				if (len == sizeof(usb_endpoint_desc_t)) {
					if ((nifs != 0) && (nifs <= dev->nifs) && (dev->ifs[nifs - 1].eps == NULL)) {
						dev->ifs[nifs - 1].eps = (usb_endpoint_desc_t *)ptr;
					}
				}
				else {
					USB_LOG("usb: Endpoint descriptor with invalid size\n");
					ret = -1;
				}
				break;

			case USB_DESC_INTERFACE_ASSOCIATION:
				/* FIXME: right now all IADs needs to be of the same class (eg. CDC), multiple claseeses are not supported */
				if (len == sizeof(usb_interface_association_desc_t)) {
					dev->desc.bDeviceClass = ((usb_interface_association_desc_t *)ptr)->bFunctionClass;
					dev->desc.bDeviceSubClass = ((usb_interface_association_desc_t *)ptr)->bFunctionSubClass;
					dev->desc.bDeviceProtocol = ((usb_interface_association_desc_t *)ptr)->bFunctionProtocol;
				}
				else {
					USB_LOG("usb: Interface assoctiation descriptor with invalid size\n");
					ret = -1;
				}
				break;

			case USB_DESC_CS_INTERFACE:
				/* TODO: save Class-Specific Functional Descriptors to be used by device drivers - silently ignored for now */
				break;

			default:
				USB_LOG("usb: Ignoring unkonown descriptor type: 0x%02x\n", ((struct usb_desc_header *)ptr)->bDescriptorType);
				break;
		}

		size -= len;
		ptr += len;
	}

	if ((ret != 0) || (nifs != dev->nifs)) {
		USB_LOG("usb: Fail to parse interface descriptors\n");
		free(dev->ifs);
		free(conf);
		return ret;
	}

	dev->conf = conf;

	return 0;
}


static int usb_getStringDesc(usb_dev_t *dev, char **buf, int index)
{
	usb_string_desc_t desc = { 0 };
	int i;
	size_t asciisz;

	if (usb_getDescriptor(dev, USB_DESC_STRING, index, (char *)&desc, sizeof(desc)) < 0) {
		USB_LOG("usb: Fail to get string descriptor\n");
		return -1;
	}
	asciisz = (desc.bLength - 2) / 2;

	/* Convert from unicode to ascii */
	if ((*buf = calloc(1, asciisz + 1)) == NULL)
		return -ENOMEM;

	for (i = 0; i < asciisz; i++)
		(*buf)[i] = desc.wData[i * 2];

	return 0;
}


static int usb_getAllStringDescs(usb_dev_t *dev)
{
	usb_string_desc_t desc = { 0 };
	int i;

	if (usb_getDescriptor(dev, USB_DESC_STRING, 0, (char *)&desc, sizeof(desc)) < 0) {
		USB_LOG("usb: Fail to get configuration descriptor\n");
		return -1;
	}

	if (desc.bLength < 4)
		return -1;

	/* Choose language id */
	dev->langId = desc.wData[0] | ((uint16_t)desc.wData[1] << 8);

	if (dev->desc.iManufacturer != 0) {
		if (usb_getStringDesc(dev, &dev->manufacturer, dev->desc.iManufacturer) != 0)
			return -ENOMEM;
	}

	if (dev->desc.iProduct != 0) {
		if (usb_getStringDesc(dev, &dev->product, dev->desc.iProduct) != 0)
			return -ENOMEM;
	}

	if (dev->desc.iSerialNumber != 0) {
		if (usb_getStringDesc(dev, &dev->serialNumber, dev->desc.iSerialNumber) != 0)
			return -ENOMEM;
	}

	for (i = 0; i < dev->nifs; i++) {
		if (dev->ifs[i].desc->iInterface == 0)
			continue;
		if (usb_getStringDesc(dev, &dev->ifs[i].str, dev->ifs[i].desc->iInterface) != 0)
			return -ENOMEM;
	}

	/* TODO: Configuration string descriptors */

	return 0;
}


int usb_devEnumerate(usb_dev_t *dev)
{
	int addr;

	if (usb_genLocationID(dev) < 0) {
		USB_LOG("usb: Fail to generate location ID\n");
		return -1;
	}

	if (usb_getDevDesc(dev) < 0) {
		USB_LOG("usb: Fail to get device descriptor\n");
		return -1;
	}

	/* First one is always control */
	dev->ctrlPipe->maxPacketLen = dev->desc.bMaxPacketSize0;

	if ((addr = hcd_addrAlloc(dev->hcd)) < 0) {
		USB_LOG("usb: Fail to add device to hcd\n");
		return -1;
	}

	if (usb_setAddress(dev, addr) < 0) {
		USB_LOG("usb: Fail to set device address\n");
		return -1;
	}

	if (usb_getDevDesc(dev) < 0) {
		USB_LOG("usb: Fail to get device descriptor\n");
		return -1;
	}

	if (usb_getConfiguration(dev) < 0) {
		USB_LOG("usb: Fail to get configuration descriptor\n");
		return -1;
	}

	if (usb_getAllStringDescs(dev) < 0) {
		USB_LOG("usb: Fail to get string descriptors\n");
		return -1;
	}

	if (!usb_isRoothub(dev))
		usb_devSetChild(dev->hub, dev->port, dev);

	USB_LOG("usb: New device addr: %d locationID: %08x %s, %s\n", dev->address, dev->locationID,
		dev->manufacturer, dev->product);

	if (dev->desc.bDeviceClass == USB_CLASS_HUB) {
		if (hub_conf(dev) != 0)
			return -1;
	}
	else if (usb_drvBind(dev) != 0) {
		USB_LOG("usb: Fail to match drivers for device\n");
		/* TODO: make device orphaned */
	}

	return 0;
}


static void usb_devUnbind(usb_dev_t *dev)
{
	int i;

	for (i = 0; i < dev->nports; i++) {
		if (dev->devs[i] != NULL)
			usb_devUnbind(dev->devs[i]);
	}

	for (i = 0; i < dev->nifs; i++) {
		if (dev->ifs[i].driver)
			usb_drvUnbind(dev->ifs[i].driver, dev, i);
	}
}


void usb_devSetChild(usb_dev_t *parent, int port, usb_dev_t *child)
{
	mutexLock(usbdev_common.lock);
	parent->devs[port - 1] = child;
	mutexUnlock(usbdev_common.lock);
}


usb_dev_t *usb_devFind(usb_dev_t *hub, int locationID)
{
	usb_dev_t *dev = hub;
	int port;

	mutexLock(usbdev_common.lock);
	locationID >>= 4;
	while (locationID != 0) {
		port = locationID & 0xf;
		if (port > dev->nports || dev->devs[port - 1] == NULL) {
			dev = NULL;
			break;
		}
		dev = dev->devs[port - 1];
		locationID >>= 4;
	}
	mutexUnlock(usbdev_common.lock);

	return dev;
}


void usb_devDisconnected(usb_dev_t *dev)
{
	printf("usb: Device disconnected addr %d locationID: %08x\n", dev->address, dev->locationID);
	usb_devSetChild(dev->hub, dev->port, NULL);
	usb_devUnbind(dev);
	usb_devDestroy(dev);
}


void usb_devSignal(void)
{
	condSignal(usbdev_common.cond);
}


int usb_isRoothub(usb_dev_t *dev)
{
	return (dev->hub == NULL);
}


int usb_devInit(void)
{
	if (mutexCreate(&usbdev_common.lock) != 0) {
		USB_LOG("usbdev: Can't create mutex!\n");
		return -ENOMEM;
	}

	if (condCreate(&usbdev_common.cond) != 0) {
		resourceDestroy(usbdev_common.lock);
		USB_LOG("usbdev: Can't create cond!\n");
		return -ENOMEM;
	}

	if ((usbdev_common.setupBuf = usb_alloc(USBDEV_BUF_SIZE)) == NULL) {
		resourceDestroy(usbdev_common.lock);
		resourceDestroy(usbdev_common.cond);
		USB_LOG("usbdev: Fail to allocate buffer!\n");
		return -ENOMEM;
	}

	usbdev_common.ctrlBuf = usbdev_common.setupBuf + 32;

	return 0;
}
