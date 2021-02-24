/*
 * Phoenix-RTOS
 *
 * cdc - USB Communication Device Class
 *
 * Copyright 2019-2021 Phoenix Systems
 * Author: Hubert Buczynski, Gerard Swiderski
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */


#ifndef _CDC_CLIENT_H_
#define _CDC_CLIENT_H_


#include <cdc.h>


enum {
	/* CDC Endpoint Types */
	CDC_ENDPT_CTRL,
	CDC_ENDPT_IRQ,
	/* CDC_ACM bulk endpoints are used as IN/OUT pipe communication */
	CDC_ENDPT_BULK
};


enum {
	/* CDC Event Types */
	CDC_EV_DISCONNECT,
	CDC_EV_CONNECT,
	CDC_EV_RESET,
	CDC_EV_INIT,
	CDC_EV_LINECODING,
	CDC_EV_CARRIER_ACTIVATE,
	CDC_EV_CARRIER_DEACTIVATE,
};


/* Initialize CDC device, allocate usb_client resources */
int cdc_init(void (*cbEvent)(int _evType, void *_ctxUser), void *ctxUser);


/* Free CDC device, release usb_client resources */
void cdc_destroy(void);


/* Sends data on an given endpoint */
int cdc_send(int endpt, const void *data, unsigned int len);


/* Receives data on an given endpoint */
int cdc_recv(int endpt, void *data, unsigned int len);


/* Get current Line Coding information */
usb_cdc_line_coding_t cdc_getLineCoding(void);


/* Set Line Coding information */
void cdc_setLineCoding(usb_cdc_line_coding_t lineCoding);


#endif
