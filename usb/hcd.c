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

#include <errno.h>
#include <sys/list.h>
#include <sys/threads.h>
#include <string.h>
#include <stdlib.h>

#include "usbhost.h"
#include "dev.h"
#include "hub.h"
#include "hcd.h"

struct hcd_ops_node {
	struct hcd_ops_node *prev, *next;
	const hcd_ops_t *ops;
};


static struct {
	struct hcd_ops_node *ops;
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


void hcd_register(const hcd_ops_t *ops)
{
	struct hcd_ops_node *node;

	if ((node = malloc(sizeof(*node))) == NULL)
		return;

	node->ops = ops;
	LIST_ADD(&hcd_common.ops, node);
}


static const hcd_ops_t *hcd_lookup(const char *type)
{
	struct hcd_ops_node *node = hcd_common.ops;

	if (node == NULL)
		return NULL;

	do {
		if (!strcmp(type, node->ops->type))
			return node->ops;
		node = node->next;
	} while (node != hcd_common.ops);

	return NULL;
}


static void hcd_free(hcd_t *hcd)
{
	resourceDestroy(hcd->transLock);
	free(hcd);
}

static hcd_t *hcd_create(const hcd_ops_t *ops, const hcd_info_t *info, int num)
{
	hcd_t *hcd;

	if ((hcd = malloc(sizeof(hcd_t))) == NULL)
		return NULL;

	if (mutexCreate(&hcd->transLock) != 0) {
		free(hcd);
		return NULL;
	}

	hcd->info = info;
	hcd->priv = NULL;
	hcd->transfers = NULL;
	hcd->ops = ops;
	hcd->num = num;

	/*
	 * 0 address is reserved for the enumerating device,
	 */
	hcd->addrmask[0] = 0x1;
	hcd->addrmask[1] = 0;
	hcd->addrmask[2] = 0;
	hcd->addrmask[3] = 0;

	return hcd;
}


hcd_t *hcd_find(hcd_t *hcdList, uint32_t locationID)
{
	hcd_t *hcd = hcdList;

	do {
		if ((locationID & 0xf) == hcd->num)
			break;
	} while ((hcd = hcd->next) != hcdList);

	if ((locationID & 0xf) != hcd->num)
		return NULL;

	return hcd;
}


static int hcd_roothubInit(hcd_t *hcd)
{
	usb_dev_t *hub;

	if ((hub = usb_devAlloc()) == NULL)
		return -ENOMEM;

	hcd->roothub = hub;
	hub->hub = NULL;
	hub->port = 1;
	hub->hcd = hcd;

	return usb_devEnumerate(hub);
}


hcd_t *hcd_init(void)
{
	const hcd_info_t *info;
	const hcd_ops_t *ops;
	hcd_t *hcd, *res = NULL;
	int nhcd, i;
	int num = 1;

	if ((nhcd = hcd_getInfo(&info)) <= 0)
		return NULL;

	for (i = 0; i < nhcd; i++) {
		if ((ops = hcd_lookup(info[i].type)) == NULL) {
			USB_LOG("usb-hcd: No ops found for hcd type %s\n", info[i].type);
			continue;
		}

		if ((hcd = hcd_create(ops, &info[i], num++)) == NULL) {
			USB_LOG("usb-hcd: Not enough memory to allocate hcd type: %s\n", info[i].type);
			return res;
		}

		if (hcd->ops->init(hcd) != 0) {
			USB_LOG("usb-hcd: Fail to initialize hcd type: %s\n", info[i].type);
			hcd_free(hcd);
			continue;
		}

		if (hcd_roothubInit(hcd) != 0) {
			USB_LOG("usb-hcd: Fail to initialize roothub: %s\n", info[i].type);
			hcd_free(hcd);
			continue;
		}

		LIST_ADD(&res, hcd);
	}

	return res;
}
