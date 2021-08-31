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
#include "hcd.h"
#include "dev.h"

struct {
	handle_t lock;
	handle_t cond;
	usb_dev_t *hubs;
	int restart;
} hub_common;


static void hub_handleTransfer(usb_transfer_t *t)
{
	condSignal(hub_common.cond);
}


static int hub_getDesc(usb_dev_t *hub, char *buf, size_t len)
{
	usb_setup_packet_t setup = (usb_setup_packet_t) {
		.bmRequestType = REQUEST_DIR_DEV2HOST | REQUEST_TYPE_CLASS | REQUEST_RECIPIENT_DEVICE,
		.bRequest = REQ_GET_DESCRIPTOR,
		.wValue = USB_DESC_TYPE_HUB << 8,
		.wIndex = 0,
		.wLength = len,
	};

	return usb_controlTransferSync(hub, usb_dir_in, &setup, buf, len);
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

	return usb_controlTransferSync(hub, usb_dir_out, &setup, NULL, 0);
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

	return usb_controlTransferSync(hub, usb_dir_out, &setup, NULL, 0);
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

	return usb_controlTransferSync(hub, usb_dir_in, &setup, (char *)status, sizeof(usb_port_status_t));
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

	return usb_controlTransferSync(hub, usb_dir_out, &setup, NULL, 0);
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

	return usb_controlTransferSync(hub, usb_dir_out, &setup, NULL, 0);
}


int hub_poll(usb_dev_t *hub)
{
	usb_transfer_t *t;

	if ((t = hub->statusTransfer) == NULL) {
		if ((t = calloc(1, sizeof(usb_transfer_t))) == NULL) {
			fprintf(stderr, "hub: Out of memory!\n");
			return -ENOMEM;
		}

		if ((t->buffer = usb_alloc(sizeof(uint32_t))) == NULL) {
			free(t);
			fprintf(stderr, "hub: Out of memory!\n");
			return -ENOMEM;
		}
		t->type = usb_transfer_interrupt;
		t->direction = usb_dir_in;
		t->ep = hub->eps->next;
		t->handler = hub_handleTransfer;
		t->size = hub->nports / 8 + 1;
		hub->statusTransfer = t;
		hub->hcd->ops->transferEnqueue(hub->hcd, hub->statusTransfer);
	}
	else if (t->finished) {
		t->finished = 0;
		t->error = 0;
		t->transferred = 0;
		hub->hcd->ops->transferEnqueue(hub->hcd, hub->statusTransfer);
	}

	return 0;
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
	int retries = 5;

	usleep(100000);
	if (hub_setPortFeature(hub, port, USB_PORT_FEAT_RESET) < 0)
		return -1;

	do {
		usleep(100000);
		if (hub_getPortStatus(hub, port, status) < 0)
			return -1;
		retries--;
	} while ((status->wPortChange & USB_PORT_FEAT_C_RESET) == 0 && retries > 0);

	if (hub_clearPortFeatures(hub, port, status->wPortChange) < 0)
		return -1;

	return 0;
}


static void hub_devConnected(usb_dev_t *hub, int port)
{
	usb_dev_t *dev;
	usb_port_status_t status;

	if ((dev = usb_devAlloc()) == NULL) {
		fprintf(stderr, "hub: Not enough memory to allocate a new device!\n");
		return;
	}

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

	hub->devs[port - 1] = dev;
	dev->hub = hub;
	dev->hcd = hub->hcd;
	dev->port = port;

	if (usb_devEnumerate(dev) != 0)
		hub->devs[port - 1] = NULL;
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

	for (i = 1; i <= hub->nports; i++) {
		if (status & (1 << i))
			hub_portStatusChanged(hub, i);
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
	else if (hub->statusTransfer->finished) {
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
	if (hub->hub != NULL)
		return hub_poll(hub);
	else
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

	beginthread(hub_thread, 4, malloc(0x1000), 0x1000, NULL);

	return 0;
}