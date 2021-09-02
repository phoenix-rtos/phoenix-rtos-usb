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


void usb_devCtrlFinished(usb_transfer_t *t)
{
	condSignal(usbdev_common.cond);
}


int usb_devCtrl(usb_dev_t *dev, usb_dir_t dir, usb_setup_packet_t *setup, char *buf, size_t len)
{
	usb_transfer_t t = (usb_transfer_t) {
		.ep = dev->eps,
		.type = usb_transfer_control,
		.direction = dir,
		.setup = (usb_setup_packet_t *)usbdev_common.setupBuf,
		.buffer = usbdev_common.ctrlBuf,
		.size = len,
	};

	if (len > USBDEV_BUF_SIZE)
		return -1;

	mutexLock(usbdev_common.lock);
	memcpy(usbdev_common.setupBuf, setup, sizeof(usb_setup_packet_t));
	if (dir == usb_dir_out && len > 0)
		memcpy(usbdev_common.ctrlBuf, buf, len);

	if (dev->hcd->ops->transferEnqueue(dev->hcd, &t) != 0)
		return -1;

	while (!t.finished)
		condWait(usbdev_common.cond, usbdev_common.lock, 0);

	if (t.error == 0 && dir == usb_dir_in && len > 0)
		memcpy(buf, usbdev_common.ctrlBuf, len);
	mutexUnlock(usbdev_common.lock);

	return (t.error == 0) ? t.transferred : -t.error;
}


static void usb_epConf(usb_dev_t *dev, usb_endpoint_t *ep, usb_endpoint_desc_t *desc, int interface)
{
	/* TODO: this should be not necessary, as we are copying descriptor data */
	/* TODO: use only PIPE abstraction - it shall contain all the host internal data,
	 * such as hcdpriv, speed, etc. and pointer to the descriptor.
	 */
	/* TODO: consider removing "interface structure" */
	ep->device = dev;
	ep->number = desc->bEndpointAddress & 0xF;
	ep->direction = (desc->bEndpointAddress & 0x80) ? usb_dir_in : usb_dir_out;
	ep->maxPacketLen = desc->wMaxPacketSize;
	ep->type = desc->bmAttributes & 0x3;
	ep->interval = desc->bInterval;
	ep->hcdpriv = NULL;
	ep->interface = interface;
	ep->pipe = NULL;
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
	usb_endpoint_t *ep0;

	if ((dev = calloc(1, sizeof(usb_dev_t))) == NULL)
		return NULL;

	/* Create control endpoint */
	if ((ep0 = malloc(sizeof(usb_endpoint_t))) == NULL) {
		free(dev);
		return NULL;
	}

	ep0->maxPacketLen = 64; /* Default value */
	ep0->number = 0;
	ep0->device = dev;
	ep0->type = usb_transfer_control;
	ep0->direction = 0;
	ep0->interval = 0;
	ep0->interface = 0;
	ep0->hcdpriv = NULL;
	ep0->pipe = NULL;
	LIST_ADD(&dev->eps, ep0);

	return dev;
}


void usb_devFree(usb_dev_t *dev)
{
	usb_endpoint_t *ep;
	int i;

	if (dev->manufacturer != NULL)
		free(dev->manufacturer);
	if (dev->product != NULL)
		free(dev->product);
	if (dev->product != NULL)
		free(dev->serialNumber);
	if (dev->conf != NULL)
		free(dev->conf);

	for (i = 0; i < dev->nifs; i++) {
		if (dev->ifs[i].str != NULL)
			free(dev->ifs[i].str);
	}

	if (dev->ifs != NULL)
		free(dev->ifs);

	while ((ep = dev->eps) != NULL) {
		LIST_REMOVE(&dev->eps, ep);
		free(ep);
	}

	if (dev->statusTransfer) {
		free(dev->statusTransfer->buffer);
		free(dev->statusTransfer);
	}

	free(dev);
}


