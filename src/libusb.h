/*
 * Phoenix-RTOS
 *
 * USB library
 *
 * usb/libusb.h
 *
 * Copyright 2018 Phoenix Systems
 * Author: Kamil Amanowicz
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#ifndef _LIBUSB_H_
#define _LIBUSB_H_

#include "usb.h"
#include "usbd.h"

typedef void (*libusb_event_cb)(usb_event_t *event, void *data, size_t size);


int libusb_init(void);


int libusb_connect(usb_device_id_t *deviceId, libusb_event_cb event_cb);  


int libusb_open(usb_open_t *open);


int libusb_write(usb_urb_t *urb, void *data, size_t size);


int libusb_read(usb_urb_t *urb, void *data, size_t size);


int libusb_exit(void);


#endif
