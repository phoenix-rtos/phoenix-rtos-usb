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

#define USB_CONNECT_WILDCARD ((unsigned)-1)


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
	int device_id;
	int pipe;
	setup_packet_t setup;
} usb_urb_t;


typedef struct {
	int device_id;
	endpoint_desc_t endpoint;
} usb_open_t;


typedef struct {
	enum { usb_msg_connect, usb_msg_urb, usb_msg_open } type;

	union {
		usb_connect_t connect;
		usb_urb_t urb;
		usb_open_t open;
	};
} usb_msg_t;


typedef struct {
	int id;
} usb_insertion_t;


typedef struct {
	int device_id;
} usb_removal_t;


typedef struct {
	enum { usb_event_insertion, usb_event_removal, usb_event_interrupt } type;

	union {
		usb_insertion_t insertion;
		usb_removal_t removal;
	};
} usb_event_t;

#endif
