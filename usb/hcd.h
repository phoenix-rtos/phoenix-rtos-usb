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

#include "usbhost.h"
#include "dev.h"

#define HCD_TYPE_LEN 5


typedef struct {
	char type[HCD_TYPE_LEN];
	uintptr_t hcdaddr;
	uintptr_t phyaddr;
	int irq;
	int clk;
} hcd_info_t;


typedef struct hcd_ops {
	const char type[HCD_TYPE_LEN];

	int (*init)(struct hcd *);
	int (*transferEnqueue)(struct hcd *, usb_transfer_t *);
	void (*transferDequeue)(struct hcd *, usb_transfer_t *);
	int (*roothubTransfer)(struct hcd *, usb_transfer_t *);
	void (*pipeDestroy)(struct hcd *, usb_pipe_t *);
	uint32_t (*getRoothubStatus)(usb_dev_t *);
} hcd_ops_t;


typedef struct hcd {
	struct hcd *prev, *next;
	char stack[4096] __attribute__((aligned(8)));
	const hcd_info_t *info;
	const hcd_ops_t *ops;
	usb_dev_t *roothub;
	usb_transfer_t *status;
	int num;

	uint32_t addrmask[4];
	usb_transfer_t *transfers;
	usb_transfer_t *finished;
	handle_t transLock;
	handle_t transCond;
	handle_t blockCond;
	time_t time;
	volatile int *base, *phybase;
	void *priv;
} hcd_t;


void hcd_register(const hcd_ops_t *ops);


int hcd_getInfo(const hcd_info_t **info);


int hcd_addrAlloc(hcd_t *hcd);


void hcd_addrFree(hcd_t *hcd, int addr);


hcd_t *hcd_find(hcd_t *hcdList, uint32_t locationID);


hcd_t *hcd_init(void);


int hcd_transfer(hcd_t *hcd, usb_transfer_t *t, int block);


void hcd_notify(hcd_t *hcd);


void hcd_portStatus(hcd_t *hcd);


#endif
