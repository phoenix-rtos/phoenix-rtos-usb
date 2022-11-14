
/*
 * Phoenix-RTOS
 *
 * Libusb driver interface
 *
 * libusb/dev_msg.h
 *
 * Copyright 2021 Phoenix Systems
 * Author: Maciej Purski
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#ifndef _USBDRVMSG_H_
#define _USBDRVMSG_H_


typedef struct {
	int bus;
	int dev;
	int interface;
} usbdrv_deletion_t;


typedef struct {
	int pipeid;
	int urbid;
	size_t size;
	addr_t physaddr;
	usb_setup_packet_t setup;
	enum {
		urbcmd_submit,
		urbcmd_cancel,
		urbcmd_free } cmd;
} usbdrv_in_urbcmd_t;


typedef struct {
	size_t size;
} usbdrv_in_alloc_t;


typedef struct {
	int pipeid;
	usb_dir_t dir;
	int type;
} usbdrv_in_urballoc_t;


typedef struct {
	int urbid;
} usbdrv_out_urballoc_t;


typedef struct {
	enum {
		usbdrv_insertion_event,
		usbdrv_deletion_event,
		usbdrv_completion_event
	} type;
	
	int type;
	int bus;
	int dev;
	int interface;
	unsigned locationID;

	/* Completion event only */
	int urbid;
	size_t transferred;
	int status;
} usbdrv_event_t;


typedef struct {
	int maxevents;
} usbdrv_in_wait_t;


typedef struct {
	int nevents;
} usbdrv_out_wait_t;


typedef struct {
	addr_t physaddr;
	int err;
} usbdrv_out_alloc_t;


typedef struct {
	size_t size;
	addr_t physaddr;
} usbdrv_in_free_t;


typedef struct {
	int pipeid;
	int urbid;
	size_t transferred;
	int err;
} usbdrv_completion_t;


typedef struct {
	unsigned port;
	unsigned nfilters;
} usbdrv_in_connect_t;


typedef struct {
	int pipeid;
	size_t size;
	usb_setup_packet_t setup;
	usb_dir_t dir;
	int type;
	addr_t physaddr;
	unsigned int timeout;
} usbdrv_in_submit_t;


typedef struct {
	size_t transferred;
} usbdrv_out_submit_t;

typedef struct {
	int bus;
	int dev;
	int iface;
	unsigned locationID;
	usb_transfer_type_t type;
	usb_dir_t dir;
} usbdrv_in_open_t;


typedef struct {
	unsigned int id;
	unsigned int epnum;
	int err;
} usbdrv_out_open_t;


typedef struct {
	enum {
		usbdrv_msg_wait,
		usbdrv_msg_alloc,
		usbdrv_msg_free,
		usbdrv_msg_connect,
		usbdrv_msg_submit,
		usbdrv_msg_urb,
		usbdrv_msg_open,
		usbdrv_msg_urballoc,
		usbdrv_msg_urbcmd } type;

	union {
		usbdrv_in_alloc_t alloc;
		usbdrv_in_wait_t wait;
		usbdrv_in_free_t free;
		usbdrv_in_submit_t submit;
		usbdrv_in_urballoc_t urballoc;
		usbdrv_in_connect_t connect;
		usbdrv_in_urbcmd_t urbcmd;
		usbdrv_in_open_t open;
	};
} usbdrv_in_msg_t;

typedef struct {
	union {
		usbdrv_out_alloc_t alloc;
		usbdrv_out_open_t open;
		usbdrv_out_submit_t submit;
		usbdrv_out_urballoc_t urballoc;
		usbdrv_out_wait_t wait;
	};
	int err;
} usbdrv_out_msg_t;

#endif /* _USBDRVMSG_H_ */