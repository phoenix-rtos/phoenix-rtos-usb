/*
 * Phoenix-RTOS
 *
 * USB driver
 *
 * Copyright 2018 Phoenix Systems
 * Author: Jan Sikorski
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#ifndef _USB_DESCRIPTORS_H_
#define _USB_DESCRIPTORS_H_

#define REQUEST_DIR_HOST2DEV  (0 << 7)
#define REQUEST_DIR_DEV2HOST  (1 << 7)
#define REQUEST_DIR_MASK      (1 << 7)

#define REQUEST_TYPE_STANDARD  (0 << 5)
#define REQUEST_TYPE_CLASS     (1 << 5)
#define REQUEST_TYPE_VENDOR    (2 << 5)

#define REQUEST_RECIPIENT_DEVICE    0
#define REQUEST_RECIPIENT_INTERFACE 1
#define REQUEST_RECIPIENT_ENDPOINT  2
#define REQUEST_RECIPIENT_OTHER     3

/* request types */
#define GET_STATUS 0
#define CLEAR_FEATURE 1
#define SET_FEATURE 3
#define SET_ADDRESS 5
#define GET_DESCRIPTOR 6
#define SET_DESCRIPTOR 7
#define GET_CONFIGURATION 8
#define SET_CONFIGURATION 9
#define GET_INTERFACE 10
#define SET_INTERFACE 11
#define SYNCH_FRAME 12

/* descriptor types */
#define DESC_DEVICE 1
#define DESC_CONFIG 2
#define DESC_STRING 3
#define DESC_INTERFACE 4
#define DESC_ENDPOINT 5
#define DESC_INTERFACE_ASSOCIATION 11

/* Endpoint feature */
#define ENDPOINT_HALT 0

/* class specific desctriptors */
#define DESC_CS_INTERFACE 0x24
#define DESC_CS_ENDPOINT 0x25


#define USB_TIMEOUT 5000000


enum { pid_out = 0xe1, pid_in = 0x69, pid_setup = 0x2d };

enum { out_token = 0, in_token, setup_token };


typedef struct setup_packet {
	unsigned char  bmRequestType;
	unsigned char  bRequest;
	unsigned short wValue;
	unsigned short wIndex;
	unsigned short wLength;
} __attribute__((packed)) setup_packet_t;


struct desc_header {
	unsigned char bLength;
	unsigned char bDescriptorType;
};


typedef struct device_desc {
	unsigned char  bLength;
	unsigned char  bDescriptorType;
	unsigned short bcdUSB;
	unsigned char  bDeviceClass;
	unsigned char  bDeviceSubClass;
	unsigned char  bDeviceProtocol;
	unsigned char  bMaxPacketSize0;
	unsigned short idVendor;
	unsigned short idProduct;
	unsigned short bcdDevice;
	unsigned char  iManufacturer;
	unsigned char  iProduct;
	unsigned char  iSerialNumber;
	unsigned char  bNumConfigurations;
} __attribute__((packed)) device_desc_t;


typedef struct configuration_desc {
	unsigned char  bLength;
	unsigned char  bDescriptorType;
	unsigned short wTotalLength;
	unsigned char  bNumInterfaces;
	unsigned char  bConfigurationValue;
	unsigned char  iConfiguration;
	unsigned char  bmAttributes;
	unsigned char  bMaxPower;
} __attribute__((packed)) configuration_desc_t;


typedef struct interface_desc {
	unsigned char bLength;
	unsigned char bDescriptorType;
	unsigned char bInterfaceNumber;
	unsigned char bAlternateSetting;
	unsigned char bNumEndpoints;
	unsigned char bInterfaceClass;
	unsigned char bInterfaceSubClass;
	unsigned char bInterfaceProtocol;
	unsigned char iInterface;
} __attribute__((packed)) interface_desc_t;


typedef struct interface_association_desc {
	unsigned char bLength;
	unsigned char bDescriptorType;
	unsigned char bFirstInterface;
	unsigned char bInterfaceCount;
	unsigned char bFunctionClass;
	unsigned char bFunctionSubClass;
	unsigned char bFunctionProtocol;
	unsigned char iFunction;
} __attribute__((packed)) interface_association_desc_t;


typedef struct string_desc {
	unsigned char  bLength;
	unsigned char  bDescriptorType;
	unsigned short wData[1];
} __attribute__((packed)) string_desc_t;


typedef struct endpoint_desc {
	unsigned char  bLength;
	unsigned char  bDescriptorType;
	unsigned char  bEndpointAddress;
	unsigned char  bmAttributes;
	unsigned short wMaxPacketSize;
	unsigned char  bInterval;
} __attribute__((packed)) endpoint_desc_t;


typedef struct functional_desc {
	unsigned char bFunctionLength;
	unsigned char bDescriptorType;
	unsigned char bDescriptorSubtype;
} __attribute__((packed)) functional_desc_t;


#endif
