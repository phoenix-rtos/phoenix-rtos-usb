/*
 * Phoenix-RTOS
 *
 * USB host stack
 *
 * Copyright 2021 Phoenix Systems
 * Author: Maciej Purski
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include <stdint.h>

#include <errno.h>
#include <sys/list.h>
#include <sys/msg.h>
#include <sys/platform.h>
#include <sys/types.h>
#include <sys/threads.h>
#include <posix/utils.h>

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <hcd.h>
#include <hub.h>
#include <usbdriver.h>


static struct {
	handle_t common_lock, enumeratorLock, finishedLock;
	handle_t enumeratorCond, finishedCond;
	hcd_t *hcds;
	usb_driver_t *drvs;
	usb_urb_handler_t *finished;
	int nhcd;
	uint32_t port;
	idtree_t pipes;
} usb_common;


void usb_transferFinished(usb_transfer_t *t)
{
	if (t->handler) {
		mutexLock(usb_common.finishedLock);
		LIST_ADD(&usb_common.finished, t->handler);
		mutexUnlock(usb_common.finishedLock);
	}

	if (t->cond)
		condSignal(*t->cond);
}


static void usb_epConf(usb_device_t *dev, usb_endpoint_t *ep, usb_endpoint_desc_t *desc, int interface)
{
	ep->device = dev;
	ep->number = desc->bEndpointAddress & 0xF;
	ep->direction = (desc->bEndpointAddress & 0x80) ? usb_dir_in : usb_dir_out;
	ep->max_packet_len = desc->wMaxPacketSize;
	ep->type = desc->bmAttributes & 0x3;
	ep->interval = desc->bInterval;
	ep->hcdpriv = NULL;
	ep->interface = interface;
	ep->connected = 0;
}

static void usb_dumpDeviceDescriptor(FILE *stream, usb_device_desc_t *descr)
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


static void usb_dumpConfigurationDescriptor(FILE *stream, usb_configuration_desc_t *descr)
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


static void usb_dumpInferfaceDesc(FILE *stream, usb_interface_desc_t *descr)
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


static void usb_dumpEndpointDesc(FILE *stream, usb_endpoint_desc_t *descr)
{
	fprintf(stream, "ENDPOINT DESCRIPTOR:\n");
	fprintf(stream, "\tbLength: %d\n", descr->bLength);
	fprintf(stream, "\tbDescriptorType: 0x%x\n", descr->bDescriptorType);
	fprintf(stream, "\tbEndpointAddress: %d\n", descr->bEndpointAddress);
	fprintf(stream, "\tbmAttributes: 0x%x\n", descr->bmAttributes);
	fprintf(stream, "\twMaxPacketSize: %d\n", descr->wMaxPacketSize);
	fprintf(stream, "\tbInterval: %d\n", descr->bInterval);
}


static void usb_dumpStringDesc(FILE *stream, usb_string_desc_t *descr)
{
	fprintf(stream, "STRING DESCRIPTOR:\n");
	fprintf(stream, "\tbLength: %d\n", descr->bLength);
	fprintf(stream, "\tbDescriptorType: 0x%x\n", descr->bDescriptorType);
	fprintf(stream, "\twData: %.*s\n", descr->bLength - 2,  descr->wData);
}


static int usb_getDescriptor(usb_device_t *dev, int descriptor, int index, char *buffer, int size)
{
	usb_setup_packet_t setup = (usb_setup_packet_t) {
		.bmRequestType = REQUEST_DIR_DEV2HOST | REQUEST_TYPE_STANDARD | REQUEST_RECIPIENT_DEVICE,
		.bRequest = REQ_GET_DESCRIPTOR,
		.wValue = descriptor << 8 | index,
		.wIndex = (descriptor == USB_DESC_STRING) ? dev->langId : 0,
		.wLength = size,
	};
	usb_transfer_t t = (usb_transfer_t) {
		.endpoint = dev->eps,
		.type = usb_transfer_control,
		.direction = usb_dir_in,
		.setup = &setup,
		.buffer = buffer,
		.size = size,
		.cond = &usb_common.enumeratorCond,
		.handler = NULL
	};

	if (dev->hcd->ops->transferEnqueue(dev->hcd, &t) != 0)
		return -1;

	mutexLock(usb_common.enumeratorLock);
	condWait(usb_common.enumeratorCond, usb_common.enumeratorLock, 0);
	mutexUnlock(usb_common.enumeratorLock);

	return 0;
}


static int usb_setAddress(usb_device_t *dev, int address)
{
	usb_setup_packet_t setup = (usb_setup_packet_t) {
		.bmRequestType = REQUEST_DIR_HOST2DEV | REQUEST_TYPE_STANDARD | REQUEST_RECIPIENT_DEVICE,
		.bRequest = REQ_SET_ADDRESS,
		.wValue = address,
		.wIndex = 0,
		.wLength = 0
	};
	usb_transfer_t t = (usb_transfer_t) {
		.endpoint = dev->eps,
		.type = usb_transfer_control,
		.direction = usb_dir_out,
		.setup = &setup,
		.buffer = NULL,
		.size = 0,
		.handler = NULL,
		.cond = &usb_common.enumeratorCond
	};

	if (dev->hcd->ops->transferEnqueue(dev->hcd, &t) != 0)
		return -1;

	mutexLock(usb_common.enumeratorLock);
	condWait(usb_common.enumeratorCond, usb_common.enumeratorLock, 0);
	mutexUnlock(usb_common.enumeratorLock);

	return 0;
}


static int usb_getDeviceDescriptor(usb_device_t *dev)
{
	if (usb_getDescriptor(dev, USB_DESC_DEVICE, 0, (char *)&dev->desc, sizeof(usb_device_desc_t)) != 0) {
		fprintf(stderr, "usb: Fail to get device descriptor\n");
		return -1;
	}

	usb_dumpDeviceDescriptor(stderr, &dev->desc);

	return 0;
}


static int usb_getConfiguration(usb_device_t *dev)
{
	int i, j;
	usb_configuration_desc_t pre, *conf;
	usb_interface_desc_t *iface;
	usb_endpoint_desc_t *epDesc;
	usb_endpoint_t *ep;
	char *ptr;

	/* Get first nine bytes to get to know configuration len */
	if (usb_getDescriptor(dev, USB_DESC_CONFIG, 0, (char *)&pre, sizeof(pre)) != 0) {
		fprintf(stderr, "usb: Fail to get configuration descriptor\n");
		return -1;
	}

	/* TODO: check descriptor correctness */
	if ((conf = malloc(pre.wTotalLength)) == NULL)
		return -ENOMEM;

	/* TODO: Handle multiple configuration devices */
	if (usb_getDescriptor(dev, USB_DESC_CONFIG, 0, (char *)conf, pre.wTotalLength) != 0) {
		fprintf(stderr, "usb: Fail to get configuration descriptor\n");
		return -1;
	}

	dev->nifs = conf->bNumInterfaces;
	if ((dev->ifs = calloc(dev->nifs, sizeof(usb_interface_t))) == NULL)
		return -ENOMEM;

	usb_dumpConfigurationDescriptor(stderr, conf);

	ptr = (char *)conf + sizeof(usb_configuration_desc_t);
	for (i = 0; i < dev->nifs; i++) {
		iface = (usb_interface_desc_t *)ptr;
		usb_dumpInferfaceDesc(stderr, iface);

		ptr += sizeof(usb_interface_desc_t);
		/* Class and Vendor specific descriptors */
		/* TODO: handle them properly */
		while (ptr[1] != USB_DESC_ENDPOINT) {
			ptr += ptr[0];
		}
		for (j = 0; j < iface->bNumEndpoints; j++) {
			ep = malloc(sizeof(usb_endpoint_t));
			epDesc = (usb_endpoint_desc_t *)ptr;
			usb_epConf(dev, ep, epDesc, i);
			LIST_ADD(&dev->eps, ep);
			ptr += sizeof(usb_endpoint_desc_t);
			usb_dumpEndpointDesc(stderr, epDesc);
		}
		dev->ifs[i].desc = iface;
	}

	dev->conf = conf;

	return 0;
}


