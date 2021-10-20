/*
 * Phoenix-RTOS
 *
 * USB Hub
 *
 * Copyright 2021 Phoenix Systems
 * Author: Maciej Purski
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */


#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/list.h>
#include <sys/minmax.h>
#include <sys/msg.h>
#include <sys/threads.h>
#include <sys/types.h>
#include <string.h>
#include <usb.h>
#include <unistd.h>

#include <usb.h>

#include "usbhost.h"
#include "hub.h"
#include "drv.h"
#include "hcd.h"
#include "dev.h"

#define HUB_ENUM_RETRIES 3

struct {
	char stack[4096] __attribute__((aligned(8)));
	handle_t lock;
	handle_t cond;
	usb_dev_t *hubs;
	int restart;
} hub_common;


static int hub_getDesc(usb_dev_t *hub, char *buf, size_t len)
{
	usb_setup_packet_t setup = (usb_setup_packet_t) {
		.bmRequestType = REQUEST_DIR_DEV2HOST | REQUEST_TYPE_CLASS | REQUEST_RECIPIENT_DEVICE,
		.bRequest = REQ_GET_DESCRIPTOR,
		.wValue = USB_DESC_TYPE_HUB << 8,
		.wIndex = 0,
		.wLength = len,
	};

	return usb_devCtrl(hub, usb_dir_in, &setup, buf, len);
}


static int hub_setConf(usb_dev_t *hub, int conf)
{
	usb_setup_packet_t setup = (usb_setup_packet_t) {
		.bmRequestType = REQUEST_DIR_HOST2DEV | REQUEST_TYPE_STANDARD | REQUEST_RECIPIENT_DEVICE,
		.bRequest = REQ_SET_CONFIGURATION,
		.wValue = conf,
		.wIndex = 0,
		.wLength = 0,
	};

	return usb_devCtrl(hub, usb_dir_out, &setup, NULL, 0);
}


static int hub_setPortPower(usb_dev_t *hub, int port)
{
	usb_setup_packet_t setup = (usb_setup_packet_t) {
		.bmRequestType = REQUEST_DIR_HOST2DEV | REQUEST_TYPE_CLASS | REQUEST_RECIPIENT_OTHER,
		.bRequest = REQ_SET_FEATURE,
		.wValue = USB_PORT_FEAT_POWER,
		.wIndex = port,
		.wLength = 0
	};

	return usb_devCtrl(hub, usb_dir_out, &setup, NULL, 0);
}


static int hub_getPortStatus(usb_dev_t *hub, int port, usb_port_status_t *status)
{
	usb_setup_packet_t setup = (usb_setup_packet_t) {
		.bmRequestType = REQUEST_DIR_DEV2HOST | REQUEST_TYPE_CLASS | REQUEST_RECIPIENT_OTHER,
		.bRequest = REQ_GET_STATUS,
		.wValue = 0,
		.wIndex = port,
		.wLength = sizeof(usb_port_status_t)
	};

	return usb_devCtrl(hub, usb_dir_in, &setup, (char *)status, sizeof(usb_port_status_t));
}


static int hub_clearPortFeature(usb_dev_t *hub, int port, uint16_t wValue)
{
	usb_setup_packet_t setup = (usb_setup_packet_t) {
		.bmRequestType = REQUEST_DIR_HOST2DEV | REQUEST_TYPE_CLASS | REQUEST_RECIPIENT_OTHER,
		.bRequest = REQ_CLEAR_FEATURE,
		.wValue = wValue,
		.wIndex = port,
		.wLength = 0
	};

	return usb_devCtrl(hub, usb_dir_out, &setup, NULL, 0);
}


static int hub_setPortFeature(usb_dev_t *hub, int port, uint16_t wValue)
{
	usb_setup_packet_t setup = (usb_setup_packet_t) {
		.bmRequestType = REQUEST_DIR_HOST2DEV | REQUEST_TYPE_CLASS | REQUEST_RECIPIENT_OTHER,
		.bRequest = REQ_SET_FEATURE,
		.wValue = wValue,
		.wIndex = port,
		.wLength = 0
	};

	return usb_devCtrl(hub, usb_dir_out, &setup, NULL, 0);
}


