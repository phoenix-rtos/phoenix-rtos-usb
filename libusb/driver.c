/*
 * Phoenix-RTOS
 *
 * libusb/driver.c
 *
 * Copyright 2021, 2024 Phoenix Systems
 * Author: Maciej Purski, Adam Greloch
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include <errno.h>
#include <usbdriver.h>
#include <sys/msg.h>
#include <sys/threads.h>
#include <sys/list.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>


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
} usbdrv_common;


int usb_open(usb_driver_t *drv, usb_devinfo_t *dev, usb_transfer_type_t type, usb_dir_t dir)
{
	return drv->pipeOps->open(drv, dev, type, dir);
}


int usb_urbAlloc(usb_driver_t *drv, unsigned pipe, void *data, usb_dir_t dir, size_t size, int type)
{
	return drv->pipeOps->urbAlloc(drv, pipe, data, dir, size, type);
}


int usb_urbFree(usb_driver_t *drv, unsigned pipe, unsigned urb)
{
	return drv->pipeOps->urbFree(drv, pipe, urb);
}


int usb_transferAsync(usb_driver_t *drv, unsigned pipe, unsigned urbid, size_t size, usb_setup_packet_t *setup)
{
	return drv->pipeOps->transferAsync(drv, pipe, urbid, size, setup);
}


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
		ret = usb_eventsWait(usbdrv_common.drvport, &msg);
		if (ret < 0) {
			/* TODO should the thread abort the loop on error? */
			continue;
		}

		switch (umsg->type) {
			case usb_msg_insertion:
				drv->handlers->insertion(drv, &umsg->insertion);
				break;
			case usb_msg_deletion:
				drv->handlers->deletion(drv, &umsg->deletion);
				break;
			case usb_msg_completion:
				drv->handlers->completion(drv, &umsg->completion, msg.i.data, msg.i.size);
				break;
			default:
				fprintf(stderr, "usbdrv: Error when receiving event from host\n");
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
	umsg->connect.port = usbdrv_common.drvport;
	umsg->connect.nfilters = nfilters;

	return msgSend(usbdrv_common.srvport, &msg) < 0;
}