static int usb_getStringDesc(usb_device_t *dev, char **buf, int index)
{
	usb_string_desc_t desc = { 0 };
	int i;
	size_t asciisz;

	if (usb_getDescriptor(dev, USB_DESC_STRING, index, (char *)&desc, sizeof(desc)) != 0) {
		fprintf(stderr, "usb: Fail to get string descriptor\n");
		return -1;
	}
	asciisz = (desc.bLength - 2) / 2;

	/* Convert from unicode to ascii */
	if ((*buf = calloc(1, asciisz + 1)) == NULL)
		return -ENOMEM;

	for (i = 0; i < asciisz; i++)
		(*buf)[i] = desc.wData[i * 2];

	return 0;
}


static int usb_getAllStringDescs(usb_device_t *dev)
{
	usb_string_desc_t desc = { 0 };
	int i;

	if (usb_getDescriptor(dev, USB_DESC_STRING, 0, (char *)&desc, sizeof(desc)) != 0) {
		fprintf(stderr, "usb: Fail to get configuration descriptor\n");
		return -1;
	}

	if (desc.bLength < 4)
		return -1;

	/* Choose language id */
	dev->langId = desc.wData[0] | ((uint16_t)desc.wData[1] << 8);

	if (dev->desc.iManufacturer != 0) {
		if (usb_getStringDesc(dev, &dev->manufacturer, dev->desc.iManufacturer) != 0)
			return -ENOMEM;
		printf("Manufacturer: %s\n", dev->manufacturer);
	}

	if (dev->desc.iProduct != 0) {
		if (usb_getStringDesc(dev, &dev->product, dev->desc.iProduct) != 0)
			return -ENOMEM;
		printf("Product: %s\n", dev->product);
	}

	if (dev->desc.iSerialNumber != 0) {
		if (usb_getStringDesc(dev, &dev->serialNumber, dev->desc.iSerialNumber) != 0)
			return -ENOMEM;
		printf("Serial Number: %s\n", dev->serialNumber);
	}

	for (i = 0; i < dev->nifs; i++) {
		if (dev->ifs[i].desc->iInterface == 0)
			continue;

		if (usb_getStringDesc(dev, &dev->ifs[i].str, dev->ifs[i].desc->iInterface) != 0)
			return -ENOMEM;
		printf("Interface string: %s\n", dev->ifs[i].str);
	}

	/* TODO: Configuration string descriptors */

	return 0;
}


