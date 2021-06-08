#ifndef _USBDRIVER_H_
#define _USBDRIVER_H_

#include <usb.h>
#include <stddef.h>
#include <sys/msg.h>
#include <posix/idtree.h>

#define USB_CONNECT_WILDCARD ((unsigned)-1)
#define USB_CONNECT_NONE ((unsigned)-2)

typedef struct {
	unsigned vid;
	unsigned pid;
	unsigned dclass;
	unsigned subclass;
	unsigned protocol;
} usb_device_id_t;


typedef struct {
	unsigned port;
	usb_device_id_t filter;
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

int usb_connect(usb_device_id_t *id, int drvport);
int usb_eventsWait(int port, msg_t *msg);
int usb_open(usb_insertion_t *dev, usb_transfer_type_t type, usb_dir_t dir);
int usb_transferControl(unsigned pipe, usb_setup_packet_t *setup, void *data, size_t size, usb_dir_t dir);
int usb_transferBulk(unsigned pipe, void *data, size_t size, usb_dir_t dir);
int usb_setConfiguration(unsigned pipe, int conf);

#endif /* _USBDRIVER_H_ */