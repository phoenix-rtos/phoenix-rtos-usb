/*
 * Phoenix-RTOS
 *
 * libusb/procdriver.c
 *
 * Copyright 2021, 2024 Phoenix Systems
 * Author: Maciej Purski, Adam Greloch
 *
 * %LICENSE%
 */

#include <errno.h>
#include <sys/msg.h>
#include <sys/threads.h>
#include <sys/list.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#include <usbdriver.h>
#include <usbprocdriver.h>


#ifndef USB_N_UMSG_THREADS
#define USB_N_UMSG_THREADS 2
#endif

#ifndef USB_UMSG_PRIO
#define USB_UMSG_PRIO 3
#endif


static usb_pipeOps_t usbprocdrv_pipeOps;


static struct {
	char ustack[USB_N_UMSG_THREADS - 1][2048] __attribute__((aligned(8)));
	unsigned srvport;
	unsigned drvport;
} usbprocdrv_common;


static void usb_hostLookup(oid_t *oid)
{
	int ret;

	for (;;) {
		ret = lookup("devfs/usb", NULL, oid);
		if (ret >= 0) {
			break;
		}

		ret = lookup("/dev/usb", NULL, oid);
		if (ret >= 0) {
			break;
		}

		usleep(1000000);
	}
}


static void usb_thread(void *arg)
{
	usb_driver_t *drv = (usb_driver_t *)arg;
	msg_t msg = { 0 };
	usb_msg_t *umsg = (usb_msg_t *)&msg.i.raw;
	int ret;

	for (;;) {
		ret = usb_eventsWait(usbprocdrv_common.drvport, &msg);
		if (ret < 0) {
			fprintf(stderr, "usbdrv: error when receiving event from host\n");
			continue;
		}

		switch (umsg->type) {
			case usb_msg_insertion:
				drv->handlers.insertion(drv, &umsg->insertion);
				break;
			case usb_msg_deletion:
				drv->handlers.deletion(drv, &umsg->deletion);
				break;
			case usb_msg_completion:
				drv->handlers.completion(drv, &umsg->completion, msg.i.data, msg.i.size);
				break;
			default:
				fprintf(stderr, "usbdrv: unknown msg type\n");
				break;
		}
	}
}


static int usb_connect(const usb_device_id_t *filters, unsigned int nfilters)
{
	msg_t msg = { 0 };
	usb_msg_t *umsg = (usb_msg_t *)&msg.i.raw;

	msg.type = mtDevCtl;
	msg.i.size = sizeof(*filters) * nfilters;
	msg.i.data = (void *)filters;

	umsg->type = usb_msg_connect;
	umsg->connect.port = usbprocdrv_common.drvport;
	umsg->connect.nfilters = nfilters;

	return msgSend(usbprocdrv_common.srvport, &msg) < 0;
}


int usb_driverProcRun(usb_driver_t *drv, void *args)
{
	oid_t oid;
	int ret, i;

	/* usb_driverProcRun is invoked iff drivers are in the proc variant */
	drv->pipeOps = &usbprocdrv_pipeOps;

	ret = drv->ops.init(drv, args);
	if (ret < 0) {
		return -1;
	}

	usb_hostLookup(&oid);
	usbprocdrv_common.srvport = oid.port;

	ret = portCreate(&usbprocdrv_common.drvport);
	if (ret != 0) {
		return -1;
	}

	ret = usb_connect(drv->filters, drv->nfilters);
	if (ret < 0) {
		return -1;
	}

	for (i = 0; i < USB_N_UMSG_THREADS - 1; i++) {
		ret = beginthread(usb_thread, USB_UMSG_PRIO, usbprocdrv_common.ustack[i], sizeof(usbprocdrv_common.ustack[i]), drv);
		if (ret < 0) {
			fprintf(stderr, "usbdrv: fail to beginthread ret: %d\n", ret);
			return -1;
		}
	}

	priority(USB_UMSG_PRIO);
	usb_thread(drv);

	return 0;
}


int usb_eventsWait(int port, msg_t *msg)
{
	msg_rid_t rid;
	int err;

	do {
		err = msgRecv(port, msg, &rid);
		if (err < 0 && err != -EINTR)
			return -1;
	} while (err == -EINTR);

	if (msgRespond(port, msg, rid) < 0)
		return -1;

	return 0;
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
