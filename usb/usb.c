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


static struct {
	handle_t lock;
	hcd_t *hcds;
	uint32_t port;
} usb_common;


static int usb_devsList(char *buffer, size_t size)
{
	return 0;
}


static int usb_handleUrb(msg_t *msg, unsigned int port, unsigned long rid)
{
	usb_msg_t *umsg = (usb_msg_t *)msg->i.raw;
	usb_drv_t *drv;
	usb_transfer_t *t;

	if ((drv = usb_drvFind(msg->pid)) == NULL) {
		fprintf(stderr, "usb: driver pid %d does not exist!\n", drv->pid);
		return -EINVAL;
	}

	if ((t = calloc(1, sizeof(usb_transfer_t))) == NULL)
		return -ENOMEM;

	t->direction = umsg->urb.dir;
	t->transferred = 0;
	t->size = umsg->urb.size;
	t->timeout = umsg->urb.timeout;

	if ((t->msg = malloc(sizeof(msg_t))) == NULL) {
		free(t);
		return -ENOMEM;
	}

	if (t->size > 0) {
		if ((t->buffer = usb_alloc(t->size)) == NULL) {
			free(t->msg);
			free(t);
			return -ENOMEM;
		}
	}

	if (t->type == usb_transfer_control) {
		t->setup = usb_alloc(sizeof(usb_setup_packet_t));
		memcpy(t->setup, &umsg->urb.setup, sizeof(usb_setup_packet_t));
	}

	if (t->direction == usb_dir_out && t->size > 0)
		memcpy(t->buffer, msg->i.data, t->size);

	memcpy(t->msg, msg, sizeof(msg_t));
	t->port = port;
	t->rid = rid;

	if (usb_drvTransfer(drv, t, umsg->urb.pipe) != 0) {
		usb_free(t->buffer, t->size);
		usb_free(t->setup, sizeof(usb_setup_packet_t));
		free(t->msg);
		free(t);
		return -EINVAL;
	}

	return EOK;
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
		fprintf(stderr, "usb: Fail to find driver pid: %d\n", msg->pid);
		return -EINVAL;
	}

	if ((hcd = hcd_find(usb_common.hcds, o->locationID)) == NULL) {
		fprintf(stderr, "usb: Fail to find dev: %d\n", o->dev);
		return -EINVAL;
	}

	if ((pipe = usb_drvPipeOpen(drv, hcd, o->locationID, o->iface, o->dir, o->type)) < 0)
		return -EINVAL;

	*(int *)msg->o.raw = pipe;

	return 0;
}


static void usb_msgthr(void *arg)
{
	unsigned port = (int)arg;
	unsigned long rid;
	msg_t msg;
	usb_msg_t *umsg;
	int resp;
	int ret;

	for (;;) {
		if (msgRecv(port, &msg, &rid) < 0)
			continue;
		resp = 1;
		mutexLock(usb_common.lock);
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
						if ((ret = usb_handleUrb(&msg, port, rid)) != 0)
							msg.o.io.err = ret;
						else
							resp = 0;
						break;
					default:
						msg.o.io.err = -EINVAL;
						fprintf(stderr, "usb: unsupported usb_msg type\n");
						break;
				}
				break;
			default:
				msg.o.io.err = -EINVAL;
				fprintf(stderr, "usb: unsupported msg type\n");
		}
		mutexUnlock(usb_common.lock);

		if (resp)
			msgRespond(port, &msg, rid);
	}
}


int main(int argc, char *argv[])
{
	oid_t oid;

	if (mutexCreate(&usb_common.lock) != 0) {
		fprintf(stderr, "usb: Can't create mutex!\n");
		return 1;
	}

	if (usb_memInit() != 0) {
		fprintf(stderr, "usb: Can't initiate memory management!\n");
		return 1;
	}

	if (usb_devInit() != 0) {
		fprintf(stderr, "usb: Fail to init devices!\n");
		return 1;
	}

	if (hub_init() != 0) {
		fprintf(stderr, "usb: Fail to init hub driver!\n");
		return 1;
	}

	if ((usb_common.hcds = hcd_init()) == NULL) {
		fprintf(stderr, "usb: Fail to init hcds!\n");
		return 1;
	}

	if (portCreate(&usb_common.port) != 0) {
		fprintf(stderr, "usb: Can't create port!\n");
		return 1;
	}

	oid.port = usb_common.port;
	oid.id = 0;

	if (create_dev(&oid, "/dev/usb") != 0) {
		fprintf(stderr, "usb: Can't create dev!\n");
		return 1;
	}

	usb_msgthr((void *)usb_common.port);

	return 0;
}
