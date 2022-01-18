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

#include "dev.h"

#define USB_PORT_STAT_CONNECTION  0x0001
#define USB_PORT_STAT_ENABLE      0x0002
#define USB_PORT_STAT_SUSPEND     0x0004
#define USB_PORT_STAT_OVERCURRENT 0x0008
#define USB_PORT_STAT_RESET       0x0010
#define USB_PORT_STAT_POWER       0x0100
#define USB_PORT_STAT_LOW_SPEED   0x0200
#define USB_PORT_STAT_HIGH_SPEED  0x0400
#define USB_PORT_STAT_TEST        0x0800
#define USB_PORT_STAT_INDICATOR   0x1000

#define USB_PORT_STAT_C_CONNECTION  0x01
#define USB_PORT_STAT_C_ENABLE      0x02
#define USB_PORT_STAT_C_SUSPEND     0x04
#define USB_PORT_STAT_C_OVERCURRENT 0x08
#define USB_PORT_STAT_C_RESET       0x10

#define USB_PORT_FEAT_CONNECTION     0
#define USB_PORT_FEAT_ENABLE         1
#define USB_PORT_FEAT_SUSPEND        2
#define USB_PORT_FEAT_OVER_CURRENT   3
#define USB_PORT_FEAT_RESET          4
#define USB_PORT_FEAT_POWER          8
#define USB_PORT_FEAT_LOWSPEED       9
#define USB_PORT_FEAT_C_CONNECTION   16
#define USB_PORT_FEAT_C_ENABLE       17
#define USB_PORT_FEAT_C_SUSPEND      18
#define USB_PORT_FEAT_C_OVER_CURRENT 19
#define USB_PORT_FEAT_C_RESET        20
#define USB_PORT_FEAT_TEST           21
#define USB_PORT_FEAT_INDICATOR      22

#define USB_HUB_MAX_PORTS 15

typedef struct usb_port_status {
	uint16_t wPortStatus;
	uint16_t wPortChange;
} __attribute__((packed)) usb_port_status_t;


typedef struct usb_hub_desc {
	uint8_t bDescLength;
	uint8_t bDescriptorType;
	uint8_t bNbrPorts;
	uint16_t wHubCharacteristics;
	uint8_t bPwrOn2PwrGood;
	uint8_t bHubContrCurrent;
	uint8_t variable[];
} __attribute__((packed)) usb_hub_desc_t;


int hub_init(void);


int hub_conf(usb_dev_t *hub);


void hub_notify(usb_dev_t *hub);


void hub_interrupt(void);


#endif
