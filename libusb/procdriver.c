/*
 * Phoenix-RTOS
 *
 * libusb/procdriver.c
 *
 * Copyright 2021, 2024, 2026 Phoenix Systems
 * Author: Maciej Purski, Adam Greloch, Julian Uziembło
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <assert.h>
#include <errno.h>
#include <sys/msg.h>
#include <sys/mman.h>
#include <sys/threads.h>
#include <sys/list.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#include <usbdriver.h>
#include <usbprocdriver.h>
#include <usbinternal.h>

#include "log.h"


#undef LIBUSB_LOG_TAG
#define LIBUSB_LOG_TAG "libusb(procdriver)"

#ifndef USB_UMSG_STACKSIZE
#define USB_UMSG_STACKSIZE 2048
#endif


static usb_pipeOps_t usbprocdrv_pipeOps;


static struct {
	unsigned int srvport;
	unsigned int drvport;
	usb_driver_t *driver;

	struct {
		handle_t tid;
		char *stack;
	} *threads;
	size_t nthreads;
} usbprocdrv_common;


static void usb_thread(void *arg)
{
	usb_driver_t *drv = (usb_driver_t *)arg;
	msg_t msg = { 0 };
	usb_msg_t *umsg = (usb_msg_t *)&msg.i.raw;
	usb_event_insertion_t event;
	int ret;
	msg_rid_t rid;

	for (;;) {
		do {
			ret = msgRecv(usbprocdrv_common.drvport, &msg, &rid);
			if (ret < 0 && ret != -EINTR) {
				break;
			}
		} while (ret == -EINTR);

		switch (umsg->type) {
			case usb_msg_insertion:
				msg.o.err = drv->handlers.insertion(drv, &umsg->insertion, &event);
				if (msg.o.err == 0) {
					memcpy(msg.o.raw, &event, sizeof(usb_event_insertion_t));
				}
				break;
			case usb_msg_deletion:
				drv->handlers.deletion(drv, &umsg->deletion);
				break;
			case usb_msg_completion:
				drv->handlers.completion(drv, &umsg->completion, msg.i.data, msg.i.size);
				break;
			default:
				log_error("unknown msg type\n");
				break;
		}

		ret = msgRespond(usbprocdrv_common.drvport, &msg, rid);
		if (ret < 0) {
			log_error("error when replying to host\n");
		}
	}

	endthread();
}


static int usb_connect(usb_driver_t *drv)
{
	msg_t msg = { 0 };
	usb_msg_t *umsg = (usb_msg_t *)&msg.i.raw;
	const usb_device_id_t *filters = drv->filters;
	unsigned int nfilters = drv->nfilters;
	int ret;

	msg.type = mtDevCtl;
	msg.i.size = sizeof(*filters) * nfilters;
	msg.i.data = (void *)filters;

	umsg->type = usb_msg_connect;
	umsg->connect.port = usbprocdrv_common.drvport;
	umsg->connect.nfilters = nfilters;
	memcpy(umsg->connect.name, drv->name, USB_DRVNAME_MAX);

	ret = msgSend(usbprocdrv_common.srvport, &msg);
	if (ret < 0) {
		return ret;
	}

	return msg.o.err;
}


int usb_driverThreadpoolDestroy(usb_driver_t *drv)
{
	int err;

	if (usbprocdrv_common.nthreads == 0) {
		return -EINVAL;
	}

#ifndef NDEBUG
	int tid = gettid();
	for (int i = 0; i < usbprocdrv_common.nthreads; i++) {
		assert(usbprocdrv_common.threads[i].tid != tid);
	}
#endif

	if (usbprocdrv_common.drvport != 0) {
		portDestroy(usbprocdrv_common.drvport);
	}

	if (usbprocdrv_common.threads != NULL && usbprocdrv_common.nthreads != 0) {
		for (int i = 0; i < usbprocdrv_common.nthreads; i++) {
			if (usbprocdrv_common.threads[i].tid != 0) {
				do {
					err = threadJoin(usbprocdrv_common.threads[i].tid, 0);
				} while (err == -EINTR);
			}

			if (usbprocdrv_common.threads[i].stack != NULL && usbprocdrv_common.threads[i].stack != MAP_FAILED) {
				munmap(usbprocdrv_common.threads[i].stack, USB_UMSG_STACKSIZE);
			}
		}
	}

	usbprocdrv_common.driver->ops.destroy(usbprocdrv_common.driver);

	free(usbprocdrv_common.threads);
	memset(&usbprocdrv_common, 0, sizeof(usbprocdrv_common));

	return 0;
}


int usb_driverThreadpoolSpawn(usb_driver_t *drv, int prio, unsigned int nthreads, void *args, unsigned int flags)
{
	oid_t oid;
	int ret, i;
	unsigned int newThreads;

	if (nthreads == 0) {
		return -EINVAL;
	}

	if (usbprocdrv_common.driver != NULL) {
		return -EALREADY;
	}

	usbprocdrv_common.driver = drv;

	/* usb_driverProcRun is invoked if drivers are in the proc variant */
	drv->pipeOps = &usbprocdrv_pipeOps;

	ret = drv->ops.init(drv, args);
	if (ret < 0) {
		log_error("%s: failed to initialize the driver [%d]", drv->name, ret);
		return ret;
	}

	usb_hostLookup(&oid);
	usbprocdrv_common.srvport = oid.port;

	ret = portCreate(&usbprocdrv_common.drvport);
	if (ret < 0) {
		log_error("%s: failed to create port [%d]", drv->name, ret);
		usb_driverThreadpoolDestroy(drv);
		return ret;
	}
	usbprocdrv_common.nthreads = nthreads;

	usbprocdrv_common.threads = calloc(nthreads, sizeof(*usbprocdrv_common.threads));
	if (usbprocdrv_common.threads == NULL) {
		log_error("%s: no memory for threads", drv->name);
		usb_driverThreadpoolDestroy(drv);
		return -ENOMEM;
	}

	newThreads = ((flags & USBPROCDRV_FLAG_REPLACE_CURRENT) != 0) ? (nthreads - 1) : nthreads;

	for (i = 0; i < newThreads; i++) {
		usbprocdrv_common.threads[i].stack = mmap(NULL, USB_UMSG_STACKSIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (usbprocdrv_common.threads[i].stack == MAP_FAILED) {
			log_error("%s: mmap failed", drv->name);
			usb_driverThreadpoolDestroy(drv);
			return -ENOMEM;
		}

		ret = beginthreadex(usb_thread, prio, usbprocdrv_common.threads[i].stack, USB_UMSG_STACKSIZE, drv, &usbprocdrv_common.threads[i].tid);
		if (ret < 0) {
			log_error("%s: failed to start thread [%d]\n", drv->name, ret);
			usb_driverThreadpoolDestroy(drv);
			return ret;
		}
	}

	if ((flags & USBPROCDRV_FLAG_REPLACE_CURRENT) != 0) {
		ret = setPriority(prio);
		if (ret < 0) {
			log_error("%s: failed to set priority to %d\n", drv->name, prio);
			usb_driverThreadpoolDestroy(drv);
			return ret;
		}
	}

	ret = usb_connect(drv);
	if (ret < 0) {
		log_error("%s: failed to connect to usbhost [%d]", drv->name, ret);
		usb_driverThreadpoolDestroy(drv);
		return ret;
	}

	if ((flags & USBPROCDRV_FLAG_REPLACE_CURRENT) != 0) {
		usbprocdrv_common.threads[i].tid = gettid();
		usb_thread(drv);
	}

	return 0;
}