static int usb_devBind(usb_device_t *dev, int iface, usb_driver_t *drv)
{
	msg_t msg;
	usb_msg_t *umsg = (usb_msg_t *)msg.i.raw;

	fprintf(stderr, "usb: Binding iface: %d to drv: %d\n", iface, drv->pid);

	msg.type = mtDevCtl;
	dev->ifs[iface].driver = drv;
	umsg->type = usb_msg_insertion;
	umsg->insertion.bus = dev->hcd->num;
	umsg->insertion.dev = dev->address;
	umsg->insertion.interface = iface;
	umsg->insertion.descriptor = dev->desc;

	return msgSend(drv->port, &msg);
}


/* TODO: Handle multiple VID:PID pairs per one driver*/
static int usb_drvcmp(usb_device_desc_t *dev, usb_interface_desc_t *iface, usb_device_id_t *filter)
{
	if (filter->vid != USB_CONNECT_WILDCARD && dev->idVendor != filter->vid)
		return 0;

	if (filter->pid != USB_CONNECT_WILDCARD && dev->idProduct != filter->pid)
		return 0;

	if (filter->dclass != USB_CONNECT_WILDCARD) {
		if ((dev->bDeviceClass != 0 && dev->bDeviceClass != filter->dclass) ||
		    (dev->bDeviceClass == 0 && iface->bInterfaceClass != filter->dclass))
			return 0;
	}

	if (filter->subclass != USB_CONNECT_WILDCARD) {
		if ((dev->bDeviceSubClass != 0 && dev->bDeviceSubClass != filter->subclass) ||
		    (dev->bDeviceSubClass == 0 && iface->bInterfaceSubClass != filter->subclass))
			return 0;
	}

	if (filter->protocol != USB_CONNECT_WILDCARD) {
		if ((dev->bDeviceProtocol != 0 && dev->bDeviceProtocol != filter->protocol) ||
		    (dev->bDeviceProtocol == 0 && iface->bInterfaceProtocol!= filter->protocol))
			return 0;
	}

	return 1;
}


