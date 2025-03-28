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
#include <sys/platform.h>
#include <sys/types.h>
#include <sys/threads.h>
#include <sys/minmax.h>
#include <posix/utils.h>

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <usbdriver.h>
#include <sys/msg.h>

#include "drv.h"
#include "hcd.h"
#include "hub.h"
#include "log.h"

#define N_STATUSTHRS   1
#define STATUSTHR_PRIO 3
#define MSGTHR_PRIO    3


static struct {
	char ustack[2048] __attribute__((aligned(8)));
	uint32_t port;
	char stack[N_STATUSTHRS - 1][2048] __attribute__((aligned(8)));
	handle_t transferLock;
	handle_t finishedCond;
	hcd_t *hcds;
	usb_drvpriv_t *drvs;
	usb_transfer_t *finished;
	int nhcd;
} usb_common;


static int usb_internalDriverInit(usb_driver_t *driver)
{
	usb_drvpriv_t *priv;
	int ret;

	ret = usb_libDrvInit(driver);
	if (ret < 0) {
		return ret;
	}

	priv = malloc(sizeof(usb_drvpriv_t));
	if (priv == NULL) {
		log_error("malloc failed!\n");
		usb_libDrvDestroy(driver);
		return -ENOMEM;
	}

	priv->type = usb_drvType_intrn;

	ret = mutexCreate(&priv->intrn.transferLock);
	if (ret != 0) {
		log_error("Can't create mutex!\n");
		free(priv);
		usb_libDrvDestroy(driver);
		return -ENOMEM;
	}

	ret = condCreate(&priv->intrn.finishedCond);
	if (ret != 0) {
		log_error("Can't create cond!\n");
		resourceDestroy(priv->intrn.transferLock);
		free(priv);
		usb_libDrvDestroy(driver);
		return -ENOMEM;
	}

	driver->hostPriv = (void *)priv;
	priv->driver = *driver;

	usb_drvAdd(priv);

	return 0;
}


bool usb_transferCheck(usb_transfer_t *t)
{
	bool val;

	mutexLock(usb_common.transferLock);
	val = t->finished;
	mutexUnlock(usb_common.transferLock);

	return val;
}


int usb_transferSubmit(usb_transfer_t *t, usb_pipe_t *pipe, handle_t *cond)
{
	hcd_t *hcd = pipe->dev->hcd;
	int ret = 0;

	if (t->recipient == usb_drvType_none) {
		log_error("transfer recipient unspecified!\n");
		return -EINVAL;
	}

	mutexLock(usb_common.transferLock);
	t->finished = false;
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
	bool urbtrans = false;

	mutexLock(usb_common.transferLock);
	t->finished = true;

	if (status >= 0) {
		t->transferred = status;
		t->error = 0;
	}
	else {
		t->transferred = 0;
		t->error = -status;
	}

	/* If recipient is not HCD, this is an URB transfer */
	if (t->recipient != usb_drvType_hcd) {
		urbtrans = true;
		t->state = urb_completed;
		LIST_ADD(&usb_common.finished, t);
	}

	mutexUnlock(usb_common.transferLock);

	if (urbtrans) {
		condSignal(usb_common.finishedCond);
	}
	else {
		/* Internal HCD transfer */
		if (t->type == usb_transfer_interrupt && t->transferred > 0) {
			hub_notify(t->hub);
		}
		else {
			usb_devSignal();
		}
	}
}


static int usb_devsList(char *buffer, size_t size)
{
	return 0;
}


#ifndef USB_INTERNAL_ONLY
static int usb_handleConnect(msg_t *msg, usb_connect_t *c)
{
	usb_drvpriv_t *drv;

	drv = malloc(sizeof(usb_drvpriv_t));
	if (drv == NULL) {
		return -ENOMEM;
	}

	drv->driver.filters = malloc(sizeof(usb_device_id_t) * c->nfilters);
	if (drv->driver.filters == NULL) {
		free(drv);
		return -ENOMEM;
	}

	drv->type = usb_drvType_extrn;
	drv->extrn.id = msg->pid;
	drv->extrn.port = c->port;
	drv->driver.nfilters = c->nfilters;
	memcpy(drv->driver.name, c->name, sizeof(char) * USB_DRVNAME_MAX);
	memcpy((void *)drv->driver.filters, msg->i.data, msg->i.size);
	usb_drvAdd(drv);

	/* TODO: handle orphaned devices */

	return 0;
}


static int usb_handleOpen(usb_open_t *o, msg_t *msg)
{
	usb_drvpriv_t *drv;
	int pipe;
	hcd_t *hcd;

	if ((drv = usb_drvFind(msg->pid)) == NULL) {
		log_error("Fail to find driver pid: %d\n", msg->pid);
		return -EINVAL;
	}

	if ((hcd = hcd_find(usb_common.hcds, o->locationID)) == NULL) {
		log_error("Fail to find dev: %d\n", o->dev);
		return -EINVAL;
	}

	if ((pipe = usb_drvPipeOpen(drv, hcd, o->locationID, o->iface, o->dir, o->type)) < 0)
		return -EINVAL;

	return pipe;
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

	msgSend(t->extrn.port, &msg);
	usb_transferPut(t);
}


