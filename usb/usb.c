/*
 * Phoenix-RTOS
 *
 * USB host stack
 *
 * Copyright 2021 Phoenix Systems
 * Author: Maciej Purski
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include <stdint.h>

#include <errno.h>
#include <sys/list.h>
#include <sys/mman.h>
#include <sys/msg.h>
#include <sys/platform.h>
#include <sys/types.h>
#include <sys/threads.h>
#include <posix/utils.h>

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <usbdriver.h>

#include "drv.h"
#include "hcd.h"
#include "hub.h"


#define N_STATUSTHRS   1
#define STATUSTHR_PRIO 3
#define MSGTHR_PRIO    3


static struct {
	char stack[N_STATUSTHRS][2048] __attribute__((aligned(8)));
	handle_t transferLock;
	handle_t finishedCond;
	hcd_t *hcds;
	usb_drv_t *drvs;
	usb_transfer_t *finished;
	int nhcd;
	uint32_t port;
} usb_common;


int usb_transferCheck(usb_transfer_t *t)
{
	int val;

	mutexLock(usb_common.transferLock);
	val = t->finished;
	mutexUnlock(usb_common.transferLock);

	return val;
}


int usb_transferSubmit(usb_transfer_t *t, usb_pipe_t *pipe, handle_t *cond)
{
	hcd_t *hcd = pipe->dev->hcd;
	int ret = 0;

	mutexLock(usb_common.transferLock);
	t->finished = 0;
	t->error = 0;
	t->transferred = 0;
	if (t->direction == usb_dir_in)
		memset(t->buffer, 0, t->size);
	mutexUnlock(usb_common.transferLock);

	if ((ret = hcd->ops->transferEnqueue(hcd, t, pipe)) != 0)
		return ret;

	/* Internal blocking transfer */
	if (cond != NULL) {
		mutexLock(usb_common.transferLock);

		while (!t->finished)
			condWait(*cond, usb_common.transferLock, 0);
		mutexUnlock(usb_common.transferLock);
	}

	return ret;
}


/* Called by the hcd driver */
void usb_transferFinished(usb_transfer_t *t, int status)
{
	int urbtrans = 0;

	mutexLock(usb_common.transferLock);
	t->finished = 1;

	if (status >= 0) {
		t->transferred = status;
		t->error = 0;
	}
	else {
		t->transferred = 0;
		t->error = -status;
	}

	/* URB transfer */
	if (t->port != 0) {
		urbtrans = 1;
		t->state = urb_completed;
		LIST_ADD(&usb_common.finished, t);
	}

	mutexUnlock(usb_common.transferLock);

	/* Internal transfer */
	if (!urbtrans) {
		if (t->type == usb_transfer_interrupt && t->transferred > 0)
			hub_notify(t->hub);
		else
			usb_devSignal();
	}
	else {
		/* URB transfer */
		condSignal(usb_common.finishedCond);
	}
}


static int usb_devsList(char *buffer, size_t size)
{
	return 0;
}



static int usb_handleConnect(msg_t *msg, usb_connect_t *c)
{
	usb_drv_t *drv;

	if ((drv = malloc(sizeof(usb_drv_t))) == NULL)
		return -ENOMEM;

	if ((drv->filters = malloc(sizeof(usb_device_id_t) * c->nfilters)) == NULL) {
		free(drv);
		return -ENOMEM;
	}

	drv->pid = msg->pid;
	drv->port = c->port;
	drv->nfilters = c->nfilters;
	memcpy(drv->filters, msg->i.data, msg->i.size);
	usb_drvAdd(drv);

	/* TODO: handle orphaned devices */

	return 0;
}


static int usb_handleOpen(usb_open_t *o, msg_t *msg)
{
	usb_drv_t *drv;
	int pipe;
	hcd_t *hcd;

	if ((drv = usb_drvFind(msg->pid)) == NULL) {
		USB_LOG("usb: Fail to find driver pid: %d\n", msg->pid);
		return -EINVAL;
	}

	if ((hcd = hcd_find(usb_common.hcds, o->locationID)) == NULL) {
		USB_LOG("usb: Fail to find dev: %d\n", o->dev);
		return -EINVAL;
	}

	if ((pipe = usb_drvPipeOpen(drv, hcd, o->locationID, o->iface, o->dir, o->type)) < 0)
		return -EINVAL;

	*(int *)msg->o.raw = pipe;

	return 0;
}


