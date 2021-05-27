/*
 * Phoenix-RTOS
 *
 * USB Host
 *
 * Copyright 2021 Phoenix Systems
 * Author: Maciej Purski
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#ifndef _USB_HOST_H_
#define _USB_HOST_H_

#include <sys/types.h>
#include <usb.h>

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
	usb_setup_packet_t setup;
} usb_urb_t;


typedef struct {
	int device_id;
	usb_endpoint_desc_t endpoint;
} usb_open_t;


typedef struct {
	int device_id;
} usb_reset_t;


typedef struct {
	enum { usb_msg_connect, usb_msg_urb, usb_msg_open, usb_msg_reset } type;

	union {
		usb_connect_t connect;
		usb_urb_t urb;
		usb_open_t open;
		usb_reset_t reset;
	};
} usb_msg_t;


typedef struct {
	usb_device_desc_t descriptor;
} usb_insertion_t;


typedef struct {
	int transfer_id;
	int pipe;
	int error;
} usb_completion_t;


typedef struct {
	enum { usb_event_insertion, usb_event_removal, usb_event_completion, usb_event_reset } type;

	int device_id;

	union {
		usb_insertion_t insertion;
		usb_completion_t completion;
	};
} usb_event_t;


/* Internal structures */
typedef struct {
	unsigned pid;
	unsigned port;
	usb_device_id_t filter;
	struct usb_device *devices;
} usb_driver_t;


typedef struct usb_endpoint {
	struct usb_device *device;
	enum { usb_ep_control, usb_ep_interrupt, usb_ep_bulk, usb_ep_isochronous } type;
	enum { usb_ep_in, usb_ep_out, usb_ep_bi } direction;

	int max_packet_len;
	int number;

	void *hcdpriv;
} usb_endpoint_t;


typedef struct usb_device {
	struct usb_device *next, *prev;

	enum { usb_full_speed = 0, usb_low_speed, usb_high_speed } speed;
	usb_device_desc_t descriptor;

	char name[32];
	usb_driver_t *driver;
	usb_endpoint_t *ep0;
	struct hcd *hcd;

	int address;
} usb_device_t;

typedef struct usb_transfer {
	struct usb_transfer *next, *prev;
	struct usb_endpoint *endpoint;
	usb_setup_packet_t *setup;

	unsigned async;
	unsigned id;
	volatile int finished;
	volatile int error;
	volatile int aborted;

	char *buffer;
	size_t size;
	int type;
	int direction;

	void *hcdpriv;
} usb_transfer_t;

void usb_transferFinished(usb_transfer_t *t);

#endif
