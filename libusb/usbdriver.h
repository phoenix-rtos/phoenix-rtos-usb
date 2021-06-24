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
} usb_urb_t;


typedef struct {
	int bus;
	int dev;
	int iface;
	usb_transfer_type_t type;
	usb_dir_t dir;
} usb_open_t;


typedef struct {
	int device_id;
} usb_reset_t;


typedef struct {
	usb_device_desc_t descriptor;
	int bus;
	int dev;
	int interface;
} usb_insertion_t;


typedef struct {
	int bus;
	int dev;
	int interface;
} usb_deletion_t;


typedef struct {
	enum { usb_msg_connect, usb_msg_insertion, usb_msg_deletion, usb_msg_urb, usb_msg_open, usb_msg_reset } type;

	union {
		usb_connect_t connect;
		usb_urb_t urb;
		usb_open_t open;
		usb_reset_t reset;
		usb_insertion_t insertion;
		usb_deletion_t deletion;
	};
} usb_msg_t;

int usb_connect(const usb_device_id_t *filters, int nfilters, unsigned drvport);
int usb_eventsWait(int port, msg_t *msg);
int usb_open(usb_insertion_t *dev, usb_transfer_type_t type, usb_dir_t dir);
int usb_transferControl(unsigned pipe, usb_setup_packet_t *setup, void *data, size_t size, usb_dir_t dir);
int usb_transferBulk(unsigned pipe, void *data, size_t size, usb_dir_t dir);
int usb_setConfiguration(unsigned pipe, int conf);
int usb_clearFeatureHalt(unsigned pipe, int ep);

#endif /* _USBDRIVER_H_ */