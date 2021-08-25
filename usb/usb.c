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

static struct {
	handle_t commonLock, finishedLock, drvsLock;
	handle_t finishedCond;
	hcd_t *hcds;
	usb_driver_t *drvs;
	usb_transfer_t *finished;
	int nhcd;
	uint32_t port;
	idtree_t pipes;
} usb_common;


static void usb_handleUrbTransfer(usb_transfer_t *t)
{
	mutexLock(usb_common.finishedLock);
	LIST_ADD(&usb_common.finished, t);
	condSignal(usb_common.finishedCond);
	mutexUnlock(usb_common.finishedLock);
}


static int usb_devsList(char *buffer, size_t size)
{
	return 0;
}


static usb_iface_t *usb_ifaceFind(usb_dev_t *dev, int num)
{
	/* TODO: it shall not be needed, we should remove iface structure */
	if (num >= dev->nifs)
		return NULL;

	return &dev->ifs[num];
}


static usb_pipe_t *usb_pipeFind(int pipe)
{
	return lib_treeof(usb_pipe_t, linkage, idtree_find(&usb_common.pipes, pipe));
}


static int usb_handleUrb(msg_t *msg, unsigned int port, unsigned long rid)
{
	usb_msg_t *umsg = (usb_msg_t *)msg->i.raw;
	hcd_t *hcd;
	usb_pipe_t *pipe;
	usb_transfer_t *t;
	int ret = 0;

	if ((pipe = usb_pipeFind(umsg->urb.pipe)) == NULL) {
		fprintf(stderr, "usb: Fail to find pipe: %d\n", umsg->urb.pipe);
		return -EINVAL;
	}

	if (pipe->drv->pid != msg->pid) {
		fprintf(stderr, "usb: driver pid (%d) and msg pid (%d) mismatch\n", pipe->drv->pid, msg->pid);
		return -EINVAL;
	}

	if ((t = calloc(1, sizeof(usb_transfer_t))) == NULL)
		return -ENOMEM;

	t->type = pipe->ep->type;
	t->direction = umsg->urb.dir;
	t->ep = pipe->ep;
	t->handler = usb_handleUrbTransfer;
	t->setup = &umsg->urb.setup;
	t->size = umsg->urb.size;
	t->buffer = msg->i.data;
	t->transfered = 0;

	if ((t->msg = malloc(sizeof(msg_t))) == NULL) {
		free(t);
		return -ENOMEM;
	}

	memcpy(t->msg, msg, sizeof(msg_t));
	t->port = port;
	t->rid = rid;

	hcd = pipe->ep->device->hcd;
	if ((ret = hcd->ops->transferEnqueue(hcd, t)) != 0) {
		free(t->msg);
		free(t);
	}

	return ret;
}


static int usb_handleConnect(msg_t *msg, usb_connect_t *c)
{
	usb_driver_t *drv;

	if ((drv = malloc(sizeof(usb_driver_t))) == NULL)
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
	usb_dev_t *dev;
	usb_iface_t *iface;
	usb_driver_t *drv;
	usb_pipe_t *pipe = NULL;
	usb_endpoint_t *ep;

	if ((drv = usb_drvFind(msg->pid)) == NULL) {
		fprintf(stderr, "usb: Fail to find driver pid: %d\n", msg->pid);
		return -EINVAL;
	}

	if ((dev = hcd_devFind(usb_common.hcds, o->locationID)) == NULL) {
		fprintf(stderr, "usb: Fail to find dev: %d\n", o->dev);
		return -EINVAL;
	}

	if ((iface = usb_ifaceFind(dev, o->iface)) == NULL) {
		fprintf(stderr, "usb: Fail to find iface: %d dev: %d\n", o->iface, dev->address);
		return -EINVAL;
	}

	if (iface->driver != drv) {
		fprintf(stderr, "usb: Interface and driver mismatch\n");
		return -EINVAL;
	}

	ep = dev->eps;
	do {
		if (ep->direction == o->dir && ep->type == o->type) {
			if (ep->pipe != NULL && ep->type == usb_transfer_control) {
				pipe = ep->pipe;
				break;
			}
			else if (ep->pipe == NULL && (ep->interface == o->iface || ep->type == usb_transfer_control)) {
				if ((pipe = malloc(sizeof(usb_pipe_t))) == NULL)
					return -ENOMEM;
				idtree_alloc(&usb_common.pipes, &pipe->linkage);
				ep->pipe = pipe;
				break;
			}
		}
		ep = ep->next;
	} while (ep != dev->eps);

	if (pipe == NULL)
		return -EINVAL;

	pipe->ep = ep;
	pipe->drv = drv;
	*(int *)msg->o.raw = pipe->linkage.id;

	return 0;
}