static void usb_devDestroy(usb_dev_t *dev)
{
	usb_endpoint_t *ep;
	int i;

	if ((ep = dev->eps) != NULL) {
		do {
			if (ep->pipe != NULL)
				usb_pipeFree(ep->pipe);
		} while ((ep = ep->next) != dev->eps);
	}

	for (i = 0; i < dev->nports; i++) {
		if (dev->devs[i] != NULL)
			usb_devDestroy(dev->devs[i]);
	}

	if (dev->hcd)
		dev->hcd->ops->devDestroy(dev->hcd, dev);

	if (dev->address != 0)
		hcd_addrFree(dev->hcd, dev->address);

	usb_devFree(dev);
}

static int usb_genLocationID(usb_dev_t *dev)
{
	usb_dev_t *hub = dev->hub;
	int tier = 1;

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
	if (usb_getDescriptor(dev, USB_DESC_DEVICE, 0, (char *)&dev->desc, sizeof(usb_device_desc_t)) < 0) {
		fprintf(stderr, "usb: Fail to get device descriptor\n");
		return -1;
	}

	return 0;
}


static int usb_getConfiguration(usb_dev_t *dev)
{
	int i, j;
	usb_configuration_desc_t pre, *conf;
	usb_interface_desc_t *iface;
	usb_endpoint_desc_t *epDesc;
	usb_endpoint_t *ep;
	char *ptr;

	/* Get first nine bytes to get to know configuration len */
	if (usb_getDescriptor(dev, USB_DESC_CONFIG, 0, (char *)&pre, sizeof(pre)) < 0) {
		fprintf(stderr, "usb: Fail to get configuration descriptor\n");
		return -1;
	}

	/* TODO: check descriptor correctness */
	if ((conf = malloc(pre.wTotalLength)) == NULL)
		return -ENOMEM;

	/* TODO: Handle multiple configuration devices */
	if (usb_getDescriptor(dev, USB_DESC_CONFIG, 0, (char *)conf, pre.wTotalLength) < 0) {
		fprintf(stderr, "usb: Fail to get configuration descriptor\n");
		return -1;
	}

	dev->nifs = conf->bNumInterfaces;
	if ((dev->ifs = calloc(dev->nifs, sizeof(usb_iface_t))) == NULL)
		return -ENOMEM;

	ptr = (char *)conf + sizeof(usb_configuration_desc_t);
	for (i = 0; i < dev->nifs; i++) {
		iface = (usb_interface_desc_t *)ptr;
		ptr += sizeof(usb_interface_desc_t);
		/* Class and Vendor specific descriptors */
		/* TODO: handle them properly */
		while (ptr[1] != USB_DESC_ENDPOINT) {
			ptr += ptr[0];
		}
		for (j = 0; j < iface->bNumEndpoints; j++) {
			ep = malloc(sizeof(usb_endpoint_t));
			epDesc = (usb_endpoint_desc_t *)ptr;
			usb_epConf(dev, ep, epDesc, i);
			LIST_ADD(&dev->eps, ep);
			ptr += sizeof(usb_endpoint_desc_t);
		}
		dev->ifs[i].desc = iface;
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
		fprintf(stderr, "usb: Fail to get string descriptor\n");
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
		fprintf(stderr, "usb: Fail to get configuration descriptor\n");
		return -1;
	}

	if (desc.bLength < 4)
		return -1;

	/* Choose language id */
	dev->langId = desc.wData[0] | ((uint16_t)desc.wData[1] << 8);

	if (dev->desc.iManufacturer != 0) {
		if (usb_getStringDesc(dev, &dev->manufacturer, dev->desc.iManufacturer) != 0)
			return -ENOMEM;
		printf("Manufacturer: %s\n", dev->manufacturer);
	}

	if (dev->desc.iProduct != 0) {
		if (usb_getStringDesc(dev, &dev->product, dev->desc.iProduct) != 0)
			return -ENOMEM;
		printf("Product: %s\n", dev->product);
	}

	if (dev->desc.iSerialNumber != 0) {
		if (usb_getStringDesc(dev, &dev->serialNumber, dev->desc.iSerialNumber) != 0)
			return -ENOMEM;
		printf("Serial Number: %s\n", dev->serialNumber);
	}

	for (i = 0; i < dev->nifs; i++) {
		if (dev->ifs[i].desc->iInterface == 0)
			continue;
		if (usb_getStringDesc(dev, &dev->ifs[i].str, dev->ifs[i].desc->iInterface) != 0)
			return -ENOMEM;
		printf("Interface string: %s\n", dev->ifs[i].str);
	}

	/* TODO: Configuration string descriptors */

	return 0;
}


