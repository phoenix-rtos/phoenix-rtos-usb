/*
 * Phoenix-RTOS
 *
 * cdc - USB Communication Device Class
 *
 * Copyright 2019-2021 Phoenix Systems
 * Author: Hubert Buczynski, Gerard Swiderski
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */


#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/list.h>
#include <usbclient.h>
#include "cdc_client.h"

struct {
	usb_desc_list_t *descList;

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
	usb_desc_list_t strMan;
	usb_desc_list_t strProd;

	usb_cdc_line_coding_t lineCoding;

	void *ctxUser;
	void (*cbEvent)(int, void *);
} cdc_common;


/* Device descriptor */
static const usb_device_desc_t dDev = {
	.bLength = sizeof(usb_device_desc_t),
	.bDescriptorType = USB_DESC_DEVICE,
	.bcdUSB = 0x0002,
	.bDeviceClass = 0x0,
	.bDeviceSubClass = 0,
	.bDeviceProtocol = 0,
	.bMaxPacketSize0 = 64,
	.idVendor = 0x16f9,
	.idProduct = 0x0003,
	.bcdDevice = 0x0200,
	.iManufacturer = 1,
	.iProduct = 2,
	.iSerialNumber = 0,
	.bNumConfigurations = 1
};


/* Configuration descriptor */
static const usb_configuration_desc_t dConfig = {
	.bLength = 9,
	.bDescriptorType = USB_DESC_CONFIG,
	.wTotalLength = sizeof(usb_configuration_desc_t) + sizeof(usb_interface_desc_t) + sizeof(usb_desc_cdc_header_t)
		+ sizeof(usb_desc_cdc_call_t) + sizeof(usb_desc_cdc_acm_t) + sizeof(usb_desc_cdc_union_t)
		+ sizeof(usb_endpoint_desc_t) + sizeof(usb_interface_desc_t) + sizeof(usb_endpoint_desc_t)
		+ sizeof(usb_endpoint_desc_t),
	.bNumInterfaces = 2,
	.bConfigurationValue = 1,
	.iConfiguration = 0,
	.bmAttributes = 0xc0,
	.bMaxPower = 5
};


/* Communication Interface Descriptor */
static const usb_interface_desc_t dComIface = {
	.bLength = 9,
	.bDescriptorType = USB_DESC_INTERFACE,
	.bInterfaceNumber = 0,
	.bAlternateSetting = 0,
	.bNumEndpoints = 1,
	.bInterfaceClass = 0x02,
	.bInterfaceSubClass = 0x02,
	.bInterfaceProtocol = 0x00,
	.iInterface = 4
};


static const usb_desc_cdc_header_t dHeader = {
	.bLength = 5,
	.bType = USB_DESC_TYPE_CDC_CS_INTERFACE,
	.bSubType = 0,
	.bcdCDC = 0x0110
};


static const usb_desc_cdc_call_t dCall = {
	.bLength = 5,
	.bType = USB_DESC_TYPE_CDC_CS_INTERFACE,
	.bSubType = 0x01,
	.bmCapabilities = 0x01,
	.bDataInterface = 0x1
};


static const usb_desc_cdc_acm_t dAcm = {
	.bLength = 4,
	.bType = USB_DESC_TYPE_CDC_CS_INTERFACE,
	.bSubType = 0x02,
	.bmCapabilities = 0x03
};


static const usb_desc_cdc_union_t dUnion = {
	.bLength = 5,
	.bType = USB_DESC_TYPE_CDC_CS_INTERFACE,
	.bSubType = 0x06,
	.bControlInterface = 0x0,
	.bSubordinateInterface = 0x1
};


/* Communication Interrupt Endpoint IN */
static const usb_endpoint_desc_t dComEp = {
	.bLength = 7,
	.bDescriptorType = USB_DESC_ENDPOINT,
	.bEndpointAddress = 0x80 | CDC_ENDPT_IRQ, /* direction IN */
	.bmAttributes = 0x03,
	.wMaxPacketSize = 0x20,
	.bInterval = 0x08
};