void usb_pipeFree(usb_pipe_t *pipe)
{
	idtree_remove(&usb_common.pipes, &pipe->linkage);
	free(pipe);
}


static void usb_statusthr(void *arg)
{
	usb_transfer_t *t;

	for (;;) {
		mutexLock(usb_common.finishedLock);
		while (usb_common.finished == NULL)
			condWait(usb_common.finishedCond, usb_common.finishedLock, 0);
		t = usb_common.finished;
		LIST_REMOVE(&usb_common.finished, t);
		mutexUnlock(usb_common.finishedLock);

		if (t->type == usb_transfer_bulk || t->type == usb_transfer_control) {
			t->msg->o.io.err = (t->error != 0) ? -t->error : t->transfered;
			/* TODO: it should be non-blocking */
			msgRespond(t->port, t->msg, t->rid);
			free(t->msg);
			free(t);
		}
	}
}


static void usb_msgthr(void *arg)
{
	unsigned port = (int)arg;
	unsigned long rid;
	msg_t msg;
	usb_msg_t *umsg;
	int resp = 1;
	int ret;

	for (;;) {
		if (msgRecv(port, &msg, &rid) < 0)
			continue;
		mutexLock(usb_common.commonLock);
		switch (msg.type) {
			case mtRead:
				msg.o.io.err = usb_devsList(msg.o.data, msg.o.size);
				break;
			case mtDevCtl:
				umsg = (usb_msg_t *)msg.i.raw;
				switch (umsg->type) {
					case usb_msg_connect:
						msg.o.io.err = usb_handleConnect(&msg, &umsg->connect);
						resp = 1;
						break;
					case usb_msg_open:
						if (usb_handleOpen(&umsg->open, &msg) != 0)
							msg.o.io.err = -1;
						resp = 1;
						break;
					case usb_msg_urb:
						if ((ret = usb_handleUrb(&msg, port, rid)) != 0) {
							msg.o.io.err = ret;
							resp = 1;
						} else {
							resp = 0;
						}
						break;
					default:
						fprintf(stderr, "unsupported usb_msg type\n");
						break;
				}
				break;
			default:
				fprintf(stderr, "usb: unsupported msg type\n");
				msg.o.io.err = -EINVAL;
		}
		mutexUnlock(usb_common.commonLock);

		if (resp)
			msgRespond(port, &msg, rid);
	}
}

static int usb_roothubsInit(void)
{
	usb_dev_t *hub;
	hcd_t *hcd = usb_common.hcds;

	do {
		if ((hub = usb_devAlloc()) == NULL)
			return -ENOMEM;

		hcd->roothub = hub;
		hub->address = 1;
		hub->hub = NULL;
		hub->locationID = hcd->num & 0xf;
		hub->port = 1;
		hub->hcd = hcd;
		hub_add(hub);
	} while ((hcd = hcd->next) != usb_common.hcds);

	return 0;
}


int main(int argc, char *argv[])
{
	oid_t oid;

	if (mutexCreate(&usb_common.commonLock) != 0) {
		fprintf(stderr, "usb: Can't create mutex!\n");
		return -EINVAL;
	}

	if (mutexCreate(&usb_common.finishedLock) != 0) {
		fprintf(stderr, "usb: Can't create mutex!\n");
		return -EINVAL;
	}

	if (condCreate(&usb_common.finishedCond) != 0) {
		fprintf(stderr, "usb: Can't create mutex!\n");
		return -EINVAL;
	}

	if (usb_devInit() != 0) {
		fprintf(stderr, "usb: Fail to init devices!\n");
		return -EINVAL;
	}

	if ((usb_common.hcds = hcd_init()) == NULL) {
		fprintf(stderr, "usb: Fail to init hcds!\n");
		return -EINVAL;
	}

	if (portCreate(&usb_common.port) != 0) {
		fprintf(stderr, "usb: Can't create port!\n");
		return -EINVAL;
	}

	idtree_init(&usb_common.pipes);

	oid.port = usb_common.port;
	oid.id = 0;

	if (create_dev(&oid, "/dev/usb") != 0) {
		fprintf(stderr, "usb: Can't create dev!\n");
		return -EINVAL;
	}

	if (usb_roothubsInit() != 0) {
		fprintf(stderr, "usb: Fail to init roothubs!\n");
		return -EINVAL;
	}

	if (hub_init() != 0) {
		fprintf(stderr, "usb: Fail to init hub driver!\n");
		return -EINVAL;
	}

	beginthread(usb_statusthr, 4, malloc(0x1000), 0x1000, NULL);

	usb_msgthr((void *)usb_common.port);

	return 0;
}