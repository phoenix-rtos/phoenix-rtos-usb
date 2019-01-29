/*
 * Phoenix-RTOS
 *
 * USB driver
 *
 * usb/usbd.h
 *
 * Copyright 2018 Phoenix Systems
 * Author: Jan Sikorski
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#ifndef _USBD_H_
#define _USBD_H_

#include "usb.h"

#define USB_CONNECT_WILDCARD ((unsigned)-1)
#define USB_CONNECT_NONE ((unsigned)-2)


typedef struct {
	unsigned idVendor;
	unsigned idProduct;
	unsigned bcdDevice;
	unsigned bDeviceClass;
	unsigned bDeviceSubClass;
	unsigned bDeviceProtocol;
} usb_device_id_t;


typedef struct {
	unsigned port;
	usb_device_id_t filter;
} usb_connect_t;


typedef struct {
	enum { usb_transfer_control, usb_transfer_interrupt, usb_transfer_bulk, usb_transfer_isochronous } type;
	enum { usb_transfer_in, usb_transfer_out } direction;
	int device_id;
	int pipe;
	int transfer_size;
	int async;
	setup_packet_t setup;
} usb_urb_t;


typedef struct {
	int device_id;
	endpoint_desc_t endpoint;
} usb_open_t;


typedef struct {
	enum { usb_msg_connect, usb_msg_urb, usb_msg_open, usb_msg_clear } type;

	union {
		usb_connect_t connect;
		usb_urb_t urb;
		usb_open_t open;
	};
} usb_msg_t;


typedef struct {
	int device_id;
	device_desc_t descriptor;
} usb_insertion_t;


typedef struct {
	int device_id;
} usb_removal_t;


typedef struct {
	int transfer_id;
	int pipe;
} usb_completion_t;


typedef struct {
	enum { usb_event_insertion, usb_event_removal, usb_event_completion } type;

	union {
		usb_insertion_t insertion;
		usb_removal_t removal;
		usb_completion_t completion;
	};
} usb_event_t;

#endif
