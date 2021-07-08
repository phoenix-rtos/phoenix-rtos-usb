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

#include "usbhost.h"
#include "dev.h"
#include "hub.h"
#include "hcd.h"


static struct {
	hcd_ops_t *hcdops;
} hcd_common;


int hcd_addrAlloc(hcd_t *hcd)
{
	uint32_t b, addr;
	int i;

	/* Allocate address */
	for (i = 0, addr = 0; i < 4; i++, addr += 32) {
		if ((b = __builtin_ffsl(~hcd->addrmask[i])) != 0)
			break;
	}

	if (b == 0)
		return -1;

	addr += b - 1;
	hcd->addrmask[i] |= 1UL << (b - 1UL);

	return addr;
}


void hcd_addrFree(hcd_t *hcd, int addr)
{
	hcd->addrmask[addr / 32] &= ~(1UL << (addr % 32));
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


static hcd_t *hcd_create(hcd_ops_t *ops, const hcd_info_t *info, int num)
{
	hcd_t *hcd;

	if ((hcd = malloc(sizeof(hcd_t))) == NULL)
		return NULL;

	hcd->info = info;
	hcd->priv = NULL;
	hcd->transfers = NULL;
	hcd->ops = ops;
	hcd->num = num;
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


usb_dev_t *hcd_devFind(hcd_t *hcdList, uint32_t locationID)
{
	usb_dev_t *d;
	hcd_t *hcd = hcdList;
	int port;

	do {
		if ((locationID & 0xf) == hcd->num)
			break;
	} while ((hcd = hcd->next) != hcdList);

	if ((locationID & 0xf) != hcd->num)
		return NULL;
	locationID >>= 4;

	d = hcd->roothub;
	while (locationID != 0) {
		port = locationID & 0xf;
		if (port > d->nports || d->devs[port - 1] == NULL)
			return NULL;
		d = d->devs[port - 1];
		locationID >>= 4;
	}

	return d;
}


hcd_t *hcd_init(void)
{
	const hcd_info_t *info;
	hcd_ops_t *ops;
	hcd_t *hcd, *res = NULL;
	int nhcd, i;
	int num = 1;

	if ((nhcd = hcd_getInfo(&info)) <= 0)
		return NULL;

	for (i = 0; i < nhcd; i++) {
		if ((ops = hcd_lookup(info[i].type)) == NULL) {
			hcd_free(res);
			return NULL;
		}

		if ((hcd = hcd_create(ops, &info[i], num++)) == NULL) {
			hcd_free(res);
			return NULL;
		}

		if (hcd->ops->init(hcd) != 0) {
			hcd_free(res);
			free(hcd);
			return NULL;
		}

		LIST_ADD(&res, hcd);
	}

	return res;
}