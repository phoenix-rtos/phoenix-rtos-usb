
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

#define USBDRV_ANY             ((unsigned)-1)
#define USBDRV_NO_TIMEOUT      0
#define USBDRV_MAX_DEVNAME

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
} usbdrv_devid_t;


typedef struct {
	rbnode_t node;
	int id;
	int pipeid;
	char *vaddr;
} usbdrv_urb_t;


typedef struct {
	int id;
	int bus;
	int dev;
	int iface;
	int epnum;
	unsigned locationID;
	usb_transfer_type_t type;
	usb_dir_t dir;
} usbdrv_pipe_t;


typedef struct {
	idnode_t node;
	usbdrv_dev_t *prev;
	usbdrv_dev_t *next;
	volatile int refcnt;
	int bus;
	int dev;
	int interface;
	unsigned locationID;
	void *ctx;
} usbdrv_dev_t;


typedef struct {
	uint16_t vid;
	uint16_t pid;
	uint8_t msg[31];
	int scsiresp;
} usbdrv_modeswitch_t;


typedef int (*usbdrv_completion_handler_t)(usbdrv_dev_t *dev, usbdrv_urb_t *urb, char *data, size_t transferred, int status);


typedef int (*usbdrv_insertion_handler_t)(usbdrv_dev_t *dev);


typedef int (*usbdrv_deletion_handler_t)(usbdrv_dev_t *dev);


typedef struct {
	usbdrv_insertion_handler_t insertion;
	usbdrv_deletion_handler_t deletion;
	usbdrv_completion_handler_t completion;
} usbdrv_handlers_t;


/* Allocate memory used for usb transfers */
void *usbdrv_alloc(size_t size);

/* Frees memory used for usb transfers. The caller must ensure there is no ongoing URB using this memory chunk */
void usbdrv_free(void *ptr, size_t size);

/* Opens a pipe to communicate with an endpoint with given type and dir. If such an endpoint does not exist on the device, returns NULL */
usbdrv_pipe_t *usbdrv_pipeOpen(usbdrv_dev_t *dev, usb_transfer_type_t type, usb_dir_t dir);

/* Closes a previously opened pipe. It cancels all the ongoing urbs on this pipe */
void usbdrv_pipeClose(usbdrv_pipe_t *pipe);

/* Perform a blocking bulk transfer to a given pipe with timeout in milliseconds, data pointer must be allocated using usbdrv_alloc */
ssize_t usbdrv_transferBulk(usbdrv_pipe_t *pipe, void *data, size_t size, unsigned int timeout);

/* Perform a blocking control transfer to a given pipe with timeot in milliseconds, optional data pointer
 * must be allocated using usbdrv_alloc, direction can be deduced from bmRequestType field */
ssize_t usbdrv_transferControl(usbdrv_pipe_t *pipe, usb_setup_packet_t *setup, void *data, unsigned int timeout);


usbdrv_urb_t *usbdrv_urbAlloc(usbdrv_pipe_t *pipe);


int usbdrv_transferBulkAsync(usbdrv_urb_t *urb, void *data, size_t size, unsigned int timeout);


int usbdrv_transferControlAsync(usbdrv_urb_t *urb, usb_setup_packet_t *setup, void *data, size_t size, unsigned int timeout);


int usbdrv_modeswitchHandle(usbdrv_dev_t *dev, const usbdrv_modeswitch_t *mode);


const usbdrv_modeswitch_t *usbdrv_modeswitchFind(uint16_t vid, uint16_t pid, const usbdrv_modeswitch_t *modes, int nmodes);


int usbdrv_connect(const usbdrv_handlers_t *handlers, const usbdrv_devid_t *filters, unsigned int nfilters);


usbdrv_dev_t *usbdrv_devGet(int id);


int usbdrv_devID(usbdrv_dev_t *dev);


void usbdrv_devPut(usbdrv_dev_t *dev);


int usbdrv_eventsWait(int port, msg_t *msg);


int usbdrv_setConfiguration(usbdrv_pipe_t *pipe, int conf);


int usbdrv_clearFeatureHalt(usbdrv_pipe_t *pipe);



int usbdrv_urbFree(usbdrv_pipe_t *pipe, unsigned urb);



void usbdrv_dumpDeviceDescriptor(FILE *stream, usb_device_desc_t *descr);


void usbdrv_dumpConfigurationDescriptor(FILE *stream, usb_configuration_desc_t *descr);


void usbdrv_dumpInferfaceDesc(FILE *stream, usb_interface_desc_t *descr);


void usbdrv_dumpEndpointDesc(FILE *stream, usb_endpoint_desc_t *descr);


void usbdrv_dumpStringDesc(FILE *stream, usb_string_desc_t *descr);


#endif /* _USBDRIVER_H_ */
