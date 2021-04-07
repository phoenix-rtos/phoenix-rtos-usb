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


int hid_init(const usb_hid_dev_setup_t* dev_setup);


void hid_destroy(void);


int hid_send(int endpt, const char *data, unsigned int len);


int hid_recv(int endpt, char *data, unsigned int len);


#endif
