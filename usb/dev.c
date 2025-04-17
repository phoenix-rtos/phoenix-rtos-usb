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


#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <unistd.h>
#include <sys/threads.h>
#include <sys/minmax.h>
#include <sys/list.h>
#include <sys/stat.h>
#include <posix/utils.h>

#include <usb.h>
#include <usbdriver.h>

#include "usbhost.h"
#include "dev.h"
#include "drv.h"
#include "hcd.h"
#include "hub.h"
#include "log.h"

#define USBDEV_BUF_SIZE 0x200


struct {
	handle_t lock;
	handle_t cond;
	char *ctrlBuf;
	char *setupBuf;
	rbtree_t devtree;
} usbdev_common;


int usb_devCtrl(usb_dev_t *dev, usb_dir_t dir, usb_setup_packet_t *setup, char *buf, size_t len)
{
	usb_transfer_t t = (usb_transfer_t) {
		.type = usb_transfer_control,
		.direction = dir,
		.setup = (usb_setup_packet_t *)usbdev_common.setupBuf,
		.buffer = usbdev_common.ctrlBuf,
		.size = len,
		.recipient = usb_drvType_hcd,
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

	free(dev->manufacturer.str);
	free(dev->product.str);
	free(dev->serialNumber.str);
	free(dev->conf);

	for (i = 0; i < dev->nifs; i++)
		free(dev->ifs[i].name.str);

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
	int size;
	int ret = 0;

	/* Get first nine bytes to get to know configuration len */
	if (usb_getDescriptor(dev, USB_DESC_CONFIG, 0, (char *)&pre, sizeof(pre)) < 0) {
		return -1;
	}

	if ((pre.bLength != sizeof(pre)) || (pre.bDescriptorType != USB_DESC_CONFIG) || (pre.wTotalLength < sizeof(pre))) {
		/* Invalid data returned */
		return -1;
	}

	if ((conf = malloc(pre.wTotalLength)) == NULL)
		return -ENOMEM;

	/* TODO: Handle multiple configuration devices */
	if (usb_getDescriptor(dev, USB_DESC_CONFIG, 0, (char *)conf, pre.wTotalLength) < 0) {
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


	int lastIfNum = -1;
	uint8_t lastAlternateSetting = 0;
	while ((size >= (int)sizeof(struct usb_desc_header)) && (ret == 0)) {
		uint8_t len = ((struct usb_desc_header *)ptr)->bLength;

		if ((len < sizeof(struct usb_desc_header)) || (len > size)) {
			log_error("Invalid descriptor size: %u\n", len);
			break;
		}

		switch (((struct usb_desc_header *)ptr)->bDescriptorType) {
			case USB_DESC_INTERFACE:
				if (len == sizeof(usb_interface_desc_t)) {
					usb_interface_desc_t *desc = (usb_interface_desc_t *)ptr;
					lastIfNum = desc->bInterfaceNumber;
					lastAlternateSetting = desc->bAlternateSetting;
					if (lastIfNum >= dev->nifs) {
						/* Invalid interface number */
						ret = -1;
						break;
					}

					if (lastAlternateSetting != 0) {
						/* TODO: handle alternate setting maybe */
					}
					else {
						dev->ifs[lastIfNum].desc = desc;
					}
				}
				else {
					log_error("Interface descriptor with invalid size\n");
					ret = -1;
				}
				break;

			case USB_DESC_ENDPOINT:
				if (len == sizeof(usb_endpoint_desc_t)) {
					if (lastIfNum < 0) {
						/* TODO: should this be considered an error? */
						break;
					}

					if (lastAlternateSetting != 0) {
						/* Endpoint belongs to alternate setting - we don't handle it right now */
					}
					else if (dev->ifs[lastIfNum].eps == NULL) {
						dev->ifs[lastIfNum].eps = (usb_endpoint_desc_t *)ptr;
						if (size < (dev->ifs[lastIfNum].desc->bNumEndpoints * sizeof(usb_endpoint_desc_t))) {
							ret = -1;
							break;
						}
					}
					else if (ptr >= (char *)(dev->ifs[lastIfNum].eps + dev->ifs[lastIfNum].desc->bNumEndpoints)) {
						ret = -1;
						break;
					}
				}
				else {
					log_error("Endpoint descriptor with invalid size\n");
					ret = -1;
				}
				break;

			case USB_DESC_INTERFACE_ASSOCIATION:
				/* FIXME: right now all IADs needs to be of the same class (eg. CDC), multiple classes are not supported */
				if (len == sizeof(usb_interface_association_desc_t)) {
					dev->desc.bDeviceClass = ((usb_interface_association_desc_t *)ptr)->bFunctionClass;
					dev->desc.bDeviceSubClass = ((usb_interface_association_desc_t *)ptr)->bFunctionSubClass;
					dev->desc.bDeviceProtocol = ((usb_interface_association_desc_t *)ptr)->bFunctionProtocol;
				}
				else {
					log_error("Interface assoctiation descriptor with invalid size\n");
					ret = -1;
				}
				break;

			case USB_DESC_CS_INTERFACE:
				/* TODO: save Class-Specific Functional Descriptors to be used by device drivers - silently ignored for now */
				break;

			default:
				log_error("Ignoring unkonown descriptor type: 0x%02x\n", ((struct usb_desc_header *)ptr)->bDescriptorType);
				break;
		}

		size -= len;
		ptr += len;
	}

	for (size_t i = 0; i < dev->nifs; i++) {
		if ((dev->ifs[i].desc == NULL) || (dev->ifs[i].eps == NULL)) {
			/* Data missing */
			ret = -1;
			break;
		}
	}

	if (ret != 0) {
		log_error("Fail to parse interface descriptors\n");
		free(dev->ifs);
		dev->ifs = NULL;
		dev->nifs = 0;
		free(conf);
		return ret;
	}

	dev->conf = conf;

	return 0;
}


static int usb_getStringDesc(usb_dev_t *dev, usb_lenStr_t *dest, int index)
{
	usb_string_desc_t desc = { 0 };

	if (usb_getDescriptor(dev, USB_DESC_STRING, index, (char *)&desc, sizeof(desc)) < 0) {
		log_error("Fail to get string descriptor\n");
		return -1;
	}

	dest->len = desc.bLength;
	dest->str = calloc(sizeof(char), dest->len);
	if (dest->str == NULL) {
		return -ENOMEM;
	}

	memcpy(dest->str, desc.wData, desc.bLength);

	return 0;
}


/* assumes dest buffer size >= len / 2 + 1 */
static unsigned int usb_utf16ToAscii(char *dest, const char *src, unsigned int len)
{
	unsigned int asciilen = len / 2;
	int i;

	if (len < 2) {
		return 0;
	}

	for (i = 0; i < asciilen; i++) {
		dest[i] = src[i * 2];
	}

	dest[asciilen] = 0;

	return asciilen;
}


static int usb_asciiToUtf16(char *dest, const char *src)
{
	unsigned int n = min(strlen(src), USB_STR_MAX);
	int i;

	for (i = 0; i < n; i++) {
		dest[i * 2] = src[i];
		dest[i * 2 + 1] = 0;
	}

	return 2 * n;
}


static void usb_fallbackProductString(usb_dev_t *dev)
{
	char product[USB_STR_MAX] = { 0 };
	unsigned int len;

	switch (dev->desc.bDeviceClass) {
		case USB_CLASS_HID:
			len = usb_asciiToUtf16(product, "USB HID");
			break;
		case USB_CLASS_HUB:
			switch (dev->desc.bDeviceProtocol) {
				case USB_HUB_PROTO_ROOT:
					len = usb_asciiToUtf16(product, "USB Root Hub");
					break;
				case USB_HUB_PROTO_SINGLE_TT:
					len = usb_asciiToUtf16(product, "USB Single TT Hub");
					break;
				default:
					len = usb_asciiToUtf16(product, "USB Hub");
					break;
			}
			break;
		case USB_CLASS_MASS_STORAGE:
			len = usb_asciiToUtf16(product, "USB Mass Storage");
			break;
		default:
			len = usb_asciiToUtf16(product, "Unknown USB Device");
			break;
	}

	dev->product.len = len;
	dev->product.str = calloc(sizeof(char), dev->product.len);
	memcpy(dev->product.str, product, dev->product.len);
}


static void usb_fallbackManufacturerString(usb_dev_t *dev)
{
	char manufacturer[] = "Generic";

	dev->manufacturer.len = 2 * strnlen(manufacturer, USB_STR_MAX / 2) + 1;
	dev->manufacturer.str = calloc(sizeof(char), dev->manufacturer.len);
	usb_asciiToUtf16(dev->manufacturer.str, manufacturer);
}


static void usb_fallbackSerialNumberString(usb_dev_t *dev)
{
	char serialNumber[] = "Unknown";

	dev->serialNumber.len = 2 * strnlen(serialNumber, USB_STR_MAX / 2) + 1;
	dev->serialNumber.str = calloc(sizeof(char), dev->serialNumber.len);
	usb_asciiToUtf16(dev->serialNumber.str, serialNumber);
}


static int usb_getAllStringDescs(usb_dev_t *dev)
{
	usb_string_desc_t desc = { 0 };
	int i, ret;

	/* Get an array of language ids */
	/* String descriptors are optional. If a device omits all string descriptors,
	 * it must not return this array, so the following call is allowed to fail in
	 * that case */
	if (usb_getDescriptor(dev, USB_DESC_STRING, 0, (char *)&desc, sizeof(desc)) < 0) {
		usb_fallbackManufacturerString(dev);
		usb_fallbackProductString(dev);
		usb_fallbackSerialNumberString(dev);
		return -ENOTSUP;
	}

	if (desc.bLength < 4)
		return -1;

	/* Choose language id */
	dev->langId = desc.wData[0] | ((uint16_t)desc.wData[1] << 8);

	if (dev->desc.iManufacturer != 0) {
		ret = usb_getStringDesc(dev, &dev->manufacturer, dev->desc.iManufacturer);
	}
	if (dev->desc.iManufacturer == 0 || ret != 0) {
		usb_fallbackManufacturerString(dev);
	}

	if (dev->desc.iProduct != 0) {
		ret = usb_getStringDesc(dev, &dev->product, dev->desc.iProduct);
	}
	if (dev->desc.iProduct == 0 || ret != 0) {
		usb_fallbackProductString(dev);
	}

	if (dev->desc.iSerialNumber != 0) {
		ret = usb_getStringDesc(dev, &dev->serialNumber, dev->desc.iSerialNumber);
	}
	if (dev->desc.iSerialNumber == 0 || ret != 0) {
		usb_fallbackSerialNumberString(dev);
	}

	for (i = 0; i < dev->nifs; i++) {
		if (dev->ifs[i].desc->iInterface == 0)
			continue;
		if (usb_getStringDesc(dev, &dev->ifs[i].name, dev->ifs[i].desc->iInterface) != 0)
			return -ENOMEM;
	}

	/* TODO: Configuration string descriptors */

	return 0;
}


int usb_devFilterMatch(usb_device_desc_t *dev, usb_interface_desc_t *iface, const usb_device_id_t *filter)
{
	int match = usbdrv_match;

	if (filter->dclass != USBDRV_ANY) {
		if ((dev->bDeviceClass != 0 && dev->bDeviceClass == filter->dclass) ||
				(dev->bDeviceClass == 0 && iface->bInterfaceClass == filter->dclass))
			match |= usbdrv_class_match;
		else {
			return usbdrv_nomatch;
		}
	}

	if (filter->subclass != USBDRV_ANY) {
		if ((dev->bDeviceSubClass != 0 && dev->bDeviceSubClass == filter->subclass) ||
				(dev->bDeviceSubClass == 0 && iface->bInterfaceSubClass == filter->subclass))
			match |= usbdrv_subclass_match;
		else {
			return usbdrv_nomatch;
		}
	}

	if (filter->protocol != USBDRV_ANY) {
		if ((dev->bDeviceProtocol != 0 && dev->bDeviceProtocol == filter->protocol) ||
				(dev->bDeviceProtocol == 0 && iface->bInterfaceProtocol == filter->protocol))
			match |= usbdrv_protocol_match;
		else {
			return usbdrv_nomatch;
		}
	}

	if (filter->vid != USBDRV_ANY) {
		if (dev->idVendor == filter->vid)
			match |= usbdrv_vid_match;
		else {
			return usbdrv_nomatch;
		}
	}

	if (filter->pid != USBDRV_ANY) {
		if (dev->idProduct == filter->pid)
			match |= usbdrv_pid_match;
		else {
			return usbdrv_nomatch;
		}
	}

	return match;
}


static int usb_devCmp(rbnode_t *node1, rbnode_t *node2)
{
	usb_dev_t *dev1 = lib_treeof(usb_dev_t, node, node1);
	usb_dev_t *dev2 = lib_treeof(usb_dev_t, node, node2);

	if (dev1->oid.id > dev2->oid.id)
		return 1;
	if (dev1->oid.id < dev2->oid.id)
		return -1;

	if (dev1->oid.port > dev2->oid.port)
		return 1;
	if (dev1->oid.port < dev2->oid.port)
		return -1;

	return 0;
}


#define USB_DEV_SYMLINK_FORMAT "/dev/usb-%04x-%04x-if%02d"


static void usb_devSymlinksCreate(usb_dev_t *dev)
{
	char linkpath[64] = { 0 };
	int ret;

	/* TODO: handle per iface symlink */
	sprintf(linkpath, USB_DEV_SYMLINK_FORMAT, dev->desc.idVendor, dev->desc.idProduct, 0);

	unlink(linkpath);
	ret = symlink(dev->devPath, linkpath);

	if (ret < 0) {
		log_error("%s -> %s symlink error: %d", linkpath, dev->devPath, errno);
	}
}


static void usb_devSymlinksDestroy(usb_dev_t *dev)
{
	char linkpath[64] = { 0 };

	/* TODO: handle per iface symlink */
	sprintf(linkpath, USB_DEV_SYMLINK_FORMAT, dev->desc.idVendor, dev->desc.idProduct, 0);
	unlink(linkpath);
}


int usb_devEnumerate(usb_dev_t *dev)
{
	char manufacturerAscii[USB_STR_MAX / 2 + 1];
	char productAscii[USB_STR_MAX / 2 + 1];
	int addr, iface;
	usb_event_insertion_t insertion = { 0 };

	if (usb_genLocationID(dev) < 0) {
		log_error("Fail to generate location ID\n");
		return -1;
	}

	if (usb_getDevDesc(dev) < 0) {
		log_error("Fail to get device descriptor\n");
		return -1;
	}

	/* First one is always control */
	dev->ctrlPipe->maxPacketLen = dev->desc.bMaxPacketSize0;

	if ((addr = hcd_addrAlloc(dev->hcd)) < 0) {
		log_error("Fail to add device to hcd\n");
		return -1;
	}

	if (usb_setAddress(dev, addr) < 0) {
		log_error("Fail to set device address\n");
		return -1;
	}

	if (usb_getDevDesc(dev) < 0) {
		log_error("Fail to get device descriptor\n");
		return -1;
	}

	if (usb_getConfiguration(dev) < 0) {
		log_error("Fail to get configuration descriptor\n");
		return -1;
	}

	(void)usb_getAllStringDescs(dev);

	if (!usb_isRoothub(dev))
		usb_devSetChild(dev->hub, dev->port, dev);

	usb_utf16ToAscii(manufacturerAscii, dev->manufacturer.str, dev->manufacturer.len);
	usb_utf16ToAscii(productAscii, dev->product.str, dev->product.len);

	log_msg("New device: %04x:%04x %s, %s (%d, %08x)\n",
			dev->desc.idVendor, dev->desc.idProduct, manufacturerAscii, productAscii,
			dev->address, dev->locationID);

	if (dev->desc.bDeviceClass == USB_CLASS_HUB) {
		return hub_conf(dev);
	}

	if (usb_drvBind(dev, &insertion, &iface) != 0) {
		log_error("Fail to match drivers for device\n");
		/* TODO: make device orphaned */
	}

	if (insertion.deviceCreated) {
		log_msg("Dev oid bound to device with addr %d: port=%d, id=%d\n",
				dev->address, insertion.dev.port, insertion.dev.id);

		dev->oid = insertion.dev;
		strncpy(dev->devPath, insertion.devPath, sizeof(dev->devPath));
		lib_rbInsert(&usbdev_common.devtree, &dev->node);

		usb_devSymlinksCreate(dev);
	}

	return 0;
}


static void usb_devUnbind(usb_dev_t *dev)
{
	int i;

	for (i = 0; i < dev->nports; i++) {
		if (dev->devs[i] != NULL) {
			usb_devUnbind(dev->devs[i]);
		}
	}

	lib_rbRemove(&usbdev_common.devtree, &dev->node);
	usb_devSymlinksDestroy(dev);

	for (i = 0; i < dev->nifs; i++) {
		if (dev->ifs[i].driver != NULL) {
			usb_drvUnbind(dev->ifs[i].driver, dev, i);
		}
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


int usb_devFindDescFromOid(oid_t oid, usb_devinfo_desc_t *desc)
{
	usb_dev_t *fdev;
	usb_dev_t dev;

	if (desc == NULL) {
		return -EINVAL;
	}

	dev.oid = oid;

	mutexLock(usbdev_common.lock);

	fdev = lib_treeof(usb_dev_t, node, lib_rbFind(&usbdev_common.devtree, &dev.node));
	if (fdev == NULL) {
		log_msg("device not found with oid.id=%d oid.port=%d\n", oid.id, oid.port);
		return -EINVAL;
	}

	memcpy(&desc->desc, &fdev->desc, sizeof(usb_device_desc_t));

	memcpy(desc->product.str, fdev->product.str, fdev->product.len);
	desc->product.len = fdev->product.len;

	memcpy(desc->manufacturer.str, fdev->manufacturer.str, fdev->manufacturer.len);
	desc->manufacturer.len = fdev->manufacturer.len;

	memcpy(desc->serialNumber.str, fdev->serialNumber.str, fdev->serialNumber.len);
	desc->serialNumber.len = fdev->serialNumber.len;

	mutexUnlock(usbdev_common.lock);

	return 0;
}


void usb_devDisconnected(usb_dev_t *dev, bool silent)
{
	if (!silent) {
		log_msg("Device disconnected addr %d locationID: %08x\n", dev->address, dev->locationID);
	}
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
		log_error("Can't create mutex!\n");
		return -ENOMEM;
	}

	if (condCreate(&usbdev_common.cond) != 0) {
		resourceDestroy(usbdev_common.lock);
		log_error("Can't create cond!\n");
		return -ENOMEM;
	}

	if ((usbdev_common.setupBuf = usb_alloc(USBDEV_BUF_SIZE)) == NULL) {
		resourceDestroy(usbdev_common.lock);
		resourceDestroy(usbdev_common.cond);
		log_error("Fail to allocate buffer!\n");
		return -ENOMEM;
	}

	usbdev_common.ctrlBuf = usbdev_common.setupBuf + 32;

	lib_rbInit(&usbdev_common.devtree, usb_devCmp, NULL);

	return 0;
}
