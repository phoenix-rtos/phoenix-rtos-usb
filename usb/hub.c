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

#define HUB_ENUM_RETRIES     3
#define HUB_DEBOUNCE_STABLE  100000
#define HUB_DEBOUNCE_PERIOD  25000
#define HUB_DEBOUNCE_TIMEOUT 1500000


typedef struct _hub_event {
	struct _hub_event *next, *prev;
	usb_dev_t *hub;
} hub_event_t;


struct {
	char stack[4096] __attribute__((aligned(8)));
	handle_t lock;
	handle_t cond;
	hub_event_t *events;
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
		USB_LOG("hub: Out of memory!\n");
		return -ENOMEM;
	}

	if ((t->buffer = usb_alloc(sizeof(uint32_t))) == NULL) {
		free(t);
		USB_LOG("hub: Out of memory!\n");
		return -ENOMEM;
	}

	if ((hub->irqPipe = usb_pipeOpen(hub, 0, usb_dir_in, usb_transfer_interrupt)) == NULL) {
		usb_free(t->buffer, sizeof(uint32_t));
		free(t);
		USB_LOG("hub: Fail to open interrupt pipe!\n");
		return -ENOMEM;
	}

	t->type = usb_transfer_interrupt;
	t->direction = usb_dir_in;
	t->size = (hub->nports / 8) + 1;
	t->hub = hub;

	hub->statusTransfer = t;

	return 0;
}


static int hub_poll(usb_dev_t *hub)
{
	return usb_transferSubmit(hub->statusTransfer, hub->irqPipe, NULL);
}


static int hub_clearPortFeatures(usb_dev_t *hub, int port, uint32_t change)
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


static int hub_portDebounce(usb_dev_t *hub, int port)
{
	usb_port_status_t status;
	uint16_t pstatus = 0xffff;
	int totalTime = 0, stableTime = 0;
	int ret;

	while (totalTime < HUB_DEBOUNCE_TIMEOUT) {
		if ((ret = hub_getPortStatus(hub, port, &status)) < 0)
			return ret;

		if (!(status.wPortChange & USB_PORT_STAT_C_CONNECTION) &&
				(status.wPortStatus & USB_PORT_STAT_CONNECTION) == pstatus) {
			stableTime += HUB_DEBOUNCE_PERIOD;
			if (stableTime >= HUB_DEBOUNCE_STABLE)
				break;
		}
		else {
			stableTime = 0;
			pstatus = status.wPortStatus & USB_PORT_STAT_CONNECTION;
		}

		if (status.wPortChange & USB_PORT_STAT_C_CONNECTION)
			hub_clearPortFeature(hub, port, USB_PORT_FEAT_C_CONNECTION);

		totalTime += HUB_DEBOUNCE_PERIOD;
		usleep(HUB_DEBOUNCE_PERIOD);
	}

	if (stableTime < HUB_DEBOUNCE_STABLE)
		return -ETIMEDOUT;

	return pstatus;
}


static void hub_devConnected(usb_dev_t *hub, int port)
{
	usb_dev_t *dev;
	usb_port_status_t status;
	int ret;
	int retries = HUB_ENUM_RETRIES;

	if ((dev = usb_devAlloc()) == NULL) {
		USB_LOG("hub: Not enough memory to allocate a new device!\n");
		return;
	}

	dev->hub = hub;
	dev->hcd = hub->hcd;
	dev->port = port;

	do {
		if ((ret = hub_portReset(hub, port, &status)) < 0) {
			USB_LOG("hub: fail to reset port %d\n", port);
			break;
		}

		if (status.wPortStatus & USB_PORT_STAT_HIGH_SPEED)
			dev->speed = usb_high_speed;
		else if (status.wPortStatus & USB_PORT_STAT_LOW_SPEED)
			dev->speed = usb_low_speed;
		else
			dev->speed = usb_full_speed;

		ret = usb_devEnumerate(dev);
		retries--;
		if (ret != 0 && !hub_portDebounce(hub, port)) {
			printf("usb: Enumeration failed. No retrying\n");
			break;
		}
		else if (ret != 0) {
			printf("usb: Enumeration failed retries left: %d\n", retries);
			dev->hcd->ops->pipeDestroy(dev->hcd, dev->ctrlPipe);
			if (dev->address != 0)
				hcd_addrFree(hub->hcd, dev->address);
			dev->address = 0;
			dev->locationID = 0;
		}
	} while (ret != 0 && retries > 0);

	if (ret != 0)
		usb_devDisconnected(dev);
}