__attribute__((noreturn)) void usb_driverProcRun(usb_driver_t *drv, int prio, unsigned int nthreads, void *args)
{
	exit(usb_driverThreadpoolSpawn(drv, prio, nthreads, args, USBPROCDRV_FLAG_REPLACE_CURRENT) < 0 ? 1 : 0);
}


static int usbprocdrv_open(usb_driver_t *drv, usb_devinfo_t *dev, usb_transfer_type_t type, usb_dir_t dir)
{
	msg_t msg = { 0 };
	usb_msg_t *umsg = (usb_msg_t *)msg.i.raw;
	int ret;

	msg.type = mtDevCtl;
	umsg->type = usb_msg_open;

	umsg->open.bus = dev->bus;
	umsg->open.dev = dev->dev;
	umsg->open.iface = dev->interface;
	umsg->open.type = type;
	umsg->open.dir = dir;
	umsg->open.locationID = dev->locationID;

	ret = msgSend(usbprocdrv_common.srvport, &msg);
	if (ret != 0) {
		return ret;
	}

	return msg.o.err;
}


static int usbprocdrv_urbSubmitSync(usb_driver_t *drv, usb_urb_t *urb, void *data)
{
	msg_t msg = { 0 };
	usb_msg_t *umsg = (usb_msg_t *)msg.i.raw;
	int ret;

	msg.type = mtDevCtl;
	umsg->type = usb_msg_urb;

	memcpy(&umsg->urb, urb, sizeof(usb_urb_t));

	if (urb->dir == usb_dir_out) {
		msg.i.data = data;
		msg.i.size = urb->size;
	}
	else {
		msg.o.data = data;
		msg.o.size = urb->size;
	}

	ret = msgSend(usbprocdrv_common.srvport, &msg);
	if (ret != 0) {
		return ret;
	}

	return msg.o.err;
}