int usb_procDrvRun(usb_driver_t *drv)
{
	oid_t oid;
	int ret, i;

	/* usb_procDrvRun is invoked iff drivers are in the proc variant */
	drv->pipeOps = &usbprocdrv_pipeOps;

	ret = drv->ops->init(drv);
	if (ret < 0) {
		return 1;
	}

	usb_hostLookup(&oid);
	usbdrv_common.srvport = oid.port;

	if (portCreate(&usbdrv_common.drvport) != 0) {
		return -1;
	}

	ret = usb_connect(drv->filters, drv->nfilters);
	if (ret < 0) {
		return -1;
	}

	for (i = 0; i < USB_N_UMSG_THREADS - 1; i++) {
		ret = beginthread(usb_thread, USB_UMSG_PRIO, usbdrv_common.ustack[i], sizeof(usbdrv_common.ustack[i]), drv);
		if (ret < 0) {
			fprintf(stderr, "usbdrv: fail to beginthread ret: %d\n", ret);
			return 1;
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

	ret = msgSend(usbdrv_common.srvport, &msg);
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

	if ((ret = msgSend(usbdrv_common.srvport, &msg)) != 0)
		return ret;

	return msg.o.err;
}


int usb_transferControl(usb_driver_t *drv, unsigned pipe, usb_setup_packet_t *setup, void *data, size_t size, usb_dir_t dir)
{
	usb_urb_t urb = {
		.pipe = pipe,
		.setup = *setup,
		.dir = dir,
		.size = size,
		.type = usb_transfer_control,
		.sync = 1
	};

	return drv->pipeOps->submitSync(drv, &urb, data);
}


int usb_transferBulk(usb_driver_t *drv, unsigned pipe, void *data, size_t size, usb_dir_t dir)
{
	usb_urb_t urb = {
		.pipe = pipe,
		.dir = dir,
		.size = size,
		.type = usb_transfer_bulk,
		.sync = 1
	};

	return drv->pipeOps->submitSync(drv, &urb, data);
}


int usb_setConfiguration(usb_driver_t *drv, unsigned pipe, int conf)
{
	usb_setup_packet_t setup = (usb_setup_packet_t) {
		.bmRequestType = REQUEST_DIR_HOST2DEV | REQUEST_TYPE_STANDARD | REQUEST_RECIPIENT_DEVICE,
		.bRequest = REQ_SET_CONFIGURATION,
		.wValue = conf,
		.wIndex = 0,
		.wLength = 0,
	};

	return usb_transferControl(drv, pipe, &setup, NULL, 0, usb_dir_out);
}


int usb_clearFeatureHalt(usb_driver_t *drv, unsigned pipe, int ep)
{
	usb_setup_packet_t setup = (usb_setup_packet_t) {
		.bmRequestType = REQUEST_DIR_HOST2DEV | REQUEST_TYPE_STANDARD | REQUEST_RECIPIENT_DEVICE,
		.bRequest = REQ_CLEAR_FEATURE,
		.wValue = USB_ENDPOINT_HALT,
		.wIndex = ep,
		.wLength = 0,
	};

	return usb_transferControl(drv, pipe, &setup, NULL, 0, usb_dir_out);
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

	ret = msgSend(usbdrv_common.srvport, &msg);
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
	ret = msgSend(usbdrv_common.srvport, &msg);
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
	if ((ret = msgSend(usbdrv_common.srvport, &msg)) < 0)
		return ret;

	return 0;
}


const usb_modeswitch_t *usb_modeswitchFind(uint16_t vid, uint16_t pid, const usb_modeswitch_t *modes, int nmodes)
{
	int i;

	for (i = 0; i < nmodes; i++) {
		if (vid == modes[i].vid && pid == modes[i].pid)
			return &modes[i];
	}

	return NULL;
}


void usb_dumpDeviceDescriptor(FILE *stream, usb_device_desc_t *descr)
{
	fprintf(stream, "DEVICE DESCRIPTOR:\n");
	fprintf(stream, "\tbLength: %d\n", descr->bLength);
	fprintf(stream, "\tbDescriptorType: 0x%x\n", descr->bDescriptorType);
	fprintf(stream, "\tbcdUSB: %d\n", descr->bcdUSB);
	fprintf(stream, "\tbDeviceClass: %d\n", descr->bDeviceClass);
	fprintf(stream, "\tbDeviceSubClass: %d\n", descr->bDeviceSubClass);
	fprintf(stream, "\tbDeviceProtocol: %d\n", descr->bDeviceProtocol);
	fprintf(stream, "\tbMaxPacketSize0: %d\n", descr->bMaxPacketSize0);
	fprintf(stream, "\tidVendor: 0x%x\n", descr->idVendor);
	fprintf(stream, "\tidProduct: 0x%x\n", descr->idProduct);
	fprintf(stream, "\tbcdDevice: %d\n", descr->bcdDevice);
	fprintf(stream, "\tiManufacturer: %d\n", descr->iManufacturer);
	fprintf(stream, "\tiProduct: %d\n", descr->iProduct);
	fprintf(stream, "\tiSerialNumber: %d\n", descr->iSerialNumber);
	fprintf(stream, "\tbNumConfigurations: %d\n", descr->bNumConfigurations);
}


void usb_dumpConfigurationDescriptor(FILE *stream, usb_configuration_desc_t *descr)
{
	fprintf(stream, "CONFIGURATION DESCRIPTOR:\n");
	fprintf(stream, "\tbLength: %d\n", descr->bLength);
	fprintf(stream, "\tbDescriptorType: 0x%x\n", descr->bDescriptorType);
	fprintf(stream, "\twTotalLength: %d\n", descr->wTotalLength);
	fprintf(stream, "\tbNumInterfaces: %d\n", descr->bNumInterfaces);
	fprintf(stream, "\tbConfigurationValue: %d\n", descr->bConfigurationValue);
	fprintf(stream, "\tiConfiguration %d\n", descr->iConfiguration);
	fprintf(stream, "\tbmAttributes: 0x%x\n", descr->bmAttributes);
	fprintf(stream, "\tbMaxPower: %d\n", descr->bMaxPower);
}


void usb_dumpInferfaceDesc(FILE *stream, usb_interface_desc_t *descr)
{
	fprintf(stream, "INTERFACE DESCRIPTOR:\n");
	fprintf(stream, "\tbLength: %d\n", descr->bLength);
	fprintf(stream, "\tbDescriptorType: 0x%x\n", descr->bDescriptorType);
	fprintf(stream, "\tbInterfaceNumber: %d\n", descr->bInterfaceNumber);
	fprintf(stream, "\tbNumEndpoints: %d\n", descr->bNumEndpoints);
	fprintf(stream, "\tbInterfaceClass: %x\n", descr->bInterfaceClass);
	fprintf(stream, "\tbInterfaceSubClass: 0x%x\n", descr->bInterfaceSubClass);
	fprintf(stream, "\tbInterfaceProtocol: 0x%x\n", descr->bInterfaceProtocol);
	fprintf(stream, "\tiInterface: %d\n", descr->iInterface);
}


void usb_dumpEndpointDesc(FILE *stream, usb_endpoint_desc_t *descr)
{
	fprintf(stream, "ENDPOINT DESCRIPTOR:\n");
	fprintf(stream, "\tbLength: %d\n", descr->bLength);
	fprintf(stream, "\tbDescriptorType: 0x%x\n", descr->bDescriptorType);
	fprintf(stream, "\tbEndpointAddress: %d\n", descr->bEndpointAddress);
	fprintf(stream, "\tbmAttributes: 0x%x\n", descr->bmAttributes);
	fprintf(stream, "\twMaxPacketSize: %d\n", descr->wMaxPacketSize);
	fprintf(stream, "\tbInterval: %d\n", descr->bInterval);
}


void usb_dumpStringDesc(FILE *stream, usb_string_desc_t *descr)
{
	fprintf(stream, "STRING DESCRIPTOR:\n");
	fprintf(stream, "\tbLength: %d\n", descr->bLength);
	fprintf(stream, "\tbDescriptorType: 0x%x\n", descr->bDescriptorType);
	fprintf(stream, "\twData: %.*s\n", descr->bLength - 2, descr->wData);
}


int usb_modeswitchHandle(usb_driver_t *drv, usb_devinfo_t *dev, const usb_modeswitch_t *mode)
{
	char msg[sizeof(mode->msg)];
	int pipeCtrl, pipeIn, pipeOut, ret;

	pipeCtrl = usb_open(drv, dev, usb_transfer_control, 0);
	if (pipeCtrl < 0) {
		return -EINVAL;
	}

	ret = usb_setConfiguration(drv, pipeCtrl, 1);
	if (ret != 0) {
		return -EINVAL;
	}

	pipeIn = usb_open(drv, dev, usb_transfer_bulk, usb_dir_in);
	if (pipeIn < 0) {
		return -EINVAL;
	}

	pipeOut = usb_open(drv, dev, usb_transfer_bulk, usb_dir_out);
	if (pipeOut < 0) {
		return -EINVAL;
	}

	memcpy(msg, mode->msg, sizeof(msg));

	ret = usb_transferBulk(drv, pipeOut, msg, sizeof(msg), usb_dir_out);
	if (ret < 0) {
		return -EINVAL;
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
