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


#include <errno.h>
#include <stdio.h>

#include <usbclient.h>

#include "cdc_client.h"

struct {
	usb_conf_t config;

	usb_desc_list_t dev;
	usb_desc_list_t conf;

	usb_desc_list_t comIface;
	usb_desc_list_t header;
	usb_desc_list_t call;
	usb_desc_list_t acm;
	usb_desc_list_t unio;
	usb_desc_list_t comEp;

	usb_desc_list_t dataIface;
	usb_desc_list_t dataEpOUT;
	usb_desc_list_t dataEpIN;

	usb_desc_list_t str0;
	usb_desc_list_t strman;
	usb_desc_list_t strprod;
} cdc_common;


/* Device descriptor */
static const usb_device_desc_t ddev = { .bLength = sizeof(usb_device_desc_t), .bDescriptorType = USB_DESC_DEVICE, .bcdUSB = 0x0002,
	.bDeviceClass = 0x0, .bDeviceSubClass = 0, .bDeviceProtocol = 0, .bMaxPacketSize0 = 64,
	.idVendor = 0x16f9, .idProduct = 0x0003, .bcdDevice = 0x0200,
	.iManufacturer = 1, .iProduct = 2, .iSerialNumber = 0,
	.bNumConfigurations = 1 };


/* Configuration descriptor */
static const usb_configuration_desc_t dconfig = { .bLength = 9, .bDescriptorType = USB_DESC_CONFIG,
	.wTotalLength = sizeof(usb_configuration_desc_t) + sizeof(usb_interface_desc_t) + sizeof(usb_desc_cdc_header_t) + sizeof(usb_desc_cdc_call_t)
	+ sizeof(usb_desc_cdc_acm_t) + sizeof(usb_desc_cdc_union_t) + sizeof(usb_endpoint_desc_t) + sizeof(usb_interface_desc_t) + sizeof(usb_endpoint_desc_t) + sizeof(usb_endpoint_desc_t),
	.bNumInterfaces = 2, .bConfigurationValue = 1, .iConfiguration = 0, .bmAttributes = 0xc0, .bMaxPower = 5 };


/* Communication Interface Descriptor */
static const usb_interface_desc_t dComIntf =  { .bLength = 9, .bDescriptorType = USB_DESC_INTERFACE, .bInterfaceNumber = 0, .bAlternateSetting = 0,
	.bNumEndpoints = 1, .bInterfaceClass = 0x02, .bInterfaceSubClass = 0x02, .bInterfaceProtocol = 0x00, .iInterface = 4 };


static const usb_desc_cdc_header_t dHeader = { .bLength = 5, .bType = USB_DESC_TYPE_CDC_CS_INTERFACE, .bSubType = 0, .bcdCDC = 0x0110 };


static const usb_desc_cdc_call_t dCall = { .bLength = 5, .bType = USB_DESC_TYPE_CDC_CS_INTERFACE, .bSubType = 0x01, .bmCapabilities = 0x01, .bDataInterface = 0x1 };


static const usb_desc_cdc_acm_t dAcm = { .bLength = 4, .bType = USB_DESC_TYPE_CDC_CS_INTERFACE, .bSubType = 0x02, .bmCapabilities = 0x03 };


static const usb_desc_cdc_union_t dUnion = { .bLength = 5, .bType = USB_DESC_TYPE_CDC_CS_INTERFACE, .bSubType = 0x06, .bControlInterface = 0x0, .bSubordinateInterface = 0x1};


/* Communication Interrupt Endpoint IN */
static const usb_endpoint_desc_t dComEp = { .bLength = 7, .bDescriptorType = USB_DESC_ENDPOINT, .bEndpointAddress = 0x83	, /* direction IN */
	.bmAttributes = 0x03, .wMaxPacketSize = 16, .bInterval = 0x08
};


/* CDC Data Interface Descriptor */
static const usb_interface_desc_t dDataIntf = { .bLength = 9, .bDescriptorType = USB_DESC_INTERFACE, .bInterfaceNumber = 1, .bAlternateSetting = 0,
	 .bNumEndpoints = 2, .bInterfaceClass = 0x0a, .bInterfaceSubClass = 0x00, .bInterfaceProtocol = 0x00, .iInterface = 0
};


/* Data Bulk Endpoint OUT */
static const usb_endpoint_desc_t depOUT = { .bLength = 7, .bDescriptorType = USB_DESC_ENDPOINT, .bEndpointAddress = 0x01, /* direction OUT */
	.bmAttributes = 0x02, .wMaxPacketSize = 0x0200, .bInterval = 0
};


/* Data Bulk Endpoint IN */
static const usb_endpoint_desc_t depIN = { .bLength = 7, .bDescriptorType = USB_DESC_ENDPOINT, .bEndpointAddress = 0x82, /* direction IN */
	.bmAttributes = 0x02, .wMaxPacketSize = 0x0200, .bInterval = 1
};