int usb_devEnumerate(usb_dev_t *dev)
{
	int addr;

	if (usb_genLocationID(dev) < 0) {
		fprintf(stderr, "usb: Fail to generate location ID\n");
		usb_devFree(dev);
		return -1;
	}

	if (usb_getDevDesc(dev) < 0) {
		fprintf(stderr, "usb: Fail to get device descriptor\n");
		usb_devDestroy(dev);
		return -1;
	}

	/* First one is always control */
	dev->eps->maxPacketLen = dev->desc.bMaxPacketSize0;

	if ((addr = hcd_addrAlloc(dev->hcd)) < 0) {
		fprintf(stderr, "usb: Fail to add device to hcd\n");
		usb_devDestroy(dev);
		return -1;
	}

	if (usb_setAddress(dev, addr) < 0) {
		fprintf(stderr, "usb: Fail to set device address\n");
		usb_devDestroy(dev);
		return -1;
	}
	fprintf(stderr, "usb: New device addr: %d locationID: %08x\n", dev->address, dev->locationID);

	if (usb_getDevDesc(dev) < 0) {
		fprintf(stderr, "usb: Fail to get device descriptor\n");
		usb_devDestroy(dev);
		return -1;
	}

	if (usb_getConfiguration(dev) < 0) {
		fprintf(stderr, "usb: Fail to get configuration descriptor\n");
		usb_devDestroy(dev);
		return -1;
	}

	if (usb_getAllStringDescs(dev) < 0) {
		fprintf(stderr, "usb: Fail to get string descriptors\n");
		usb_devDestroy(dev);
		return -1;
	}

	if (dev->desc.bDeviceClass == USB_CLASS_HUB) {
		if (hub_add(dev) != 0)
			return -1;
	}
	else if (usb_drvBind(dev) != 0) {
		fprintf(stderr, "usb: Fail to match drivers for device\n");
		/* TODO: make device orphaned */
		usb_devDestroy(dev);
		return -1;
	}

	return 0;
}


static void usb_devUnbind(usb_dev_t *dev)
{
	int i;

	fprintf(stderr, "usb: Device disconnected addr %d locationID: %08x\n", dev->address, dev->locationID);
	if (dev->desc.bDeviceClass == USB_CLASS_HUB)
		hub_remove(dev);
	else
		usb_drvUnbind(dev);

	for (i = 0; i < dev->nports; i++) {
		if (dev->devs[i] != NULL)
			usb_devUnbind(dev->devs[i]);
	}
}


void usb_devDisconnected(usb_dev_t *dev)
{
	usb_devUnbind(dev);
	usb_devDestroy(dev);
}


int usb_devInit(void)
{
	if (mutexCreate(&usbdev_common.lock) != 0) {
		fprintf(stderr, "usbdev: Can't create mutex!\n");
		return -ENOMEM;
	}

	if (condCreate(&usbdev_common.cond) != 0) {
		resourceDestroy(usbdev_common.lock);
		fprintf(stderr, "usbdev: Can't create mutex!\n");
		return -ENOMEM;
	}

	if ((usbdev_common.setupBuf = usb_alloc(USBDEV_BUF_SIZE)) == NULL) {
		fprintf(stderr, "usbdev: Fail to allocate buffer!\n");
		return -ENOMEM;
	}

	usbdev_common.ctrlBuf = usbdev_common.setupBuf + 32;

	return 0;
}