static int usbprocdrv_urbAlloc(usb_driver_t *drv, unsigned pipe, void *data, usb_dir_t dir, size_t size, int type)
{
	msg_t msg = { 0 };
	usb_msg_t *umsg = (usb_msg_t *)msg.i.raw;
	int ret;
	usb_urb_t *urb = &umsg->urb;

	urb->pipe = pipe;
	urb->type = type;
	urb->dir = dir;
	urb->size = size;
	urb->sync = 0;

	msg.type = mtDevCtl;
	umsg->type = usb_msg_urb;

	ret = msgSend(usbprocdrv_common.srvport, &msg);
	if (ret < 0) {
		return ret;
	}

	/* URB id */
	return msg.o.err;
}


static int usbprocdrv_transferAsync(usb_driver_t *drv, unsigned pipe, unsigned urbid, size_t size, usb_setup_packet_t *setup)
{
	msg_t msg = { 0 };
	usb_msg_t *umsg = (usb_msg_t *)msg.i.raw;
	int ret;
	usb_urbcmd_t *urbcmd = &umsg->urbcmd;

	urbcmd->pipeid = pipe;
	urbcmd->size = size;
	urbcmd->urbid = urbid;
	urbcmd->cmd = urbcmd_submit;

	if (setup != NULL) {
		memcpy(&urbcmd->setup, setup, sizeof(*setup));
	}
	msg.type = mtDevCtl;
	umsg->type = usb_msg_urbcmd;
	ret = msgSend(usbprocdrv_common.srvport, &msg);
	if (ret < 0) {
		return ret;
	}

	return 0;
}


static int usbprocdrv_urbFree(usb_driver_t *drv, unsigned pipe, unsigned urb)
{
	msg_t msg = { 0 };
	usb_msg_t *umsg = (usb_msg_t *)msg.i.raw;
	int ret;
	usb_urbcmd_t *urbcmd = &umsg->urbcmd;

	urbcmd->pipeid = pipe;
	urbcmd->urbid = urb;
	urbcmd->cmd = urbcmd_free;

	msg.type = mtDevCtl;
	umsg->type = usb_msg_urbcmd;

	ret = msgSend(usbprocdrv_common.srvport, &msg);
	if (ret < 0) {
		return ret;
	}

	return 0;
}


static usb_pipeOps_t usbprocdrv_pipeOps = {
	.open = usbprocdrv_open,
	.submitSync = usbprocdrv_urbSubmitSync,
	.transferAsync = usbprocdrv_transferAsync,

	.urbFree = usbprocdrv_urbFree,
	.urbAlloc = usbprocdrv_urbAlloc,
};
