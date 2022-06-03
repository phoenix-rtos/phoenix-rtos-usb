
/*
 * Phoenix-RTOS
 *
 * Libusb driver interface
 *
 * libusb/usbdriver.h
 *
 * Copyright 2021 Phoenix Systems
 * Author: Maciej Purski
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#ifndef _USBDRIVER_H_
#define _USBDRIVER_H_

#include <usb.h>
#include <stddef.h>
#include <sys/msg.h>
#include <posix/idtree.h>

#define USBDRV_ANY ((unsigned)-1)


enum {
	usbdrv_nomatch = 0x0,
	usbdrv_match = 0x01,
	usbdrv_class_match = 0x02,
	usbdrv_subclass_match = 0x04,
	usbdrv_protocol_match = 0x08,
	usbdrv_vid_match = 0x10,
	usbdrv_pid_match = 0x20
};


typedef struct {
	unsigned vid;
	unsigned pid;
	unsigned dclass;
	unsigned subclass;
	unsigned protocol;
} usb_device_id_t;


typedef struct {
	unsigned port;
	unsigned nfilters;
} usb_connect_t;


typedef struct {
	int pipe;
	int size;
	usb_setup_packet_t setup;
	usb_dir_t dir;
	int type;
	int sync;
} usb_urb_t;


typedef struct {
	int bus;
	int dev;
	int iface;
	unsigned locationID;
	usb_transfer_type_t type;
	usb_dir_t dir;
} usb_open_t;


typedef struct {
	usb_device_desc_t descriptor;
	char manufacturer[32];
	char product[32];
	char serialNumber[32];
	int bus;
	int dev;
	int interface;
	unsigned locationID;
} usb_devinfo_t;


typedef struct {
	int bus;
	int dev;
	int interface;
} usb_deletion_t;


typedef struct {
	int pipeid;
	int urbid;
	size_t size;
	usb_setup_packet_t setup;
	enum {
		urbcmd_submit,
		urbcmd_cancel,
		urbcmd_free } cmd;
} usb_urbcmd_t;


typedef struct {
	int pipeid;
	int urbid;
	size_t transferred;
	int err;
} usb_completion_t;


typedef struct {
	enum { usb_msg_connect,
		usb_msg_insertion,
		usb_msg_deletion,
		usb_msg_urb,
		usb_msg_open,
		usb_msg_urbcmd,
		usb_msg_completion } type;

	union {
		usb_connect_t connect;
		usb_urb_t urb;
		usb_urbcmd_t urbcmd;
		usb_open_t open;
		usb_devinfo_t insertion;
		usb_deletion_t deletion;
		usb_completion_t completion;
	};
} usb_msg_t;


typedef struct {
	uint16_t vid;
	uint16_t pid;
	uint8_t msg[31];
	int scsiresp;
} usb_modeswitch_t;


int usb_modeswitchHandle(usb_devinfo_t *dev, const usb_modeswitch_t *mode);


const usb_modeswitch_t *usb_modeswitchFind(uint16_t vid, uint16_t pid, const usb_modeswitch_t *modes, int nmodes);


int usb_connect(const usb_device_id_t *filters, int nfilters, unsigned drvport);


int usb_eventsWait(int port, msg_t *msg);


int usb_open(usb_devinfo_t *dev, usb_transfer_type_t type, usb_dir_t dir);


int usb_transferControl(unsigned pipe, usb_setup_packet_t *setup, void *data, size_t size, usb_dir_t dir);


int usb_transferBulk(unsigned pipe, void *data, size_t size, usb_dir_t dir);


int usb_transferAsync(unsigned pipe, unsigned urbid, size_t size, usb_setup_packet_t *setup);


int usb_setConfiguration(unsigned pipe, int conf);


int usb_urbAlloc(unsigned pipe, void *data, usb_dir_t dir, size_t size, int type);


int usb_urbFree(unsigned pipe, unsigned urb);


int usb_clearFeatureHalt(unsigned pipe, int ep);


void usb_dumpDeviceDescriptor(FILE *stream, usb_device_desc_t *descr);


void usb_dumpConfigurationDescriptor(FILE *stream, usb_configuration_desc_t *descr);


void usb_dumpInferfaceDesc(FILE *stream, usb_interface_desc_t *descr);


void usb_dumpEndpointDesc(FILE *stream, usb_endpoint_desc_t *descr);


void usb_dumpStringDesc(FILE *stream, usb_string_desc_t *descr);


#endif /* _USBDRIVER_H_ */
