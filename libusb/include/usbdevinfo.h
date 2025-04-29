/*
 * Phoenix-RTOS
 *
 * Libusb driver interface
 *
 * USB low-level information API for userspace applications
 *
 * Copyright 2025 Phoenix Systems
 * Author: Adam Greloch
 *
 * %LICENSE%
 */


#ifndef _USB_DEVINFO_H_
#define _USB_DEVINFO_H_

#include <usb.h>
#include <usbdriver.h>


int usb_devinfoGet(oid_t oid, usb_devinfo_desc_t *desc);


#endif
