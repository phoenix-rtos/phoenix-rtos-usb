/*
 * Phoenix-RTOS
 *
 * USB Host Controller Device
 *
 * Copyright 2021 Phoenix Systems
 * Author: Maciej Purski
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include <sys/list.h>
#include <sys/threads.h>
#include <string.h>
#include <stdlib.h>

#include <hcd.h>
#include <usbhost.h>

static struct {
	hcd_ops_t *hcdops;
} hcd_common;


usb_device_t *hcd_deviceCreate(hcd_t *hcd)
{
	usb_device_t *dev;

	if ((dev = calloc(1, sizeof(usb_device_t))) == NULL)
		return NULL;

	/* Create control endpoint */
	if ((dev->ep0 = malloc(sizeof(usb_endpoint_t))) == NULL) {
		free(dev);
		return NULL;
	}

	dev->hcd = hcd;
	dev->ep0->max_packet_len = 64; /* Default value */
	dev->ep0->number = 0;
	dev->ep0->device = dev;
	dev->ep0->type = usb_ep_control;
	dev->ep0->direction = usb_ep_bi;
	dev->ep0->hcdpriv = NULL;

	return dev;
}


void hcd_deviceFree(usb_device_t *dev)
{
	int i;

	if (dev->hcd)
		dev->hcd->ops->devDestroy(dev->hcd, dev);
	free(dev->ep0);
	if (dev->manufacturer != NULL)
		free(dev->manufacturer);
	if (dev->product != NULL)
		free(dev->product);
	if (dev->product != NULL)
		free(dev->serialNumber);
	if (dev->conf != NULL)
		free(dev->conf);

	for (i = 0; i < dev->nifs; i++) {
		if (dev->ifs[i].eps != NULL)
			free(dev->ifs[i].eps);

		if (dev->ifs[i].str != NULL)
			free(dev->ifs[i].str);
	}

	if (dev->ifs != NULL)
		free(dev->ifs);

	free(dev);
}


int hcd_deviceAdd(hcd_t *hcd, usb_device_t *hub, usb_device_t *dev, int port)
{
	uint32_t b, addr;
	int i;

	if (port > hub->ndevices)
		return -1;

	for (i = 0, addr = 0; i < 4; i++, addr += 32) {
		if ((b = __builtin_ffsl(~hcd->addrmask[i])) != 0)
			break;
	}

	if (b == 0)
		return -1;

	addr += b - 1;
	hcd->addrmask[i] |= 1UL << (b - 1UL);

	hub->devices[port - 1] = dev;
	dev->hub = hub;

	return addr;
}


void hcd_deviceRemove(hcd_t *hcd, usb_device_t *hub, int port)
{
	usb_device_t *dev;

	if (port > hub->ndevices)
		return;

	/* TODO: remove subdevices recursively */
	dev = hub->devices[port - 1];
	hub->devices[port - 1] = NULL;

	hcd->addrmask[dev->address / 32] &= ~(1UL << (dev->address % 32));
}


void hcd_register(hcd_ops_t *ops)
{
	LIST_ADD(&hcd_common.hcdops, ops);
}


static const hcd_ops_t *hcd_lookup(const char *type)
{
	hcd_ops_t *ops = hcd_common.hcdops;

	if (ops == NULL)
		return NULL;

	do {
		if (!strcmp(type, ops->type))
			return ops;
		ops = ops->next;
	} while (ops != hcd_common.hcdops);

	return NULL;
}


static hcd_t *hcd_create(const hcd_ops_t *ops, const hcd_info_t *info)
{
	hcd_t *hcd;

	if ((hcd = malloc(sizeof(hcd_t))) == NULL)
		return NULL;

	hcd->info = info;
	hcd->priv = NULL;
	hcd->transfers = NULL;
	hcd->ops = ops;
	mutexCreate(&hcd->transLock);
	/*
	 * 0 is reserved for the enumerating device,
	 * 1 is reserved for the roothub.
	 */
	hcd->addrmask[0] = 0x3;
	hcd->addrmask[1] = 0;
	hcd->addrmask[2] = 0;
	hcd->addrmask[3] = 0;

	return hcd;
}

static void hcd_free(hcd_t *hcds)
{
	hcd_ops_t *nops, *tops = hcd_common.hcdops;
	hcd_t *tmp = hcds, *n;

	if (tmp != NULL) {
    	do {
			n = tmp->next;
			free(tmp->roothub);
			free(tmp);
    	} while ((tmp = n) != hcds);
	}

	if (tops != NULL) {
    	do {
			nops = tops->next;
			free(tops);
    	} while ((tops = nops) != hcd_common.hcdops);
	}
}

hcd_t *hcd_init(void)
{
	const hcd_info_t *info;
	const hcd_ops_t *ops;
	usb_device_t *hub;
	hcd_t *hcd, *res = NULL;
	int nhcd, i;

	if ((nhcd = hcd_getInfo(&info)) <= 0)
		return NULL;

	for (i = 0; i < nhcd; i++) {
		if ((ops = hcd_lookup(info[i].type)) == NULL) {
			hcd_free(res);
			return NULL;
		}

		if ((hcd = hcd_create(ops, &info[i])) == NULL) {
			hcd_free(res);
			return NULL;
		}

		/* Create root hub */
		if ((hub = hcd_deviceCreate(hcd)) == NULL) {
			hcd_free(res);
			free(hcd);
			return NULL;
		}

		hcd->roothub = hub;
		hub->address = 1;

		if (hcd->ops->init(hcd) != 0) {
			hcd_free(res);
			hcd_deviceFree(hub);
			free(hcd);
			return NULL;
		}

		LIST_ADD(&res, hcd);
	}

	return res;
}