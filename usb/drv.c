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


#include <errno.h>
#include <sys/msg.h>
#include <sys/threads.h>
#include <sys/list.h>

#include <usb.h>
#include <usbdriver.h>

#include "drv.h"
#include "hcd.h"

struct {
	handle_t lock;
	handle_t cond;
	usb_driver_t *drvs;
} usbdrv_common;


static int usb_drvcmp(usb_device_desc_t *dev, usb_interface_desc_t *iface, usb_device_id_t *filter)
{
	int match = usbdrv_match;

	if (filter->dclass != USBDRV_ANY) {
		if ((dev->bDeviceClass != 0 && dev->bDeviceClass == filter->dclass) ||
			(dev->bDeviceClass == 0 && iface->bInterfaceClass == filter->dclass))
			match |= usbdrv_class_match;
		else
			return usbdrv_nomatch;
	}

	if (filter->subclass != USBDRV_ANY) {
		if ((dev->bDeviceSubClass != 0 && dev->bDeviceSubClass == filter->subclass) ||
			(dev->bDeviceSubClass == 0 && iface->bInterfaceSubClass == filter->subclass))
			match |= usbdrv_subclass_match;
		else
			return usbdrv_nomatch;
	}

	if (filter->protocol != USBDRV_ANY) {
		if ((dev->bDeviceProtocol != 0 && dev->bDeviceProtocol == filter->protocol) ||
			(dev->bDeviceProtocol == 0 && iface->bInterfaceProtocol == filter->protocol))
			match |= usbdrv_protocol_match;
		else
			return usbdrv_nomatch;
	}

	if (filter->vid != USBDRV_ANY) {
		if (dev->idVendor == filter->vid)
			match |= usbdrv_vid_match;
		else
			return usbdrv_nomatch;
	}

	if (filter->pid != USBDRV_ANY) {
		if (dev->idProduct == filter->pid)
			match |= usbdrv_pid_match;
		else
			return usbdrv_nomatch;
	}

	return match;
}


static usb_driver_t *usb_drvMatchIface(usb_dev_t *dev, usb_iface_t *iface)
{
	usb_driver_t *drv, *best = NULL;
	int i, match, bestmatch = 0;

	mutexLock(usbdrv_common.lock);
	drv = usbdrv_common.drvs;
	if (drv == NULL) {
		mutexUnlock(usbdrv_common.lock);
		return NULL;
	}

	do {
		for (i = 0; i < drv->nfilters; i++) {
			match = usb_drvcmp(&dev->desc, iface->desc, &drv->filters[i]);
			if (match > bestmatch) {
				bestmatch = match;
				best = drv;
			}
		}
	} while ((drv = drv->next) != usbdrv_common.drvs);
	mutexUnlock(usbdrv_common.lock);

	return best;
}


int usb_drvUnbind(usb_dev_t *dev)
{
	msg_t msg = { 0 };
	usb_msg_t *umsg = (usb_msg_t *)msg.i.raw;
	int i;

	msg.type = mtDevCtl;
	umsg->type = usb_msg_deletion;
	umsg->deletion.bus = dev->hcd->num;
	umsg->deletion.dev = dev->address;

	for (i = 0; i < dev->nifs; i++) {
		umsg->deletion.interface = i;
		/* TODO: use non blocking version of msgSend */
		if (dev->ifs[i].driver != NULL)
			msgSend(dev->ifs[i].driver->port, &msg);
	}

	return 0;
}


int usb_drvBind(usb_dev_t *dev)
{
	usb_driver_t *drv;
	msg_t msg = { 0 };
	usb_msg_t *umsg = (usb_msg_t *)msg.i.raw;
	int i;

	msg.type = mtDevCtl;
	umsg->type = usb_msg_insertion;
	umsg->insertion.bus = dev->hcd->num;
	umsg->insertion.dev = dev->address;
	umsg->insertion.descriptor = dev->desc;
	umsg->insertion.locationID = dev->locationID;

	for (i = 0; i < dev->nifs; i++) {
		if ((drv = usb_drvMatchIface(dev, &dev->ifs[i])) != NULL) {
			dev->ifs[i].driver = drv;
			umsg->insertion.interface = i;
			msgSend(drv->port, &msg);
		}
		else {
			fprintf(stderr, "usb: Fail to match iface: %d\n", i);
		}
	}

	return 0;
}


usb_driver_t *usb_drvFind(int pid)
{
	usb_driver_t *drv = usbdrv_common.drvs, *res = NULL;

	mutexLock(usbdrv_common.lock);
	if (drv != NULL) {
		do {
			if (drv->pid == pid)
				res = drv;

			drv = drv->next;
		} while (drv != usbdrv_common.drvs);
	}
	mutexUnlock(usbdrv_common.lock);

	return res;
}


void usb_drvAdd(usb_driver_t *drv)
{
	mutexLock(usbdrv_common.lock);
	LIST_ADD(&usbdrv_common.drvs, drv);
	mutexUnlock(usbdrv_common.lock);
}


int usb_drvInit(void)
{
	if (mutexCreate(&usbdrv_common.lock) != 0) {
		fprintf(stderr, "usbdrv: Can't create mutex!\n");
		return -ENOMEM;
	}

	if (condCreate(&usbdrv_common.cond) != 0) {
		fprintf(stderr, "usbdrv: Can't create mutex!\n");
		return -ENOMEM;
	}

	return 0;
}
