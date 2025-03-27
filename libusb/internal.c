/*
 * Phoenix-RTOS
 *
 * libusb/internal.c
 *
 * Copyright 2025 Phoenix Systems
 * Author: Adam Greloch
 *
 * %LICENSE%
 */


#include <usbinternal.h>
#include <unistd.h>


void usb_hostLookup(oid_t *oid)
{
	for (;;) {
		if (lookup("devfs/usb", NULL, oid) >= 0) {
			break;
		}

		if (lookup("/dev/usb", NULL, oid) >= 0) {
			break;
		}

		usleep(1000000);
	}
}
