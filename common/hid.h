/*
 * Phoenix-RTOS
 *
 * USB Human Interface Device Class definitions
 *
 * Copyright 2020 Phoenix Systems
 * Author: Hubert Buczynski
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */


#ifndef _USB_HID_H_
#define _USB_HID_H_


#include <usb.h>


enum { hidOK = 0, eReport1, eReport2, eReport3, eReport4, eErase, eControlBlock };


typedef struct _usb_hid_dev_setup {
	usb_device_desc_t dDevice;
	usb_string_desc_t dStrMan;
	usb_string_desc_t dStrProd;
} __attribute__((packed)) usb_hid_dev_setup_t;


typedef struct _usb_hid_desc_report {
	uint8_t bLength;
	uint8_t bType;
	uint8_t wData[128];
} __attribute__((packed)) usb_hid_desc_report_t;


typedef struct _usb_hid_desc_t {
	uint8_t bLength;
	uint8_t bType;
	uint16_t bcdHID;
	uint8_t bCountryCode;
	uint8_t bNumDescriptors;
	uint8_t bDescriptorType;
	uint16_t wDescriptorLength;
} __attribute__((packed)) usb_hid_desc_t;


#endif
