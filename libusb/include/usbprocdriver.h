/*
 * Phoenix-RTOS
 *
 * Libusb process driver interface
 *
 * libusb/usbprocdriver.h
 *
 * Copyright 2024, 2026 Phoenix Systems
 * Author: Adam Greloch, Julian Uziembło
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _PROCDRIVER_H_
#define _PROCDRIVER_H_

#include <stdbool.h>
#include <usbdriver.h>

/* One of the threads in the thread pool replaces the calling thread instead of being run separately. */
#define USBPROCDRV_FLAG_REPLACE_CURRENT (1U << 0)


/*
 * Initializes the given driver and spawns a thread pool of nthreads of prio priority to handle usbhost events.
 * Threads concurrently process insertion/deletion/completion events with provided driver->handlers.
 */
int usb_driverThreadpoolSpawn(usb_driver_t *drv, int prio, unsigned int nthreads, void *args, unsigned int flags);


/* Destroy the proc driver run by usb_driverThreadpoolSpawn or usb_driverProcRun. */
int usb_driverThreadpoolDestroy(usb_driver_t *drv);


/* Wrapper around usb_driverThreadpoolSpawn that exits on error or on thread pool destroy. */
__attribute__((noreturn)) void usb_driverProcRun(usb_driver_t *driver, int prio, unsigned int nthreads, void *args);


#endif
