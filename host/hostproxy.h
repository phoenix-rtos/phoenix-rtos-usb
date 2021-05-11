/*
 * Phoenix-RTOS
 *
 * USB Host Proxy
 *
 * host/hostproxy.h
 *
 * Copyright 2018, 2020 Phoenix Systems
 * Author: Kamil Amanowicz, Hubert Buczynski
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#ifndef _HOST_PROXY_H_
#define _HOST_PROXY_H_


#include <hostsrv.h>


typedef void (*hostproxy_event_cb)(usb_event_t *event, void *data, size_t size);


int hostproxy_init(void);


int hostproxy_connect(usb_device_id_t *deviceId, hostproxy_event_cb event_cb);


int hostproxy_open(usb_open_t *open);


int hostproxy_write(usb_urb_t *urb, void *data, size_t size);


int hostproxy_read(usb_urb_t *urb, void *data, size_t size);


int hostproxy_reset(int deviceId);


int hostproxy_clear(void);


int hostproxy_exit(void);


void hostproxy_dumpConfiguration(FILE *stream, usb_configuration_desc_t *desc);


#endif