static int usb_devMatchDrivers(usb_device_t *dev)
{
	usb_driver_t *drv = usb_common.drvs;
	int i;

	if (drv == NULL) {
		/* TODO: make device orphaned */
		return -1;
	}

	for (i = 0; i < dev->nifs; i++) {
		do {
			/* TODO: better matching */
			if (usb_drvcmp(&dev->desc, dev->ifs[i].desc, &drv->filter)) {
				if (usb_devBind(dev, i, drv) != 0)
					return -1;
				break;
			}
			drv = drv->next;
		} while (drv != usb_common.drvs);
	}

	return 0;
}


static int usb_devsList(char *buffer, size_t size)
{
	return 0;
}


static int usb_portCleanFeatures(usb_device_t *hub, int port, uint32_t change)
{
	int i;

	for (i = 0; i < 5; i++) {
		if (change & (1 << i)) {
			if (hub->hubOps->clearPortFeature(hub, port, USB_PORT_FEAT_C_CONNECTION + i) != 0)
				return -1;
		}
	}

	return 0;
}


static int usb_portReset(usb_device_t *hub, int port, usb_port_status_t *status)
{
	if (hub->hubOps->setPortFeature(hub, port, USB_PORT_FEAT_RESET) != 0)
		return -1;

	if (hub->hubOps->getPortStatus(hub, port, status) != 0)
		return -1;

	if ((status->wPortChange & USB_PORT_FEAT_C_RESET) == 0)
		return -1;

	if (usb_portCleanFeatures(hub, port, status->wPortChange) != 0)
		return -1;

	return 0;
}


static void usb_deviceConnected(usb_device_t *hub, int port)
{
	usb_port_status_t status;
	usb_device_t *dev;
	int addr;

	if (usb_portReset(hub, port, &status) < 0) {
		fprintf(stderr, "usb: fail to reset port %d\n", port);
		return;
	}

	if ((dev = hcd_deviceCreate(hub->hcd)) == NULL) {
		fprintf(stderr, "usb: fail to create device!\n");
		return;
	}

	if (status.wPortStatus & USB_PORT_STAT_HIGH_SPEED)
		dev->speed = usb_high_speed;
	else if (status.wPortStatus & USB_PORT_STAT_LOW_SPEED)
		dev->speed = usb_low_speed;
	else
		dev->speed = usb_full_speed;

	if (usb_getDeviceDescriptor(dev) != 0) {
		fprintf(stderr, "usb: Fail to get device descriptor\n");
		hcd_deviceFree(dev);
		return;
	}

	if (usb_portReset(hub, port, &status) != 0) {
		fprintf(stderr, "usb: Fail to reset port\n");
		hcd_deviceFree(dev);
		return;
	}

	/* First one is always control */
	dev->eps->max_packet_len = dev->desc.bMaxPacketSize0;

	if ((addr = hcd_deviceAdd(hub->hcd, hub, dev, port)) < 0) {
		fprintf(stderr, "usb: Fail to add device to hcd\n");
		hcd_deviceFree(dev);
		return;
	}

	if (usb_setAddress(dev, addr) != 0) {
		fprintf(stderr, "usb: Fail to set device address\n");
		hcd_deviceRemove(hub->hcd, hub, port);
		hcd_deviceFree(dev);
		return;
	}

	dev->address = addr;
	if (usb_getDeviceDescriptor(dev) != 0) {
		fprintf(stderr, "usb: Fail to get device descriptor\n");
		hcd_deviceRemove(hub->hcd, hub, port);
		hcd_deviceFree(dev);
		return;
	}

	if (usb_getConfiguration(dev) != 0) {
		fprintf(stderr, "usb: Fail to get configuration descriptor\n");
		hcd_deviceRemove(hub->hcd, hub, port);
		hcd_deviceFree(dev);
		return;
	}

	if (usb_getAllStringDescs(dev) != 0) {
		fprintf(stderr, "usb: Fail to get string descriptors\n");
		hcd_deviceRemove(hub->hcd, hub, port);
		hcd_deviceFree(dev);
		return;
	}

	if (usb_devMatchDrivers(dev) != 0) {
		fprintf(stderr, "usb: Fail to match drivers for device\n");
		/* TODO: make device orphaned */
		return;
	}
}


