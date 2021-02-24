/*
 * Phoenix-RTOS
 *
 * hid_client - USB Human Interface Device
 *
 * Copyright 2019 Phoenix Systems
 * Author: Hubert Buczynski
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */


#ifndef _HID_CLIENT_H_
#define _HID_CLIENT_H_


#include <hid.h>


enum {
	/* HID Endpoint Types */
	HID_ENDPT_CTRL,
	HID_ENDPT_IRQ,
};


/* Initialize HID device */
int hid_init(const usb_hid_dev_setup_t* dev_setup);


/* Free resources used by HID device */
void hid_destroy(void);


/* Sends HID data on an given endpoint */
int hid_send(int endpt, const void *data, unsigned int len);


/* Receives HID data on an given endpoint */
int hid_recv(int endpt, void *data, unsigned int len);


#endif