/* CDC Data Interface Descriptor */
static const usb_interface_desc_t dDataIface = {
	.bLength = 9,
	.bDescriptorType = USB_DESC_INTERFACE,
	.bInterfaceNumber = 1,
	.bAlternateSetting = 0,
	.bNumEndpoints = 2,
	.bInterfaceClass = 0x0a,
	.bInterfaceSubClass = 0x00,
	.bInterfaceProtocol = 0x00,
	.iInterface = 0
};


/* Data Bulk Endpoint OUT */
static const usb_endpoint_desc_t dEpOUT = {
	.bLength = 7,
	.bDescriptorType = USB_DESC_ENDPOINT,
	.bEndpointAddress = 0x00 | CDC_ENDPT_BULK, /* direction OUT */
	.bmAttributes = 0x02,
	.wMaxPacketSize = 0x0200,
	.bInterval = 0
};


/* Data Bulk Endpoint IN */
static const usb_endpoint_desc_t dEpIN = {
	.bLength = 7,
	.bDescriptorType = USB_DESC_ENDPOINT,
	.bEndpointAddress = 0x80 | CDC_ENDPT_BULK, /* direction IN */
	.bmAttributes = 0x02,
	.wMaxPacketSize = 0x0200,
	.bInterval = 1
};


/* String Data: Manufacturer = "Phoenix Systems" */
static const usb_string_desc_t dStrMan = {
	.bLength = 2 * 15 + 2,
	.bDescriptorType = USB_DESC_STRING,
	.wData = { 'P', 0, 'h', 0, 'o', 0, 'e', 0, 'n', 0, 'i', 0, 'x', 0, ' ', 0, 'S', 0, 'y', 0, 's', 0, 't', 0, 'e', 0, 'm', 0, 's', 0 }
};


/* String Data: Language Identifier = 0x0409 (U.S. English) */
static const usb_string_desc_t dStr0 = {
	.bLength = 4,
	.bDescriptorType = USB_DESC_STRING,
	.wData = { 0x09, 0x04 }
};


/* String Data: Product = "Virtual COM Port" */
static const usb_string_desc_t dStrProd = {
	.bLength = 2 * 16 + 2,
	.bDescriptorType = USB_DESC_STRING,
	.wData = { 'V', 0, 'i', 0, 'r', 0, 't', 0, 'u', 0, 'a', 0, 'l', 0, ' ', 0, 'C', 0, 'O', 0, 'M', 0, ' ', 0, 'P', 0, 'o', 0, 'r', 0, 't', 0 }
};


static int cdc_classSetup(const usb_setup_packet_t *setup, void *buf, unsigned int len, void *ctxUser)
{
	switch (setup->bRequest) {
		case CLASS_REQ_SET_LINE_CODING:
			/* receive lineCoding set request from host */
			memcpy(&cdc_common.lineCoding, buf, sizeof(usb_cdc_line_coding_t));
			if (cdc_common.cbEvent)
				cdc_common.cbEvent(CDC_EV_LINECODING, ctxUser);

			/* send ACK */
			return CLASS_SETUP_ACK;

		case CLASS_REQ_GET_LINE_CODING:
			memcpy(buf, &cdc_common.lineCoding, sizeof(usb_cdc_line_coding_t));

			/* send lineCoding response to host */
			return sizeof(usb_cdc_line_coding_t);

		case CLASS_REQ_SET_CONTROL_LINE_STATE:
			if (cdc_common.cbEvent) {
				if (setup->wValue & 0x3)
					cdc_common.cbEvent(CDC_EV_CARRIER_ACTIVATE, ctxUser);
				else
					cdc_common.cbEvent(CDC_EV_CARRIER_DEACTIVATE, ctxUser);
			}

			/* send ACK */
			return CLASS_SETUP_ACK;

		default:
			/* not handled */
			return CLASS_SETUP_NOACTION;
	}

	/* never reached */
}


static void cdc_eventNotify(int evType, void *ctxUser)
{
	if (!cdc_common.cbEvent)
		return;

	switch (evType) {
		case USBCLIENT_EV_CONNECT:
			cdc_common.cbEvent(CDC_EV_CONNECT, ctxUser);
			return;

		case USBCLIENT_EV_DISCONNECT:
			cdc_common.cbEvent(CDC_EV_DISCONNECT, ctxUser);
			return;

		case USBCLIENT_EV_INIT:
			cdc_common.cbEvent(CDC_EV_INIT, ctxUser);
			return;

		case USBCLIENT_EV_RESET:
			cdc_common.cbEvent(CDC_EV_RESET, ctxUser);
			return;

		case USBCLIENT_EV_CONFIGURED:
		default:
			return;
	}
}


