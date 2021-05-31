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

typedef struct hcd_ops {
	struct hcd_ops *next, *prev;

	const char type[HCD_TYPE_LEN];

	int (*init)(hcd_t *);
	int (*transferEnqueue)(hcd_t *, usb_transfer_t *);
	void (*devDestroy)(hcd_t *, usb_device_t *);
} hcd_ops_t;

typedef struct hcd {
	struct hcd *prev, *next;
	const hcd_info_t *info;
	const hcd_ops_t *ops;
	usb_device_t *roothub;

	uint32_t addrmask[4];
	usb_transfer_t *transfers;
	handle_t transfer_lock;
	volatile int *base, *phybase;
	void *priv;
} hcd_t;



void hcd_register(hcd_ops_t *ops);

#endif