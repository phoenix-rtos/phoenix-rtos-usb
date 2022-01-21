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
#include <time.h>

#include "usbhost.h"
#include "dev.h"
#include "drv.h"
#include "hub.h"
#include "hcd.h"


#define HCD_REFRESH_US 100000 /* 100 ms */


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
	resourceDestroy(hcd->blockCond);
	resourceDestroy(hcd->transCond);
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

	if (condCreate(&hcd->transCond) != 0) {
		resourceDestroy(hcd->transLock);
		free(hcd);
		return NULL;
	}

	if (condCreate(&hcd->blockCond) != 0) {
		resourceDestroy(hcd->transLock);
		resourceDestroy(hcd->transCond);
		free(hcd);
		return NULL;
	}

	hcd->info = info;
	hcd->priv = NULL;
	hcd->transfers = NULL;
	hcd->finished = NULL;
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


void hcd_portStatus(hcd_t *hcd)
{
	usb_dev_t *hub = hcd->roothub;
	uint32_t status;

	status = hcd->ops->getRoothubStatus(hub);
	mutexLock(hcd->transLock);
	if (!hub->statusTransfer->finished) {
		memcpy(hub->statusTransfer->buffer, &status, sizeof(status));
		hub->statusTransfer->finished = 1;
		hub->statusTransfer->error = 0;
		hub->statusTransfer->transferred = sizeof(status);
		hub_notify(hub);
	}
	mutexUnlock(hcd->transLock);
}


void hcd_notify(hcd_t *hcd)
{
	condSignal(hcd->transCond);
}


int hcd_transfer(hcd_t *hcd, usb_transfer_t *t, int block)
{
	usb_dev_t *dev = t->pipe->dev;
	int ret;

	mutexLock(hcd->transLock);
	t->finished = 0;
	t->error = 0;
	t->transferred = 0;
	t->elapsed = 0;
	t->block = block;
	if (t->direction == usb_dir_in)
		memset(t->buffer, 0, t->size);

	if (usb_isRoothub(dev)) {
		if (dev != hcd->roothub)
			ret = -EINVAL;
		else
			ret = hcd->ops->roothubTransfer(hcd, t);
	}
	else {
		ret = hcd->ops->transferEnqueue(hcd, t);
		if (ret == 0)
			LIST_ADD(&hcd->transfers, t);
	}

	if (block) {
		while (!t->finished)
			condWait(hcd->blockCond, hcd->transLock, 0);
	}
	mutexUnlock(hcd->transLock);

	return ret;
}


static void _hcd_update(hcd_t *hcd, time_t diff)
{
	usb_transfer_t *t, *n;

	t = hcd->transfers;
	if (t == NULL)
		return;

	do {
		n = t->next;

		if (t->timeout > 0) {
			t->elapsed += diff;
		}

		if (t->elapsed > t->timeout && !t->finished) {
			t->finished = 1;
			t->error = -ETIMEDOUT;
			hcd->ops->transferDequeue(hcd, t);
			printf("TRANSFER TIMEOUT dir: %d\n", t->direction);
		}

		if (t->finished) {
			LIST_REMOVE(&hcd->transfers, t);
			LIST_ADD(&hcd->finished, t);
		}

		t = n;
	} while (hcd->transfers != NULL && t != hcd->transfers);
}


static void hcd_thread(void *arg)
{
	hcd_t *hcd = (hcd_t *)arg;
	usb_transfer_t *t;
	time_t diff;
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	hcd->time = ts.tv_sec * 1000 + ts.tv_nsec / 1000000;

	while (1) {
		mutexLock(hcd->transLock);

		condWait(hcd->transCond, hcd->transLock, HCD_REFRESH_US);

		clock_gettime(CLOCK_MONOTONIC, &ts);
		diff = ts.tv_sec * 1000 + ts.tv_nsec / 1000000 - hcd->time;
		hcd->time += diff;

		/* Update timeout, move transfers to finished list */
		_hcd_update(hcd, diff);

		mutexUnlock(hcd->transLock);

		/*
		 * Empty finished queue out of the critical section in order to
		 * prevent blocking adding to transfer queue, while waiting on
		 * msgRespond to driver's process.
		 */
		while ((t = hcd->finished) != NULL) {
			LIST_REMOVE(&hcd->finished, t);
			if (t->block)
				condSignal(hcd->blockCond);
			else if (t->msg)
				usb_drvRespond(t);
			else if (t->type == usb_transfer_interrupt)
				hub_notify(t->pipe->dev);
		}
	}
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
			fprintf(stderr, "usb-hcd: No ops found for hcd type %s\n", info[i].type);
			continue;
		}

		if ((hcd = hcd_create(ops, &info[i], num++)) == NULL) {
			fprintf(stderr, "usb-hcd: Not enough memory to allocate hcd type: %s\n", info[i].type);
			return res;
		}

		if (hcd->ops->init(hcd) != 0) {
			fprintf(stderr, "usb-hcd: Fail to initialize hcd type: %s\n", info[i].type);
			hcd_free(hcd);
			continue;
		}

		if (hcd_roothubInit(hcd) != 0) {
			fprintf(stderr, "usb-hcd: Fail to initialize roothub: %s\n", info[i].type);
			hcd_free(hcd);
			continue;
		}

		LIST_ADD(&res, hcd);

		beginthread(hcd_thread, 4, hcd->stack, sizeof(hcd->stack), hcd);
	}

	return res;
}