static void hub_connectstatus(usb_dev_t *hub, int port, usb_port_status_t *status)
{
	int pstatus;

	if (hub->devs[port - 1] != NULL)
		usb_devDisconnected(hub->devs[port - 1]);

	if ((pstatus = hub_portDebounce(hub, port)) < 0)
		return;

	if (pstatus)
		hub_devConnected(hub, port);
}


static void hub_portstatus(usb_dev_t *hub, int port)
{
	usb_port_status_t status;
	int connection = 0;

	if (hub_getPortStatus(hub, port, &status) < 0)
		return;

	if (status.wPortChange & USB_PORT_STAT_C_CONNECTION) {
		hub_clearPortFeature(hub, port, USB_PORT_FEAT_C_CONNECTION);
		connection = 1;
	}

	if (status.wPortChange & USB_PORT_STAT_C_ENABLE) {
		hub_clearPortFeature(hub, port, USB_PORT_FEAT_C_ENABLE);
		if (!(status.wPortStatus & USB_PORT_STAT_ENABLE))
			connection = 1;
	}

	if (status.wPortChange & USB_PORT_STAT_C_RESET)
		hub_clearPortFeature(hub, port, USB_PORT_FEAT_C_RESET);

	if (connection)
		hub_connectstatus(hub, port, &status);
}


static uint32_t hub_getStatus(usb_dev_t *hub)
{
	uint32_t status = 0;

	if (usb_transferCheck(hub->statusTransfer)) {
		if (hub->statusTransfer->error == 0 && hub->statusTransfer->transferred > 0)
			memcpy(&status, hub->statusTransfer->buffer, sizeof(status));

		hub_poll(hub);
	}

	return status;
}


static void hub_thread(void *args)
{
	hub_event_t *ev;
	uint32_t status;
	int i;

	for (;;) {
		mutexLock(hub_common.lock);
		while (hub_common.events == NULL)
			condWait(hub_common.cond, hub_common.lock, 0);
		ev = hub_common.events;
		LIST_REMOVE(&hub_common.events, ev);
		mutexUnlock(hub_common.lock);

		status = hub_getStatus(ev->hub);
		for (i = 0; i < ev->hub->nports; i++) {
			if (status & (1 << (i + 1)))
				hub_portstatus(ev->hub, i + 1);
		}

		free(ev);
	}
}


void hub_notify(usb_dev_t *hub)
{
	hub_event_t *e;

	if ((e = malloc(sizeof(hub_event_t))) == NULL)
		return;

	e->hub = hub;

	mutexLock(hub_common.lock);
	LIST_ADD(&hub_common.events, e);
	condSignal(hub_common.cond);
	mutexUnlock(hub_common.lock);
}


int hub_conf(usb_dev_t *hub)
{
	char buf[15];
	usb_hub_desc_t *desc;
	int i;

	if (hub_setConf(hub, 1) < 0) {
		USB_LOG("hub: Fail to set configuration!\n");
		return -EINVAL;
	}

	if (hub_getDesc(hub, buf, sizeof(buf)) < 0) {
		USB_LOG("hub: Fail to get descriptor\n");
		return -EINVAL;
	}

	/* Hub descriptors might vary in size */
	desc = (usb_hub_desc_t *)buf;
	hub->nports = min(USB_HUB_MAX_PORTS, desc->bNbrPorts);
	if ((hub->devs = calloc(hub->nports, sizeof(usb_dev_t *))) == NULL) {
		USB_LOG("hub: Out of memory!\n");
		return -ENOMEM;
	}

	for (i = 0; i < hub->nports; i++) {
		if (hub_setPortPower(hub, i + 1) < 0) {
			USB_LOG("hub: Fail to set port %d power!\n", i + 1);
			free(hub->devs);
			return -EINVAL;
		}
	}

	if (hub_interruptInit(hub) != 0)
		return -EINVAL;

	return hub_poll(hub);
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
