/*
 * Phoenix-RTOS
 *
 * Libusb process driver interface
 *
 * libusb/usbprocdriver.h
 *
 * Copyright 2024 Phoenix Systems
 * Author: Adam Greloch
 *
 * %LICENSE%
 */

#ifndef _PROCDRIVER_H_
#define _PROCDRIVER_H_

#include <stdbool.h>
#include <usbdriver.h>


int usb_driverProcRun(usb_driver_t *driver, void *args);


#endif
