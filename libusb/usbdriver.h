#ifndef _USBDRIVER_H_
#define _USBDRIVER_H_

#include <usb.h>

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
	enum { usb_transfer_control, usb_transfer_interrupt, usb_transfer_bulk, usb_transfer_isochronous } type;
	enum { usb_transfer_in, usb_transfer_out } direction;
	int device_id;
	int pipe;
	int transfer_size;
	int async;
	usb_setup_packet_t setup;
} usb_urb_t;


typedef struct {
	int device_id;
	usb_endpoint_desc_t endpoint;
} usb_open_t;


typedef struct {
	int device_id;
} usb_reset_t;


typedef struct usb_device_instance {
	struct usb_device_instance *prev, *next;
	usb_device_desc_t descriptor;
	int bus;
	int dev;
	int interface;
} usb_device_instance_t;


typedef struct {
	usb_device_desc_t descriptor;
	int bus;
	int dev;
	int interface;
} usb_insertion_t;

typedef struct {
	int bus;
	int dev;
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


int usb_connect(usb_device_id_t *id, int port);
int usb_eventsWait(int port, usb_insertion_t *insertion, usb_deletion_t *deletion);


#endif /* _USBDRIVER_H_ */