static void usb_urbAsyncCompleted(usb_transfer_t *t)
{
	msg_t msg = { 0 };
	usb_msg_t *umsg = (usb_msg_t *)&msg.i.raw;
	usb_completion_t *c = &umsg->completion;

	umsg->type = usb_msg_completion;

	c->pipeid = t->pipeid;
	c->urbid = t->linkage.id;
	c->transferred = t->transferred;
	c->err = t->error;

	msg.type = mtDevCtl;
	if (t->direction == usb_dir_in) {
		msg.i.size = t->transferred;
		msg.i.data = t->buffer;
	}
	t->state = urb_idle;

	msgSend(t->port, &msg);
	usb_transferPut(t);
}


static void usb_urbSyncCompleted(usb_transfer_t *t)
{
	msg_t msg = { 0 };

	msg.type = mtDevCtl;
	msg.pid = t->pid;
	msg.o.io.err = (t->error != 0) ? -t->error : t->transferred;

	if (t->direction == usb_dir_in)
		msg.o.data = t->buffer;

	/* TODO: it should be non-blocking */
	msgRespond(usb_common.port, &msg, t->rid);
	usb_transferFree(t);
}


static void usb_statusthr(void *arg)
{
	usb_transfer_t *t;

	for (;;) {
		mutexLock(usb_common.transferLock);
		while (usb_common.finished == NULL)
			condWait(usb_common.finishedCond, usb_common.transferLock, 0);
		t = usb_common.finished;
		LIST_REMOVE(&usb_common.finished, t);
		mutexUnlock(usb_common.transferLock);

		if (t->async)
			usb_urbAsyncCompleted(t);
		else
			usb_urbSyncCompleted(t);
	}
}


static void usb_msgthr(void *arg)
{
	unsigned port = (int)arg;
	msg_rid_t rid;
	msg_t msg;
	usb_msg_t *umsg;
	int resp;
	int ret;

	for (;;) {
		if (msgRecv(port, &msg, &rid) < 0)
			continue;
		resp = 1;
		switch (msg.type) {
			case mtRead:
				msg.o.io.err = usb_devsList(msg.o.data, msg.o.size);
				break;
			case mtDevCtl:
				umsg = (usb_msg_t *)msg.i.raw;
				switch (umsg->type) {
					case usb_msg_connect:
						msg.o.io.err = usb_handleConnect(&msg, &umsg->connect);
						break;
					case usb_msg_open:
						if (usb_handleOpen(&umsg->open, &msg) != 0)
							msg.o.io.err = -1;
						break;
					case usb_msg_urb:
						ret = usb_handleUrb(&msg, port, rid);
						if (umsg->urb.sync && ret == 0) {
							/* Block the sender until the transfer finishes */
							resp = 0;
						}
						else {
							msg.o.io.err = ret;
						}
						break;
					case usb_msg_urbcmd:
						msg.o.io.err = usb_handleUrbcmd(&msg);
						break;
					default:
						msg.o.io.err = -EINVAL;
						USB_LOG("usb: unsupported usb_msg type: %d\n", umsg->type);
						break;
				}
				break;
			default:
				msg.o.io.err = -EINVAL;
				USB_LOG("usb: unsupported msg type\n");
		}

		if (resp)
			msgRespond(port, &msg, rid);
	}
}


int main(int argc, char *argv[])
{
	oid_t oid;
	int i;

	if (mutexCreate(&usb_common.transferLock) != 0) {
		USB_LOG("usb: Can't create mutex!\n");
		return 1;
	}

	if (condCreate(&usb_common.finishedCond) != 0) {
		USB_LOG("usb: Can't create mutex!\n");
		return 1;
	}

	if (usb_memInit() != 0) {
		USB_LOG("usb: Can't initiate memory management!\n");
		return 1;
	}

	if (usb_devInit() != 0) {
		USB_LOG("usb: Fail to init devices!\n");
		return 1;
	}

	if (hub_init() != 0) {
		USB_LOG("usb: Fail to init hub driver!\n");
		return 1;
	}

	if ((usb_common.hcds = hcd_init()) == NULL) {
		USB_LOG("usb: Fail to init hcds!\n");
		return 1;
	}

	if (portCreate(&usb_common.port) != 0) {
		USB_LOG("usb: Can't create port!\n");
		return 1;
	}

	oid.port = usb_common.port;
	oid.id = 0;

	if (create_dev(&oid, "/dev/usb") != 0) {
		USB_LOG("usb: Can't create dev!\n");
		return 1;
	}

	for (i = 0; i < N_STATUSTHRS; i++) {
		if (beginthread(usb_statusthr, STATUSTHR_PRIO, &usb_common.stack[i], sizeof(usb_common.stack[i]), NULL) != 0) {
			USB_LOG("usb: Fail to init hub driver!\n");
			return 1;
		}
	}

	priority(MSGTHR_PRIO);

	usb_msgthr((void *)usb_common.port);

	return 0;
}
