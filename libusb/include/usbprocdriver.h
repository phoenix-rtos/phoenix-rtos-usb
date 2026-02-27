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


/*
 * Initializes the given driver and spawns a thread pool of nthreads of prio priority to handle usbhost events.
 * Threads concurrently process insertion/deletion/completion events with provided driver->handlers. Calls exit(1)
 * on any failure.
 */
__attribute__((noreturn)) void usb_driverProcRun(usb_driver_t *driver, unsigned int prio, unsigned int nthreads, void *args);


#endif
