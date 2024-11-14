/*
 * Phoenix-RTOS
 *
 * libusb/driver.c
 *
 * Copyright 2021, 2024 Phoenix Systems
 * Author: Maciej Purski, Adam Greloch
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include <errno.h>
#include <sys/threads.h>
#include <sys/list.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#include <usbdriver.h>


static struct {
	usb_driver_t *registeredDrivers;
} usbdrv_common;


void usb_driverRegister(usb_driver_t *driver)
{
	LIST_ADD(&usbdrv_common.registeredDrivers, driver);
}


usb_driver_t *usb_registeredDriverPop(void)
{
	usb_driver_t *drv = usbdrv_common.registeredDrivers;
	if (drv != NULL) {
		LIST_REMOVE(&usbdrv_common.registeredDrivers, drv);
	}
	return drv;
}


int usb_open(usb_driver_t *drv, usb_devinfo_t *dev, usb_transfer_type_t type, usb_dir_t dir)
{
	return drv->pipeOps->open(drv, dev, type, dir);
}


int usb_urbAlloc(usb_driver_t *drv, unsigned pipe, void *data, usb_dir_t dir, size_t size, int type)
{
	return drv->pipeOps->urbAlloc(drv, pipe, data, dir, size, type);
}


int usb_urbFree(usb_driver_t *drv, unsigned pipe, unsigned urb)
{
	return drv->pipeOps->urbFree(drv, pipe, urb);
}


int usb_transferAsync(usb_driver_t *drv, unsigned pipe, unsigned urbid, size_t size, usb_setup_packet_t *setup)
{
	return drv->pipeOps->transferAsync(drv, pipe, urbid, size, setup);
}


int usb_transferControl(usb_driver_t *drv, unsigned pipe, usb_setup_packet_t *setup, void *data, size_t size, usb_dir_t dir)
{
	usb_urb_t urb = {
		.pipe = pipe,
		.setup = *setup,
		.dir = dir,
		.size = size,
		.type = usb_transfer_control,
		.sync = 1
	};

	return drv->pipeOps->submitSync(drv, &urb, data);
}


int usb_transferBulk(usb_driver_t *drv, unsigned pipe, void *data, size_t size, usb_dir_t dir)
{
	usb_urb_t urb = {
		.pipe = pipe,
		.dir = dir,
		.size = size,
		.type = usb_transfer_bulk,
		.sync = 1
	};

	return drv->pipeOps->submitSync(drv, &urb, data);
}


int usb_setConfiguration(usb_driver_t *drv, unsigned pipe, int conf)
{
	usb_setup_packet_t setup = (usb_setup_packet_t) {
		.bmRequestType = REQUEST_DIR_HOST2DEV | REQUEST_TYPE_STANDARD | REQUEST_RECIPIENT_DEVICE,
		.bRequest = REQ_SET_CONFIGURATION,
		.wValue = conf,
		.wIndex = 0,
		.wLength = 0,
	};

	return usb_transferControl(drv, pipe, &setup, NULL, 0, usb_dir_out);
}


int usb_clearFeatureHalt(usb_driver_t *drv, unsigned pipe, int ep)
{
	usb_setup_packet_t setup = (usb_setup_packet_t) {
		.bmRequestType = REQUEST_DIR_HOST2DEV | REQUEST_TYPE_STANDARD | REQUEST_RECIPIENT_DEVICE,
		.bRequest = REQ_CLEAR_FEATURE,
		.wValue = USB_ENDPOINT_HALT,
		.wIndex = ep,
		.wLength = 0,
	};

	return usb_transferControl(drv, pipe, &setup, NULL, 0, usb_dir_out);
}


const usb_modeswitch_t *usb_modeswitchFind(uint16_t vid, uint16_t pid, const usb_modeswitch_t *modes, int nmodes)
{
	int i;

	for (i = 0; i < nmodes; i++) {
		if (vid == modes[i].vid && pid == modes[i].pid)
			return &modes[i];
	}

	return NULL;
}


void usb_dumpDeviceDescriptor(FILE *stream, usb_device_desc_t *descr)
{
	fprintf(stream, "DEVICE DESCRIPTOR:\n");
	fprintf(stream, "\tbLength: %d\n", descr->bLength);
	fprintf(stream, "\tbDescriptorType: 0x%x\n", descr->bDescriptorType);
	fprintf(stream, "\tbcdUSB: %d\n", descr->bcdUSB);
	fprintf(stream, "\tbDeviceClass: %d\n", descr->bDeviceClass);
	fprintf(stream, "\tbDeviceSubClass: %d\n", descr->bDeviceSubClass);
	fprintf(stream, "\tbDeviceProtocol: %d\n", descr->bDeviceProtocol);
	fprintf(stream, "\tbMaxPacketSize0: %d\n", descr->bMaxPacketSize0);
	fprintf(stream, "\tidVendor: 0x%x\n", descr->idVendor);
	fprintf(stream, "\tidProduct: 0x%x\n", descr->idProduct);
	fprintf(stream, "\tbcdDevice: %d\n", descr->bcdDevice);
	fprintf(stream, "\tiManufacturer: %d\n", descr->iManufacturer);
	fprintf(stream, "\tiProduct: %d\n", descr->iProduct);
	fprintf(stream, "\tiSerialNumber: %d\n", descr->iSerialNumber);
	fprintf(stream, "\tbNumConfigurations: %d\n", descr->bNumConfigurations);
}


void usb_dumpConfigurationDescriptor(FILE *stream, usb_configuration_desc_t *descr)
{
	fprintf(stream, "CONFIGURATION DESCRIPTOR:\n");
	fprintf(stream, "\tbLength: %d\n", descr->bLength);
	fprintf(stream, "\tbDescriptorType: 0x%x\n", descr->bDescriptorType);
	fprintf(stream, "\twTotalLength: %d\n", descr->wTotalLength);
	fprintf(stream, "\tbNumInterfaces: %d\n", descr->bNumInterfaces);
	fprintf(stream, "\tbConfigurationValue: %d\n", descr->bConfigurationValue);
	fprintf(stream, "\tiConfiguration %d\n", descr->iConfiguration);
	fprintf(stream, "\tbmAttributes: 0x%x\n", descr->bmAttributes);
	fprintf(stream, "\tbMaxPower: %d\n", descr->bMaxPower);
}


void usb_dumpInferfaceDesc(FILE *stream, usb_interface_desc_t *descr)
{
	fprintf(stream, "INTERFACE DESCRIPTOR:\n");
	fprintf(stream, "\tbLength: %d\n", descr->bLength);
	fprintf(stream, "\tbDescriptorType: 0x%x\n", descr->bDescriptorType);
	fprintf(stream, "\tbInterfaceNumber: %d\n", descr->bInterfaceNumber);
	fprintf(stream, "\tbNumEndpoints: %d\n", descr->bNumEndpoints);
	fprintf(stream, "\tbInterfaceClass: %x\n", descr->bInterfaceClass);
	fprintf(stream, "\tbInterfaceSubClass: 0x%x\n", descr->bInterfaceSubClass);
	fprintf(stream, "\tbInterfaceProtocol: 0x%x\n", descr->bInterfaceProtocol);
	fprintf(stream, "\tiInterface: %d\n", descr->iInterface);
}


void usb_dumpEndpointDesc(FILE *stream, usb_endpoint_desc_t *descr)
{
	fprintf(stream, "ENDPOINT DESCRIPTOR:\n");
	fprintf(stream, "\tbLength: %d\n", descr->bLength);
	fprintf(stream, "\tbDescriptorType: 0x%x\n", descr->bDescriptorType);
	fprintf(stream, "\tbEndpointAddress: %d\n", descr->bEndpointAddress);
	fprintf(stream, "\tbmAttributes: 0x%x\n", descr->bmAttributes);
	fprintf(stream, "\twMaxPacketSize: %d\n", descr->wMaxPacketSize);
	fprintf(stream, "\tbInterval: %d\n", descr->bInterval);
}


void usb_dumpStringDesc(FILE *stream, usb_string_desc_t *descr)
{
	fprintf(stream, "STRING DESCRIPTOR:\n");
	fprintf(stream, "\tbLength: %d\n", descr->bLength);
	fprintf(stream, "\tbDescriptorType: 0x%x\n", descr->bDescriptorType);
	fprintf(stream, "\twData: %.*s\n", descr->bLength - 2, descr->wData);
}


int usb_modeswitchHandle(usb_driver_t *drv, usb_devinfo_t *dev, const usb_modeswitch_t *mode)
{
	char msg[sizeof(mode->msg)];
	int pipeCtrl, pipeIn, pipeOut, ret;

	pipeCtrl = usb_open(drv, dev, usb_transfer_control, 0);
	if (pipeCtrl < 0) {
		return -EINVAL;
	}

	ret = usb_setConfiguration(drv, pipeCtrl, 1);
	if (ret != 0) {
		return -EINVAL;
	}

	pipeIn = usb_open(drv, dev, usb_transfer_bulk, usb_dir_in);
	if (pipeIn < 0) {
		return -EINVAL;
	}

	pipeOut = usb_open(drv, dev, usb_transfer_bulk, usb_dir_out);
	if (pipeOut < 0) {
		return -EINVAL;
	}

	memcpy(msg, mode->msg, sizeof(msg));

	ret = usb_transferBulk(drv, pipeOut, msg, sizeof(msg), usb_dir_out);
	if (ret < 0) {
		return -EINVAL;
	}

	return 0;
}
