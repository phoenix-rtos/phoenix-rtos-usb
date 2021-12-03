/*
 * Phoenix-RTOS
 *
 * hid_client - USB Human Interface Device
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
#include <sys/list.h>
#include "usbclient.h"
#include "hid_client.h"


struct {
	usb_desc_list_t *descList;

	usb_desc_list_t dev;
	usb_desc_list_t conf;
	usb_desc_list_t iface;

	usb_desc_list_t hid;
	usb_desc_list_t ep;

	usb_desc_list_t str0;
	usb_desc_list_t strMan;
	usb_desc_list_t strProd;

	usb_desc_list_t hidReport;
} hid_common;


static const usb_hid_desc_report_t dHidReport = {
	.bLength = 2 + 76,
	.bType = USB_DESC_TYPE_HID_REPORT,
	.wData = {
		/* Raw HID report descriptor - compatibile with IMX6ULL SDP protocol */
		0x06, 0x00, 0xff, 0x09, 0x01, 0xa1, 0x01, 0x85, 0x01, 0x19,
		0x01, 0x29, 0x01, 0x15, 0x00, 0x26, 0xff, 0x00, 0x75, 0x08,
		0x95, 0x10, 0x91, 0x02, 0x85, 0x02, 0x19, 0x01, 0x29, 0x01,
		0x15, 0x00, 0x26, 0xff, 0x00, 0x75, 0x80, 0x95, 0x40, 0x91,
		0x02, 0x85, 0x03, 0x19, 0x01, 0x29, 0x01, 0x15, 0x00, 0x26,
		0xff, 0x00, 0x75, 0x08, 0x95, 0x04, 0x81, 0x02, 0x85, 0x04,
		0x19, 0x01, 0x29, 0x01, 0x15, 0x00, 0x26, 0xff, 0x00, 0x75,
		0x08, 0x95, 0x40, 0x81, 0x02, 0xc0 }
};


static const usb_endpoint_desc_t dEp = {
	.bLength = 7,
	.bDescriptorType = USB_DESC_ENDPOINT,
	.bEndpointAddress = 0x80 | HID_ENDPT_IRQ, /* direction IN */
	.bmAttributes = 0x03,
	.wMaxPacketSize = 64,
	.bInterval = 0x01
};


/* USB HID Descriptor */
static const usb_hid_desc_t dHid = {
	.bLength = 9,
	.bType = USB_DESC_TYPE_HID,
	.bcdHID = 0x0110,
	.bCountryCode = 0x00,
	.bNumDescriptors = 1,
	.bDescriptorType = 0x22,
	.wDescriptorLength = 76
};


/* Interface Descriptor */
static const usb_interface_desc_t dIface = {
	.bLength = 9,
	.bDescriptorType = USB_DESC_INTERFACE,
	.bInterfaceNumber = 0,
	.bAlternateSetting = 0,
	.bNumEndpoints = 1,
	.bInterfaceClass = 0x03,
	.bInterfaceSubClass = 0x00,
	.bInterfaceProtocol = 0x00,
	.iInterface = 2
};


/* Configuration descriptor */
static const usb_configuration_desc_t dConfig = {
	.bLength = 9,
	.bDescriptorType = USB_DESC_CONFIG,
	.wTotalLength = sizeof(usb_configuration_desc_t) + sizeof(usb_interface_desc_t) + sizeof(dHid) + sizeof(usb_endpoint_desc_t),
	.bNumInterfaces = 1,
	.bConfigurationValue = 1,
	.iConfiguration = 1,
	.bmAttributes = 0xc0,
	.bMaxPower = 5
};


/* String Data: Language Identifier = 0x0409 (U.S. English) */
static const usb_string_desc_t dStr0 = {
	.bLength = 4,
	.bDescriptorType = USB_DESC_STRING,
	.wData = { 0x09, 0x04 }
};


static int hid_classSetup(const usb_setup_packet_t *setup, void *buf, unsigned int len, void *ctxUser)
{
	switch (setup->bRequest) {
		case CLASS_REQ_SET_IDLE:
			/* reply with ACK */
			return CLASS_SETUP_ACK;

		case CLASS_REQ_SET_REPORT:
			/* notify endpoint that data is ready */
			return CLASS_SETUP_ENDP0;

		case CLASS_REQ_GET_IDLE:
		case CLASS_REQ_GET_PROTOCOL:
		case CLASS_REQ_GET_REPORT:
		case CLASS_REQ_SET_PROTOCOL:
		default:
			/* not handled */
			return CLASS_SETUP_NOACTION;
	}

	/* never reached */
}


int hid_init(const usb_hid_dev_setup_t *dev_setup)
{
	hid_common.descList = NULL;

	hid_common.dev.descriptor = (usb_functional_desc_t *)&dev_setup->dDevice;
	LIST_ADD(&hid_common.descList, (usb_desc_list_t *)&hid_common.dev);

	hid_common.conf.descriptor = (usb_functional_desc_t *)&dConfig;
	LIST_ADD(&hid_common.descList, (usb_desc_list_t *)&hid_common.conf);

	hid_common.iface.descriptor = (usb_functional_desc_t *)&dIface;
	LIST_ADD(&hid_common.descList, &hid_common.iface);

	hid_common.hid.descriptor = (usb_functional_desc_t *)&dHid;
	LIST_ADD(&hid_common.descList, &hid_common.hid);

	hid_common.ep.descriptor = (usb_functional_desc_t *)&dEp;
	LIST_ADD(&hid_common.descList, &hid_common.ep);

	hid_common.str0.descriptor = (usb_functional_desc_t *)&dStr0;
	LIST_ADD(&hid_common.descList, &hid_common.str0);

	hid_common.strMan.descriptor = (usb_functional_desc_t *)&dev_setup->dStrMan;
	LIST_ADD(&hid_common.descList, &hid_common.strMan);

	hid_common.strProd.descriptor = (usb_functional_desc_t *)&dev_setup->dStrProd;
	LIST_ADD(&hid_common.descList, &hid_common.strProd);

	hid_common.hidReport.descriptor = (usb_functional_desc_t *)&dHidReport;
	LIST_ADD(&hid_common.descList, &hid_common.hidReport);

	hid_common.hidReport.next = NULL;

	usbclient_setClassCallback(hid_classSetup);

	return usbclient_init(hid_common.descList);
}


void hid_destroy(void)
{
	if (hid_common.descList != NULL)
		usbclient_destroy();
}


int hid_send(int endpt, const void *data, unsigned int len)
{
	if (hid_common.descList != NULL)
		return usbclient_send(endpt, data, len);
	else
		return -ENXIO;
}


int hid_recv(int endpt, void *data, unsigned int len)
{
	if (hid_common.descList != NULL)
		return usbclient_receive(endpt, data, len);
	else
		return -ENXIO;
}
