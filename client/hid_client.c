/*
 * Phoenix-RTOS
 *
 * hid_client - USB Human Interface Device
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
#include <sys/list.h>

#include <usbclient.h>

#include "hid_client.h"


struct {
	usb_desc_list_t *descList;

	usb_desc_list_t dev;
	usb_desc_list_t conf;
	usb_desc_list_t iface;

	usb_desc_list_t hid;
	usb_desc_list_t ep;

	usb_desc_list_t str0;
	usb_desc_list_t strman;
	usb_desc_list_t strprod;

	usb_desc_list_t hidreport;
} hid_common;


static const usb_hid_desc_report_t dhidreport = { 2 + 76, USB_DESC_TYPE_HID_REPORT,
	/* Raw HID report descriptor - compatibile with IMX6ULL SDP protocol */
	{ 0x06, 0x00, 0xff, 0x09, 0x01, 0xa1, 0x01, 0x85, 0x01, 0x19,
	  0x01, 0x29, 0x01, 0x15, 0x00, 0x26, 0xff, 0x00, 0x75, 0x08,
	  0x95, 0x10, 0x91, 0x02, 0x85, 0x02, 0x19, 0x01, 0x29, 0x01,
	  0x15, 0x00, 0x26, 0xff, 0x00, 0x75, 0x80, 0x95, 0x40, 0x91,
	  0x02, 0x85, 0x03, 0x19, 0x01, 0x29, 0x01, 0x15, 0x00, 0x26,
	  0xff, 0x00, 0x75, 0x08, 0x95, 0x04, 0x81, 0x02, 0x85, 0x04,
	  0x19, 0x01, 0x29, 0x01, 0x15, 0x00, 0x26, 0xff, 0x00, 0x75,
	  0x08, 0x95, 0x40, 0x81, 0x02, 0xc0 }
};


static const usb_endpoint_desc_t dep = { .bLength = 7, .bDescriptorType = USB_DESC_ENDPOINT, .bEndpointAddress = 0x81, /* direction IN */
	.bmAttributes = 0x03, .wMaxPacketSize = 64, .bInterval = 0x01
};


static const usb_hid_desc_t dhid = { 9, USB_DESC_TYPE_HID, 0x0110, 0x0, 1, 0x22, 76 };


static const usb_interface_desc_t diface = { .bLength = 9, .bDescriptorType = USB_DESC_INTERFACE, .bInterfaceNumber = 0, .bAlternateSetting = 0,
	.bNumEndpoints = 1, .bInterfaceClass = 0x03, .bInterfaceSubClass = 0x00, .bInterfaceProtocol = 0x00, .iInterface = 2
};


static const usb_configuration_desc_t dconfig = { .bLength = 9, .bDescriptorType = USB_DESC_CONFIG,
	.wTotalLength = sizeof(usb_configuration_desc_t) + sizeof(usb_interface_desc_t) + sizeof(dhid) + sizeof(usb_endpoint_desc_t),
	.bNumInterfaces = 1, .bConfigurationValue = 1, .iConfiguration = 1, .bmAttributes = 0xc0, .bMaxPower = 5
};


static const usb_string_desc_t dstr0 = {
	.bLength = 4,
	.bDescriptorType = USB_DESC_STRING,
	.wData = {0x04, 0x09} /* English */
};


int hid_recv(int endpt, char *data, unsigned int len)
{
	return usbclient_receive(endpt, data, len);
}


int hid_send(int endpt, const char *data, unsigned int len)
{
	return usbclient_send(endpt, data, len);
}


void hid_destroy(void)
{
	usbclient_destroy();
}


int hid_init(const usb_hid_dev_setup_t *dev_setup)
{
	int res = EOK;

	hid_common.dev.descriptor = (usb_functional_desc_t *)&dev_setup->dDevice;
	LIST_ADD(&hid_common.descList, (usb_desc_list_t *)&hid_common.dev);

	hid_common.conf.descriptor = (usb_functional_desc_t *)&dconfig;
	LIST_ADD(&hid_common.descList, (usb_desc_list_t *)&hid_common.conf);

	hid_common.iface.descriptor = (usb_functional_desc_t *)&diface;
	LIST_ADD(&hid_common.descList, &hid_common.iface);

	hid_common.hid.descriptor = (usb_functional_desc_t *)&dhid;
	LIST_ADD(&hid_common.descList, &hid_common.hid);

	hid_common.ep.descriptor = (usb_functional_desc_t *)&dep;
	LIST_ADD(&hid_common.descList, &hid_common.ep);

	hid_common.str0.descriptor = (usb_functional_desc_t *)&dstr0;
	LIST_ADD(&hid_common.descList, &hid_common.str0);

	hid_common.strman.descriptor = (usb_functional_desc_t *)&dev_setup->dStrMan;
	LIST_ADD(&hid_common.descList, &hid_common.strman);

	hid_common.strprod.descriptor = (usb_functional_desc_t *)&dev_setup->dStrProd;
	LIST_ADD(&hid_common.descList, &hid_common.strprod);

	hid_common.hidreport.descriptor = (usb_functional_desc_t *)&dhidreport;
	LIST_ADD(&hid_common.descList, &hid_common.hidreport);

	hid_common.hidreport.next = NULL;

	if ((res = usbclient_init(hid_common.descList)) != EOK)
		return res;

	return res;
}
