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


#ifndef _HUB_H_
#define _HUB_H_

#include <sys/types.h>
#include <usbhost.h>
#include <hcd.h>


#define USB_PORT_STAT_CONNECTION    0x0001
#define USB_PORT_STAT_ENABLE        0x0002
#define USB_PORT_STAT_SUSPEND       0x0004
#define USB_PORT_STAT_OVERCURRENT   0x0008
#define USB_PORT_STAT_RESET         0x0010
#define USB_PORT_STAT_POWER         0x0100
#define USB_PORT_STAT_LOW_SPEED     0x0200
#define USB_PORT_STAT_HIGH_SPEED    0x0400
#define USB_PORT_STAT_TEST          0x0800
#define USB_PORT_STAT_INDICATOR     0x1000

#define USB_PORT_STAT_C_CONNECTION  0x01
#define USB_PORT_STAT_C_ENABLE      0x02
#define USB_PORT_STAT_C_SUSPEND     0x04
#define USB_PORT_STAT_C_OVERCURRENT 0x08
#define USB_PORT_STAT_C_RESET       0x10

#define USB_PORT_FEAT_CONNECTION		0
#define USB_PORT_FEAT_ENABLE			1
#define USB_PORT_FEAT_SUSPEND			2
#define USB_PORT_FEAT_OVER_CURRENT		3
#define USB_PORT_FEAT_RESET				4
#define USB_PORT_FEAT_POWER				8
#define USB_PORT_FEAT_LOWSPEED			9	/* Should never be used */
#define USB_PORT_FEAT_C_CONNECTION		16
#define USB_PORT_FEAT_C_ENABLE			17
#define USB_PORT_FEAT_C_SUSPEND			18
#define USB_PORT_FEAT_C_OVER_CURRENT	19
#define USB_PORT_FEAT_C_RESET			20
#define USB_PORT_FEAT_TEST              21
#define USB_PORT_FEAT_INDICATOR         22


typedef struct usb_port_status {
	uint16_t wPortStatus;
	uint16_t wPortChange;
} __attribute__ ((packed)) usb_port_status_t;

typedef struct usb_port {
	usb_device_t *device;
	int num;
} usb_port_t;


typedef struct usb_hub {
	struct usb_hub *prev, *next;

	usb_device_t *dev;
	int nports;
	usb_port_t *ports;
	hcd_t *hcd;
	handle_t lock;

	int (*statusChanged)(struct usb_hub *hub, uint32_t *status);
	int (*getPortStatus)(struct usb_hub *hub, int port, usb_port_status_t *status);
	int (*clearPortFeature)(struct usb_hub *hub, int port, uint16_t wValue);
	int (*setPortFeature)(struct usb_hub *hub, int port, uint16_t wValue);
} usb_hub_t;

#endif