static int hub_interruptInit(usb_dev_t *hub)
{
	usb_transfer_t *t;

	if ((t = calloc(1, sizeof(usb_transfer_t))) == NULL) {
		fprintf(stderr, "hub: Out of memory!\n");
		return -ENOMEM;
	}

	if ((t->buffer = usb_alloc(sizeof(uint32_t))) == NULL) {
		free(t);
		fprintf(stderr, "hub: Out of memory!\n");
		return -ENOMEM;
	}

	if ((t->pipe = usb_drvPipeOpen(NULL, hub, &hub->ifs[0], usb_dir_in, usb_transfer_interrupt)) == NULL) {
		usb_free(t->buffer, sizeof(uint32_t));
		free(t);
		fprintf(stderr, "hub: Out of memory!\n");
		return -ENOMEM;
	}

	t->type = usb_transfer_interrupt;
	t->direction = usb_dir_in;
	t->size = (hub->nports / 8) + 1;
	t->cond = &hub_common.cond;

	hub->statusTransfer = t;

	return 0;
}


int hub_poll(usb_dev_t *hub)
{
	hub->statusTransfer->finished = 0;
	hub->statusTransfer->error = 0;
	hub->statusTransfer->transferred = 0;

	return usb_transferSubmit(hub->statusTransfer, 0);
}


int hub_clearPortFeatures(usb_dev_t *hub, int port, uint32_t change)
{
	int i;

	for (i = 0; i < 5; i++) {
		if (change & (1 << i)) {
			if (hub_clearPortFeature(hub, port, USB_PORT_FEAT_C_CONNECTION + i) < 0)
				return -1;
		}
	}

	return 0;
}


int hub_portReset(usb_dev_t *hub, int port, usb_port_status_t *status)
{
	int retries;

	memset(status, 0, sizeof(*status));
	if (hub_setPortFeature(hub, port, USB_PORT_FEAT_RESET) < 0)
		return -1;

	for (retries = 5; (status->wPortChange & USB_PORT_FEAT_C_RESET) == 0 && retries > 0; --retries) {
		usleep(100000);
		if (hub_getPortStatus(hub, port, status) < 0)
			return -1;
	}

	if (hub_clearPortFeatures(hub, port, status->wPortChange) < 0)
		return -1;

	return (retries > 0) ? 0 : -1;
}


static void hub_devConnected(usb_dev_t *hub, int port)
{
	usb_dev_t *dev;
	usb_port_status_t status;
	int ret;
	int retries = HUB_ENUM_RETRIES;

	if ((dev = usb_devAlloc()) == NULL) {
		fprintf(stderr, "hub: Not enough memory to allocate a new device!\n");
		return;
	}

	hub->devs[port - 1] = dev;
	dev->hub = hub;
	dev->hcd = hub->hcd;
	dev->port = port;

	do {
		if (hub_portReset(hub, port, &status) < 0) {
			fprintf(stderr, "hub: fail to reset port %d\n", port);
			usb_devFree(dev);
			return;
		}

		if (status.wPortStatus & USB_PORT_STAT_HIGH_SPEED)
			dev->speed = usb_high_speed;
		else if (status.wPortStatus & USB_PORT_STAT_LOW_SPEED)
			dev->speed = usb_low_speed;
		else
			dev->speed = usb_full_speed;

		ret = usb_devEnumerate(dev);
		retries--;
		if (ret != 0) {
			fprintf(stderr, "usb: Enumeration failed retries left: %d\n", retries);
			dev->hcd->ops->pipeDestroy(dev->hcd, dev->ctrlPipe);
			dev->address = 0;
			dev->locationID = 0;
		}
	} while (ret != 0 && retries > 0);

	if (ret != 0) {
		usb_devDestroy(dev);
		hub->devs[port - 1] = NULL;
	}
}


static void hub_portStatusChanged(usb_dev_t *hub, int port)
{
	usb_port_status_t status;

	if (hub_getPortStatus(hub, port, &status) < 0) {
		fprintf(stderr, "hub: getPortStatus port %d failed!\n", port);
		return;
	}

	if (hub_clearPortFeatures(hub, port, status.wPortChange) < 0) {
		fprintf(stderr, "hub: clearPortFeatures failed on port %d!\n", port);
		return;
	}

	if (status.wPortChange & USB_PORT_STAT_C_CONNECTION) {
		if (status.wPortStatus & USB_PORT_STAT_CONNECTION) {
			/* required by modeswitch */
			if (hub->devs[port - 1] != NULL)
				usb_devDisconnected(hub->devs[port - 1]);
			hub_devConnected(hub, port);
		}
		else if (hub->devs[port - 1] != NULL) {
			usb_devDisconnected(hub->devs[port - 1]);
			hub->devs[port - 1] = NULL;
		}
	}

	/* TODO: handle other status changes */
}