static void usb_deviceDisconnected(usb_device_t *hub, int port)
{
	usb_device_t *dev;

	dev = hub->devices[port - 1];
	hcd_deviceRemove(hub->hcd, hub, port);
	hcd_deviceFree(dev);
}


static void usb_portStatusChanged(usb_device_t *hub, int port)
{
	usb_port_status_t status;

	fprintf(stderr, "USB: PORT STATUS CHANGED\n");
	if (hub->hubOps->getPortStatus(hub, port, &status) != 0) {
		fprintf(stderr, "usb: getPortStatus port %d failed!\n", port);
		return;
	}

	if (usb_portCleanFeatures(hub, port, status.wPortChange) != 0) {
		fprintf(stderr, "usb: portCleanFeatures failed on port %d!\n", port);
		return;
	}

	if (status.wPortChange & USB_PORT_STAT_C_CONNECTION) {
		if (status.wPortStatus & USB_PORT_STAT_CONNECTION) {
			if (hub->devices[port - 1] == NULL)
				usb_deviceConnected(hub, port);
			else
				usb_portReset(hub, port, &status);
		} else {
			if (hub->devices[port - 1] != NULL)
				usb_deviceDisconnected(hub, port);
		}
	}
}


static usb_driver_t *usb_drvFind(int pid)
{
	usb_driver_t *drv = usb_common.drvs;

	if (drv != NULL) {
		do {
			if (drv->pid == pid)
				return drv;

			drv = drv->next;
		} while (drv != usb_common.drvs);
	}

	return NULL;
}


static hcd_t *usb_hcdFind(int num)
{
	hcd_t *hcd;

	if (usb_common.hcds != NULL) {
		hcd = usb_common.hcds;
		do {
			if (hcd->num == num)
				return hcd;
			hcd = hcd->next;
		} while (hcd != usb_common.hcds);
	}

	return NULL;
}


static usb_interface_t *usb_ifaceFind(usb_device_t *dev, int num)
{
	if (num >= dev->nifs)
		return NULL;

	return &dev->ifs[num];
}


static usb_pipe_t *usb_pipeFind(int pipe)
{
	return lib_treeof(usb_pipe_t, linkage, idtree_find(&usb_common.pipes, pipe));
}


static int usb_handleUrb(msg_t *msg, unsigned int port, unsigned long rid)
{
	usb_msg_t *umsg = (usb_msg_t *)msg->i.raw;
	hcd_t *hcd;
	usb_pipe_t *pipe;
	usb_transfer_t *t;
	usb_urb_handler_t *h;
	int ret = 0;

	if ((pipe = usb_pipeFind(umsg->urb.pipe)) == NULL) {
		fprintf(stderr, "usb: Fail to find pipe: %d\n", umsg->urb.pipe);
		return -EINVAL;
	}

	if (pipe->drv->pid != msg->pid) {
		fprintf(stderr, "usb: driver pid (%d) and msg pid (%d) mismatch\n", pipe->drv->pid, msg->pid);
		return -EINVAL;
	}


	if ((t = calloc(1, sizeof(usb_transfer_t))) == NULL)
		return -ENOMEM;

	if ((h = malloc(sizeof(usb_urb_handler_t))) == NULL) {
		free(t);
		return -ENOMEM;
	}

	t->type = pipe->ep->type;
	t->direction = umsg->urb.dir;
	t->endpoint = pipe->ep;
	t->handler = h;
	t->setup = &umsg->urb.setup;
	t->size = umsg->urb.size;
	t->buffer = msg->i.data;
	t->cond = &usb_common.finishedCond;
	t->transfered = 0;

	h->msg = *msg;
	h->port = port;
	h->rid = rid;
	h->transfer = t;

	hcd = pipe->ep->device->hcd;
	if ((ret = hcd->ops->transferEnqueue(hcd, t)) != 0) {
		free(t);
		free(h);
	}

	return ret;
}


