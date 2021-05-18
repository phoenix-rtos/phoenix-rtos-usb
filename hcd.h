/*
 * Phoenix-RTOS
 *
 * USB Host Controller Device
 *
 * Copyright 2021 Phoenix Systems
 * Author: Maciej Purski
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */


#ifndef _HCD_H_
#define _HCD_H_

#include <usbhost.h>

#define HCD_TYPE_LEN 5

typedef struct {
	char type[HCD_TYPE_LEN];
	uintptr_t hcdaddr, phyaddr; /* Physical addresses */
	int irq, clk;
} hcd_info_t;


typedef struct hcd hcd_t;
typedef struct usb_hub usb_hub_t;

typedef struct hcd_ops {
	struct hcd_ops *next, *prev;

	const char type[HCD_TYPE_LEN];

	int (*init)(hcd_t *);
	int (*urbSubmit)(hcd_t *, usb_urb_t *);
} hcd_ops_t;

typedef struct hcd {
	const hcd_info_t *info;
	const hcd_ops_t *ops;
	usb_hub_t *roothub;

	volatile int *base, *phybase;
	void *priv;
} hcd_t;

typedef struct usb_bus {
	struct usb_bus *next, *prev;

	int id;
	usb_device_t *devices;
	int ndevices;
	int maxaddr;
	hcd_t *hcd;
} usb_bus_t;


void hcd_register(hcd_ops_t *ops);

#endif