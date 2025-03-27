/*
 * Phoenix-RTOS
 *
 * Libusb driver interface
 *
 * Common definitions for interstack and events APIs
 *
 * Copyright 2025 Phoenix Systems
 * Author: Adam Greloch
 *
 * %LICENSE%
 */


#ifndef _USB_COMMON_H_
#define _USB_COMMON_H_


#define USB_DRVNAME_MAX 10
#define USB_STR_MAX     255

#define USBDRV_ANY ((unsigned)-1)


typedef struct {
	unsigned vid;
	unsigned pid;
	unsigned dclass;
	unsigned subclass;
	unsigned protocol;
} usb_device_id_t;


#endif /* _USB_COMMON_H_ */