/* String Data */
static const usb_string_desc_t dstrman = {
	.bLength = 2 * 18 + 2,
	.bDescriptorType = USB_DESC_STRING,
	.wData = {	'N', 0, 'X', 0, 'P', 0, ' ', 0, 'S', 0, 'E', 0, 'M', 0, 'I', 0, 'C', 0, 'O', 0, 'N', 0, 'D', 0, 'U', 0, 'C', 0, 'T', 0,
				'O', 0, 'R', 0, 'S', 0 }
};


static const usb_string_desc_t dstr0 = {
	.bLength = 4,
	.bDescriptorType = USB_DESC_STRING,
	.wData = {0x04, 0x09} /* English */
};


static const usb_string_desc_t dstrprod = {
	.bLength = 11 * 2 + 2,
	.bDescriptorType = USB_DESC_STRING,
	.wData = { 'M', 0, 'C', 0, 'U', 0, ' ', 0, 'V', 0, 'I', 0, 'R', 0, 'T', 0, 'U', 0, 'A', 0, 'L', 0 }
};


int cdc_recv(char *data, unsigned int len)
{
	return usbclient_receive(&cdc_common.config.endpoint_list.endpoints[2], data, len);
}


int cdc_send(const char *data, unsigned int len)
{
	return usbclient_send(&cdc_common.config.endpoint_list.endpoints[2], data, len);
}


void cdc_destroy(void)
{
	usbclient_destroy();
}


int cdc_init(void)
{
	int res;

	usb_endpoint_list_t endpoints = {
		.size = 3,
		.endpoints = {
			{ .id = 0, .type = USB_ENDPT_TYPE_CONTROL, .direction = 0 },
			{ .id = 1, .type = USB_ENDPT_TYPE_INTR, .direction = USB_ENDPT_DIR_IN },
			{ .id = 2, .type = USB_ENDPT_TYPE_BULK, .direction = USB_ENDPT_DIR_IN },
			{ .id = 3, .type = USB_ENDPT_TYPE_BULK, .direction = USB_ENDPT_DIR_OUT }
		}
	};

	cdc_common.dev.size = 1;
	cdc_common.dev.descriptors = (usb_functional_desc_t *)&ddev;
	cdc_common.dev.next = &cdc_common.conf;

	cdc_common.conf.size = 1;
	cdc_common.conf.descriptors = (usb_functional_desc_t *)&dconfig;
	cdc_common.conf.next = &cdc_common.comIface;

	cdc_common.comIface.size = 1;
	cdc_common.comIface.descriptors = (usb_functional_desc_t *)&dComIntf;
	cdc_common.comIface.next = &cdc_common.header;

	cdc_common.header.size = 1;
	cdc_common.header.descriptors = (usb_functional_desc_t *)&dHeader;
	cdc_common.header.next = &cdc_common.call;

	cdc_common.call.size = 1;
	cdc_common.call.descriptors = (usb_functional_desc_t *)&dCall;
	cdc_common.call.next = &cdc_common.acm;

	cdc_common.acm.size = 1;
	cdc_common.acm.descriptors = (usb_functional_desc_t *)&dAcm;
	cdc_common.acm.next = &cdc_common.unio;

	cdc_common.unio.size = 1;
	cdc_common.unio.descriptors = (usb_functional_desc_t *)&dComEp;
	cdc_common.unio.next = &cdc_common.comEp;

	cdc_common.comEp.size = 1;
	cdc_common.comEp.descriptors = (usb_functional_desc_t *)&dUnion;
	cdc_common.comEp.next = &cdc_common.dataIface;

	cdc_common.dataIface.size = 1;
	cdc_common.dataIface.descriptors = (usb_functional_desc_t *)&dDataIntf;
	cdc_common.dataIface.next = &cdc_common.dataEpOUT;

	cdc_common.dataEpOUT.size = 1;
	cdc_common.dataEpOUT.descriptors = (usb_functional_desc_t *)&depOUT;
	cdc_common.dataEpOUT.next = &cdc_common.dataEpIN;

	cdc_common.dataEpIN.size = 1;
	cdc_common.dataEpIN.descriptors = (usb_functional_desc_t *)&depIN;
	cdc_common.dataEpIN.next = &cdc_common.str0;

	cdc_common.str0.size = 1;
	cdc_common.str0.descriptors = (usb_functional_desc_t *)&dstr0;
	cdc_common.str0.next = &cdc_common.strman;

	cdc_common.strman.size = 1;
	cdc_common.strman.descriptors = (usb_functional_desc_t *)&dstrman;
	cdc_common.strman.next = &cdc_common.strprod;

	cdc_common.strprod.size = 1;
	cdc_common.strprod.descriptors = (usb_functional_desc_t *)&dstrprod;
	cdc_common.strprod.next = NULL;

	cdc_common.config.endpoint_list = endpoints;
	cdc_common.config.descriptors_head = &cdc_common.dev;

	if ((res = usbclient_init(&cdc_common.config)) != EOK)
		return res;

	return EOK;
}
