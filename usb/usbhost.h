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
#include <sys/msg.h>
#include <usbdriver.h>
#include <usb.h>


typedef struct usb_driver {
	struct usb_driver *next, *prev;
	unsigned pid;
	unsigned port;
	usb_device_id_t filter;
} usb_driver_t;


typedef struct usb_endpoint {
	struct usb_endpoint *prev, *next;

	usb_transfer_type_t type;
	usb_dir_t direction;

	int max_packet_len;
	int number;
	int interval;
	int connected;
	int interface;

	struct usb_device *device;
	void *hcdpriv;
} usb_endpoint_t;


typedef struct usb_pipe {
	idnode_t linkage;
	struct usb_driver *drv;
	struct usb_endpoint *ep;
} usb_pipe_t;


typedef struct usb_interface {
	usb_interface_desc_t *desc;
	void *classDesc;
	char *str;

	usb_driver_t *driver;
} usb_interface_t;


typedef struct usb_device {
	enum { usb_full_speed = 0, usb_low_speed, usb_high_speed } speed;
	usb_device_desc_t desc;
	usb_configuration_desc_t *conf;
	char *manufacturer;
	char *product;
	char *serialNumber;
	uint16_t langId;

	usb_endpoint_t *eps;
	struct hcd *hcd;

	struct usb_interface *ifs;
	int nifs;
	struct usb_device **devices;
	struct usb_device *hub;
	int ndevices;
	struct usb_hub_ops *hubOps;

	int address;
} usb_device_t;


typedef struct usb_urb_handler {
	struct usb_urb_handler *prev, *next;
	struct usb_transfer *transfer;
	unsigned long rid;
	msg_t msg;
	unsigned int port;
} usb_urb_handler_t;


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

	handle_t *cond;
	usb_urb_handler_t *handler;

	void *hcdpriv;
} usb_transfer_t;

void usb_transferFinished(usb_transfer_t *t);

#endif