static void usb_urbSyncCompleted(usb_transfer_t *t)
{
	msg_t msg = { 0 };

	msg.type = mtDevCtl;
	msg.pid = t->extrn.pid;
	msg.o.err = (t->error != 0) ? -t->error : t->transferred;

	if ((t->direction == usb_dir_in) && (t->error == 0)) {
		memcpy(t->extrn.odata, t->buffer, min(t->extrn.osize, t->transferred));
	}

	/* TODO: it should be non-blocking */
	msgRespond(usb_common.port, &msg, t->extrn.rid);
	usb_transferFree(t);
}
#endif /* USB_INTERNAL_ONLY */


static void usb_msgthr(void *arg)
{
	unsigned port = (int)arg;
	msg_rid_t rid;
	msg_t msg;
	usb_msg_t *umsg;
	bool respond;
	int ret;

	for (;;) {
		ret = msgRecv(port, &msg, &rid);
		if (ret < 0) {
			continue;
		}
		respond = true;
		switch (msg.type) {
			case mtRead:
				msg.o.err = usb_devsList(msg.o.data, msg.o.size);
				break;
			case mtDevCtl:
				umsg = (usb_msg_t *)msg.i.raw;
				switch (umsg->type) {
#ifndef USB_INTERNAL_ONLY
					case usb_msg_connect:
						msg.o.err = usb_handleConnect(&msg, &umsg->connect);
						break;
					case usb_msg_open:
						msg.o.err = usb_handleOpen(&umsg->open, &msg);
						break;
					case usb_msg_urb:
						ret = usb_handleUrb(&msg, port, rid);
						if (umsg->urb.sync && ret == 0) {
							/* Block the sender until the transfer finishes */
							respond = false;
						}
						else {
							msg.o.err = ret;
						}
						break;
					case usb_msg_urbcmd:
						msg.o.err = usb_handleUrbcmd(&msg);
						break;
#endif
					default:
						msg.o.err = -EINVAL;
						log_error("unsupported usb_msg type: %d\n", umsg->type);
						break;
				}
				break;
			default:
				msg.o.err = -EINVAL;
				log_error("unsupported msg type\n");
		}

		if (respond) {
			msgRespond(port, &msg, rid);
		}
	}
}


#ifndef USB_INTERNAL_ONLY
static usb_transferOps_t usbprocdrv_transferOps = {
	.urbSyncCompleted = usb_urbSyncCompleted,
	.urbAsyncCompleted = usb_urbAsyncCompleted,
};


const usb_transferOps_t *usbprocdrv_transferOpsGet(void)
{
	return &usbprocdrv_transferOps;
}
#endif


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

		if (t->async) {
			t->ops->urbAsyncCompleted(t);
		}
		else {
			t->ops->urbSyncCompleted(t);
		}
	}
}


int main(int argc, char *argv[])
{
	oid_t oid;
	int i;
	usb_driver_t *drv;

	if (mutexCreate(&usb_common.transferLock) != 0) {
		log_error("Can't create mutex!\n");
		return 1;
	}

	if (condCreate(&usb_common.finishedCond) != 0) {
		log_error("Can't create cond!\n");
		return 1;
	}

	if (usb_memInit() != 0) {
		log_error("Can't initiate memory management!\n");
		return 1;
	}

	if (usb_devInit() != 0) {
		log_error("Fail to init devices!\n");
		return 1;
	}

	for (;;) {
		drv = usb_registeredDriverPop();
		if (drv != NULL) {
			log_msg("Initializing driver as host-side: %s\n", drv->name);
			usb_internalDriverInit(drv);
		}
		else {
			break;
		}
	}

	if (hub_init() != 0) {
		log_error("Fail to init hub driver!\n");
		return 1;
	}

	if ((usb_common.hcds = hcd_init()) == NULL) {
		log_error("Fail to init hcds!\n");
		return 1;
	}

	if (portCreate(&usb_common.port) != 0) {
		log_error("Can't create port!\n");
		return 1;
	}

	oid.port = usb_common.port;
	oid.id = 0;

	if (create_dev(&oid, "/dev/usb") != 0) {
		log_error("Can't create dev!\n");
		return 1;
	}

	if (beginthread(usb_msgthr, MSGTHR_PRIO, &usb_common.ustack, sizeof(usb_common.ustack), (void *)usb_common.port) != 0) {
		log_error("Fail to run msgthr!\n");
		return 1;
	}

	for (i = 0; i < N_STATUSTHRS - 1; i++) {
		if (beginthread(usb_statusthr, STATUSTHR_PRIO, &usb_common.stack[i], sizeof(usb_common.stack[i]), NULL) != 0) {
			log_error("Fail to init hub driver!\n");
			return 1;
		}
	}

	priority(STATUSTHR_PRIO);
	usb_statusthr(NULL);

	return 0;
}


int usblibdrv_open(usb_driver_t *drv, usb_devinfo_t *dev, usb_transfer_type_t type, usb_dir_t dir)
{
	usb_drvpriv_t *drvpriv = (usb_drvpriv_t *)drv->hostPriv;
	int pipe;
	hcd_t *hcd;

	hcd = hcd_find(usb_common.hcds, dev->locationID);
	if (hcd == NULL) {
		log_error("Failed to find dev: %d\n", dev->locationID);
		return -EINVAL;
	}

	pipe = usb_drvPipeOpen(drvpriv, hcd, dev->locationID, dev->interface, dir, type);
	if (pipe < 0) {
		return -EINVAL;
	}

	return pipe;
}