static void hub_hubStatusChanged(usb_dev_t *hub)
{
}


static void hub_statusChanged(usb_dev_t *hub, uint32_t status)
{
	int i;

	if (status & 1)
		hub_hubStatusChanged(hub);

	for (i = 0; i < hub->nports; i++) {
		if (status & (1 << (i + 1)))
			hub_portStatusChanged(hub, i + 1);
	}

	/* Not a root hub */
	if (hub->hub != NULL)
		hub_poll(hub);
}


static uint32_t hub_getStatus(usb_dev_t *hub)
{
	uint32_t status = 0;

	if (hub->hub == NULL) {
		status = hub->hcd->ops->getRoothubStatus(hub);
	}
	else if (usb_transferCheck(hub->statusTransfer)) {
		if (hub->statusTransfer->error == 0 && hub->statusTransfer->transferred > 0)
			memcpy(&status, hub->statusTransfer->buffer, sizeof(status));
		else
			fprintf(stderr, "usb-hub: Interrupt transfer error\n");
	}

	return status;
}


static void hub_thread(void *args)
{
	usb_dev_t *hub;
	uint32_t status;

	mutexLock(hub_common.lock);
	for (;;) {
		condWait(hub_common.cond, hub_common.lock, 1000000);
		hub = hub_common.hubs;
		do {
			if ((status = hub_getStatus(hub)) != 0)
				hub_statusChanged(hub, status);
			if (hub_common.restart) {
				/* Hubs removed due to statusChanged */
				hub_common.restart = 0;
				break;
			}
		} while ((hub = hub->next) != hub_common.hubs);
	}
}


void hub_remove(usb_dev_t *hub)
{
	LIST_REMOVE(&hub_common.hubs, hub);
	hub_common.restart = 1;

	if (hub->statusTransfer != NULL) {
		usb_drvPipeFree(NULL, hub->statusTransfer->pipe);
		usb_free(hub->statusTransfer->buffer, sizeof(uint32_t));
		free(hub->statusTransfer);
	}
}


int hub_add(usb_dev_t *hub)
{
	char buf[15];
	usb_hub_desc_t *desc;
	int i;

	if (hub_setConf(hub, 1) < 0) {
		fprintf(stderr, "hub: Fail to set configuration!\n");
		return -EINVAL;
	}

	if (hub_getDesc(hub, buf, sizeof(buf)) < 0) {
		fprintf(stderr, "hub: Fail to get descriptor\n");
		return -EINVAL;
	}

	/* Hub descriptors might vary in size */
	desc = (usb_hub_desc_t *)buf;
	hub->nports = min(USB_HUB_MAX_PORTS, desc->bNbrPorts);
	if ((hub->devs = calloc(hub->nports, sizeof(usb_dev_t *))) == NULL) {
		fprintf(stderr, "hub: Out of memory!\n");
		return -ENOMEM;
	}

	for (i = 0; i < hub->nports; i++) {
		if (hub_setPortPower(hub, i + 1) < 0) {
			fprintf(stderr, "hub: Fail to set port %d power!\n", i + 1);
			free(hub->devs);
			return -EINVAL;
		}
	}
	LIST_ADD(&hub_common.hubs, hub);

	/* Not a root hub */
	if (hub->hub != NULL) {
		if (hub_interruptInit(hub) != 0)
			return -EINVAL;
		return hub_poll(hub);
	}

	return 0;
}


int hub_init(void)
{
	if (mutexCreate(&hub_common.lock) != 0)
		return -ENOMEM;

	if (condCreate(&hub_common.cond) != 0) {
		resourceDestroy(hub_common.lock);
		return -ENOMEM;
	}

	if (beginthread(hub_thread, 4, hub_common.stack, sizeof(hub_common.stack), NULL) != 0) {
		resourceDestroy(hub_common.lock);
		resourceDestroy(hub_common.cond);
		return -ENOMEM;
	}

	return 0;
}