int cdc_init(void (*cbEvent)(int, void *), void *ctxUser)
{
	cdc_common.descList = NULL;

	cdc_common.cbEvent = cbEvent;
	cdc_common.ctxUser = ctxUser;

	cdc_common.dev.descriptor = (usb_functional_desc_t *)&dDev;
	LIST_ADD(&cdc_common.descList, &cdc_common.dev);

	cdc_common.conf.descriptor = (usb_functional_desc_t *)&dConfig;
	LIST_ADD(&cdc_common.descList, &cdc_common.conf);

	cdc_common.comIface.descriptor = (usb_functional_desc_t *)&dComIface;
	LIST_ADD(&cdc_common.descList, &cdc_common.comIface);

	cdc_common.header.descriptor = (usb_functional_desc_t *)&dHeader;
	LIST_ADD(&cdc_common.descList, &cdc_common.header);

	cdc_common.call.descriptor = (usb_functional_desc_t *)&dCall;
	LIST_ADD(&cdc_common.descList, &cdc_common.call);

	cdc_common.acm.descriptor = (usb_functional_desc_t *)&dAcm;
	LIST_ADD(&cdc_common.descList, &cdc_common.acm);

	cdc_common.unio.descriptor = (usb_functional_desc_t *)&dUnion;
	LIST_ADD(&cdc_common.descList, &cdc_common.unio);

	cdc_common.comEp.descriptor = (usb_functional_desc_t *)&dComEp;
	LIST_ADD(&cdc_common.descList, &cdc_common.comEp);

	cdc_common.dataIface.descriptor = (usb_functional_desc_t *)&dDataIface;
	LIST_ADD(&cdc_common.descList, &cdc_common.dataIface);

	cdc_common.dataEpOUT.descriptor = (usb_functional_desc_t *)&dEpOUT;
	LIST_ADD(&cdc_common.descList, &cdc_common.dataEpOUT);

	cdc_common.dataEpIN.descriptor = (usb_functional_desc_t *)&dEpIN;
	LIST_ADD(&cdc_common.descList, &cdc_common.dataEpIN);

	cdc_common.str0.descriptor = (usb_functional_desc_t *)&dStr0;
	LIST_ADD(&cdc_common.descList, &cdc_common.str0);

	cdc_common.strMan.descriptor = (usb_functional_desc_t *)&dStrMan;
	LIST_ADD(&cdc_common.descList, &cdc_common.strMan);

	cdc_common.strProd.descriptor = (usb_functional_desc_t *)&dStrProd;
	LIST_ADD(&cdc_common.descList, &cdc_common.strProd);

	cdc_common.strProd.next = NULL;

	/*
	 * Default COM configuration as 115200 bps, 8-bit data width, 1 bit stop bit, no-parity
	 */
	cdc_common.lineCoding.dwDTERate = 115200;
	cdc_common.lineCoding.bCharFormat = 0;
	cdc_common.lineCoding.bParityType = 0;
	cdc_common.lineCoding.bDataBits = 8;

	usbclient_setUserContext(ctxUser);
	usbclient_setEventCallback(cdc_eventNotify);
	usbclient_setClassCallback(cdc_classSetup);

	return usbclient_init(cdc_common.descList);
}


void cdc_destroy(void)
{
	if (cdc_common.descList != NULL)
		usbclient_destroy();
}


int cdc_send(int endpt, const void *data, unsigned int len)
{
	if (cdc_common.descList != NULL)
		return usbclient_send(endpt, data, len);
	else
		return -ENXIO;
}


int cdc_recv(int endpt, void *data, unsigned int len)
{
	if (cdc_common.descList != NULL)
		return usbclient_receive(endpt, data, len);
	else
		return -ENXIO;
}


usb_cdc_line_coding_t cdc_getLineCoding(void)
{
	return cdc_common.lineCoding;
}


void cdc_setLineCoding(usb_cdc_line_coding_t lineCoding)
{
	cdc_common.lineCoding = lineCoding;
}