static int usb_handleConnect(usb_connect_t *c, unsigned pid)
{
	usb_driver_t *drv;

	if ((drv = malloc(sizeof(usb_driver_t))) == NULL)
		return -ENOMEM;

	drv->pid = pid;
	drv->filter = c->filter;
	drv->port = c->port;

	LIST_ADD(&usb_common.drvs, drv);

	/* TODO: handle orphaned devices */

	return 0;
}


static int usb_handleOpen(usb_open_t *o, msg_t *msg)
{
	hcd_t *hcd;
	usb_device_t *dev;
	usb_interface_t *iface;
	usb_driver_t *drv;
	usb_pipe_t *pipe = NULL;
	usb_endpoint_t *ep;


	if ((drv = usb_drvFind(msg->pid)) == NULL) {
		fprintf(stderr, "usb: Fail to find driver pid: %d\n", msg->pid);
		return -EINVAL;
	}

	if ((hcd = usb_hcdFind(o->bus)) == NULL) {
		fprintf(stderr, "usb: Fail to find hcd: %d\n", o->bus);
		return -EINVAL;
	}

	if ((dev = hcd_deviceFind(hcd, o->dev)) == NULL) {
		fprintf(stderr, "usb: Fail to find dev: %d\n", o->dev);
		return -EINVAL;
	}

	if ((iface = usb_ifaceFind(dev, o->iface)) == NULL) {
		fprintf(stderr, "usb: Fail to find iface: %d\n", o->iface);
		return -EINVAL;
	}

	if (iface->driver != drv) {
		fprintf(stderr, "usb: Interface and driver mismatch\n");
		return -EINVAL;
	}

	ep = dev->eps;
	do {
		if (ep->direction == o->dir && ep->type == o->type && !ep->connected) {
			if ((pipe = malloc(sizeof(usb_pipe_t))) == NULL)
				return -ENOMEM;
			idtree_alloc(&usb_common.pipes, &pipe->linkage);
			fprintf(stderr, "usb: Opening pipe id %d ep_num: %d ep_dir: %d ep_type: %d\n", pipe->linkage.id, ep->number, ep->direction, ep->type);
			break;
		}
		ep = ep->next;
	} while (ep != dev->eps);

	if (pipe == NULL) {
		fprintf(stderr, "usb: Fail to open pipe\n");
		return -EINVAL;
	}


	/* The control endpoint can be used by multiple drivers */
	if (ep->number != 0)
		ep->connected = 1;

	pipe->ep = ep;
	pipe->drv = drv;
	*(int *)msg->o.raw = pipe->linkage.id;

	return 0;
}


static void usb_enumerator(void *arg)
{
	hcd_t *hcd;
	usb_device_t *hub;
	uint32_t portmask = 0;
	int n;

	for (;;) {
		sleep(1);
		hcd = usb_common.hcds;
		hub = hcd->roothub;
		do {
			hub->hubOps->statusChanged(hub, &portmask);
			if (portmask > 1) {
				/* Port status changed */
				for (n = 1; n <= hub->ndevices; n++) {
					if (portmask & (1 << n))
						usb_portStatusChanged(hub, n);
				}
			}
			else if (portmask == 1) {
				/* Hub status changed */
			}
			hcd = hcd->next;
		} while (hcd != usb_common.hcds);
	}
}


