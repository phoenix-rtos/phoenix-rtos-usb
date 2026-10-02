/*
 * Phoenix-RTOS
 *
 * USB bus monitor - PCAP trace output
 *
 * Copyright 2026 Phoenix Systems
 * Author: Adam Greloch
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */


#ifndef _USBMON_H_
#define _USBMON_H_

#include <errno.h>

#include "usbhost.h"


#ifdef USBMON


/* Initialize the USB monitor and open the PCAP output file. Returns 0 on success, negative errno on failure. */
int usbmon_init(const char *pcapPath);


/* Shut down the USB monitor and close the PCAP file. */
void usbmon_destroy(void);


/* Start capturing to the given PCAPng file (runtime control). Returns 0 on success. */
int usbmon_start(const char *pcapPath, uint32_t snaplen);


/* Stop capturing and close the PCAPng file (runtime control). Returns 0 on success. */
int usbmon_stop(void);


/* Record a submission of t on a pipe. */
void usbmon_submit(usb_transfer_t *t, usb_pipe_t *pipe);


/* Record a completion of t. */
void usbmon_complete(usb_transfer_t *t);


#else /* !USBMON */


static inline int usbmon_init(const char *pcapPath)
{
	(void)pcapPath;
	return 0;
}


static inline void usbmon_destroy(void)
{
}


static inline int usbmon_start(const char *pcapPath, uint32_t snaplen)
{
	(void)pcapPath;
	(void)snaplen;
	return -ENOSYS;
}


static inline int usbmon_stop(void)
{
	return -ENOSYS;
}


static inline void usbmon_submit(usb_transfer_t *t, usb_pipe_t *pipe)
{
	(void)t;
	(void)pipe;
}


static inline void usbmon_complete(usb_transfer_t *t)
{
	(void)t;
}


#endif /* USBMON */

#endif /* _USBMON_H_ */
