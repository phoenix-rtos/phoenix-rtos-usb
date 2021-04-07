/*
 * Phoenix-RTOS
 *
 * cdc - USB Communication Device Class
 *
 * Copyright 2019 Phoenix Systems
 * Author: Hubert Buczynski
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */


#ifndef _CDC_CLIENT_H_
#define _CDC_CLIENT_H_

#include <cdc.h>


int cdc_init(void);


void cdc_destroy(void);


int cdc_send(int endpt, const char *data, unsigned int len);


int cdc_recv(int endpt, char *data, unsigned int len);


#endif