static void usb_statusthr(void *arg)
{
	usb_urb_handler_t *h;

	for (;;) {
		mutexLock(usb_common.finishedLock);
		while (usb_common.finished == NULL)
			condWait(usb_common.finishedCond, usb_common.finishedLock, 0);
		h = usb_common.finished;
		LIST_REMOVE(&usb_common.finished, h);
		mutexUnlock(usb_common.finishedLock);

		if (h->transfer->type == usb_transfer_bulk || h->transfer->type == usb_transfer_control) {
			h->msg.o.io.err = (h->transfer->error != 0) ? -h->transfer->error : h->transfer->transfered;
			if (msgRespond(h->port, &h->msg, h->rid) != 0) {
				/* Driver is dead? TODO: remove driver */
			}
			free(h->transfer);
			free(h);
		}
	}
}

static void usb_msgthr(void *arg)
{
	unsigned port = (int)arg;
	unsigned long rid;
	msg_t msg;
	usb_msg_t *umsg;
	int resp = 1;
	int ret;

	for (;;) {
		if (msgRecv(port, &msg, &rid) < 0)
			continue;
		mutexLock(usb_common.common_lock);
		switch (msg.type) {
			case mtRead:
				msg.o.io.err = usb_devsList(msg.o.data, msg.o.size);
				break;
			case mtDevCtl:
				umsg = (usb_msg_t *)msg.i.raw;
				switch (umsg->type) {
					case usb_msg_connect:
						msg.o.io.err = usb_handleConnect(&umsg->connect, msg.pid);
						resp = 1;
						break;
					case usb_msg_open:
						if (usb_handleOpen(&umsg->open, &msg) != 0)
							msg.o.io.err = -1;
						resp = 1;
						break;
					case usb_msg_urb:
						if ((ret = usb_handleUrb(&msg, port, rid)) != 0) {
							msg.o.io.err = ret;
							resp = 1;
						} else {
							resp = 0;
						}
						break;
					default:
						fprintf(stderr, "unsupported usb_msg type\n");
						break;
				}
				break;
			default:
				fprintf(stderr, "usb: unsupported msg type\n");
				msg.o.io.err = -EINVAL;
		}
		mutexUnlock(usb_common.common_lock);

		if (resp)
			msgRespond(port, &msg, rid);
	}
}


int main(int argc, char *argv[])
{
	oid_t oid;

	if (mutexCreate(&usb_common.common_lock) != 0) {
		fprintf(stderr, "usb: Can't create mutex!\n");
		return -EINVAL;
	}

	if (mutexCreate(&usb_common.enumeratorLock) != 0) {
		fprintf(stderr, "usb: Can't create mutex!\n");
		return -EINVAL;
	}

	if (mutexCreate(&usb_common.finishedLock) != 0) {
		fprintf(stderr, "usb: Can't create mutex!\n");
		return -EINVAL;
	}

	if (condCreate(&usb_common.enumeratorCond) != 0) {
		fprintf(stderr, "usb: Can't create mutex!\n");
		return -EINVAL;
	}

	if (condCreate(&usb_common.finishedCond) != 0) {
		fprintf(stderr, "usb: Can't create mutex!\n");
		return -EINVAL;
	}


	if (portCreate(&usb_common.port) != 0) {
		fprintf(stderr, "usb: Can't create port!\n");
		return -EINVAL;
	}

	idtree_init(&usb_common.pipes);

	oid.port = usb_common.port;
	oid.id = 0;

	if (create_dev(&oid, "/dev/usb") != 0) {
		fprintf(stderr, "usb: Can't create dev!\n");
		return -EINVAL;
	}

	if ((usb_common.hcds = hcd_init()) == NULL) {
		fprintf(stderr, "usb: Fail to init hcds!\n");
		return -EINVAL;
	}

	beginthread(usb_enumerator, 4, malloc(0x1000), 0x1000, NULL);
	beginthread(usb_statusthr, 4, malloc(0x1000), 0x1000, NULL);

	usb_msgthr((void *)usb_common.port);

	return 0;
}