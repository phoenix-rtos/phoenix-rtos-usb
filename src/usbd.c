/*
 * Phoenix-RTOS
 *
 * USB Host driver
 *
 * Copyright 2018 Phoenix Systems
 * Author: Jan Sikorski
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include <sys/mman.h>
#include <sys/interrupt.h>
#include <sys/threads.h>
#include <sys/platform.h>
#include <sys/list.h>
#include <sys/rb.h>
#include <sys/msg.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <posix/idtree.h>
#include <posix/utils.h>

#include <dma.h>
#include <ehci.h>
#include "usb.h"
#include "usbd.h"


#define FUN_TRACE  //usleep(10000) //*/fprintf(stderr, "usbd trace: %s\n", __PRETTY_FUNCTION__)
#define TRACE(x, ...) //usleep(10000) //*/  fprintf(stderr, "usbd: " x "\n", ##__VA_ARGS__)


typedef struct {
	rbnode_t linkage;
	unsigned pid;
	unsigned port;
	usb_device_id_t filter;
	struct usb_device *devices;
} usb_driver_t;


typedef struct usb_endpoint {
	struct usb_endpoint *next, *prev;
	idnode_t linkage;

	struct usb_device *device;

	int max_packet_len;
	int number;

	struct qh *qh;
} usb_endpoint_t;


typedef struct usb_device {
	struct usb_device *next, *prev;
	idnode_t linkage;

	usb_driver_t *driver;
	usb_endpoint_t *endpoints;
	usb_endpoint_t *control_endpoint;

	device_desc_t *descriptor;
	char address;
	idtree_t pipes;
	int speed;
} usb_device_t;


typedef struct usb_qtd_list {
	struct usb_qtd_list *next, *prev;
	struct qtd *qtd;
	size_t size;
} usb_qtd_list_t;


typedef struct usb_transfer {
	struct usb_transfer *next, *prev;
	struct usb_transfer *finished_next, *finished_prev;
	struct usb_endpoint *endpoint;

	unsigned async;
	unsigned id;
	handle_t cond;
	volatile int finished;

	void *transfer_buffer;
	size_t transfer_size;
	int transfer_type;
	int direction;
	setup_packet_t *setup;

	usb_qtd_list_t *qtds;
} usb_transfer_t;


static struct {
	usb_transfer_t *active_transfers;
	usb_transfer_t *finished_transfers;
	usb_device_t *orphan_devices;

	rbtree_t drivers;
	idtree_t devices;
	unsigned port;

	handle_t common_lock;
	handle_t async_cond, port_cond;
} usbd_common;


usb_qtd_list_t *usb_allocQtd(int token, char *buffer, size_t *size, int datax)
{
	FUN_TRACE;

	usb_qtd_list_t *element = malloc(sizeof(usb_qtd_list_t));

	if ((element->qtd = ehci_allocQtd(token, buffer, size, datax)) == NULL)
		return NULL;

	element->size = element->qtd->bytes_to_transfer;

	return element;
}


void usb_addQtd(usb_transfer_t *transfer, int token, void *buffer, size_t *size, int datax)
{
	FUN_TRACE;

	usb_qtd_list_t *el = usb_allocQtd(token, buffer, size, datax);

	TRACE("alloced %p qtd", el->qtd);
	LIST_ADD(&transfer->qtds, el);
}


usb_transfer_t *usb_allocTransfer(usb_endpoint_t *endpoint, int direction, int transfer_type, void *buffer, size_t size, int async)
{
	FUN_TRACE;

	usb_transfer_t *result;

	result = malloc(sizeof(usb_transfer_t));

	result->next = result->prev = NULL;
	result->finished_next = result->finished_prev = NULL;
	result->endpoint = endpoint;
	result->async = async;
	result->id = (unsigned)result;
	result->transfer_type = transfer_type;
	result->direction = direction;
	result->finished = 0;

	if (async)
		result->cond = usbd_common.async_cond;
	else
		condCreate(&result->cond);

	result->transfer_buffer = buffer;
	result->transfer_size = size;
	result->setup = NULL;
	result->qtds = NULL;
	return result;
}


void usb_deleteTransfer(usb_transfer_t *transfer)
{
	FUN_TRACE;

	usb_qtd_list_t *element, *next;

	if (transfer->setup != NULL)
		dma_free64(transfer->setup);

	ehci_dequeue(transfer->endpoint->qh, transfer->qtds->qtd, transfer->qtds->prev->qtd);

	element = transfer->qtds;
	do {
		next = element->next;
		ehci_freeQtd(element->qtd);
		free(element);
	}
	while (next != transfer->qtds);

	if (!transfer->async)
		resourceDestroy(transfer->cond);

	free(transfer);
}


void usb_linkTransfer(usb_endpoint_t *endpoint, usb_transfer_t *transfer)
{
	FUN_TRACE;

	usb_qtd_list_t *qtd = transfer->qtds;
	int address, speed;

	address = endpoint->device == NULL ? 0 : endpoint->device->address;
	speed = endpoint->device == NULL ? full_speed : endpoint->device->speed;

	do {
		TRACE("linking : %p to %p", qtd->qtd, qtd->next->qtd);
		ehci_linkQtd(qtd->qtd, qtd->next->qtd);
		qtd = qtd->next;
	} while (qtd != transfer->qtds);

	if (endpoint->qh == NULL) {
		endpoint->qh = ehci_allocQh(address, endpoint->number, transfer->transfer_type, speed, endpoint->max_packet_len);
		ehci_linkQh(endpoint->qh);
	}

	TRACE("first: %p last: %p", transfer->qtds->qtd, transfer->qtds->prev->qtd);
	LIST_ADD(&usbd_common.active_transfers, transfer);
	ehci_enqueue(endpoint->qh, transfer->qtds->qtd, transfer->qtds->prev->qtd);
}


int usb_finished(usb_transfer_t *transfer)
{
	usb_qtd_list_t *qtd = transfer->qtds;
	int finished = 0;

	do {
		finished |= ehci_qtdFinished(qtd->qtd);
		qtd = qtd->next;
	} while (qtd != transfer->qtds);

	return finished;
}


int usb_handleUrb(usb_urb_t *urb, usb_driver_t *driver, usb_device_t *device, usb_endpoint_t *endpoint, void *buffer)
{
	FUN_TRACE;

//	asm volatile ("1: b 1b;");

	usb_transfer_t *transfer;
	size_t remaining_size;
	int datax = 1;
	char *transfer_buffer;
	int data_token = urb->direction == usb_transfer_out ? out_token : in_token;
	int control_token = data_token == out_token ? in_token : out_token;


	transfer = usb_allocTransfer(endpoint, urb->direction, urb->type /* FIXME: should explicitly use enum from ehci.h */, buffer, urb->transfer_size, urb->async);

	if (urb->type == usb_transfer_control) {
		transfer->setup = dma_alloc64();
		*transfer->setup = urb->setup;
		remaining_size = sizeof(setup_packet_t);
		usb_addQtd(transfer, setup_token, transfer->setup, &remaining_size, 0);
	}

	remaining_size = transfer->transfer_size;

	while (remaining_size) {
		transfer_buffer = (char *)transfer->transfer_buffer + transfer->transfer_size - remaining_size;
		usb_addQtd(transfer, data_token, transfer_buffer, &remaining_size, datax);
		datax = !datax;
	}

	if (urb->type == usb_transfer_control) {
		usb_addQtd(transfer, control_token, NULL, NULL, 1);
	}

	usb_linkTransfer(endpoint, transfer);

	/* TODO; dont wait here */
	if (!transfer->async) {
		while (!transfer->finished) {
			TRACE("waiting");
			int err = condWait(transfer->cond, usbd_common.common_lock, 1000000);
			TRACE("woke up %s", err < 0 ? "timeout" : "naturally");
		}

		LIST_REMOVE(&usbd_common.active_transfers, transfer);
		//usb_deleteTransfer(transfer);
		return EOK;
	}
	else {
		return transfer->id;
	}
}


int usb_submitUrb(int pid, usb_urb_t *urb, void *inbuf, void *outbuf)
{
	FUN_TRACE;

	usb_driver_t find, *driver;
	usb_device_t *device;
	usb_endpoint_t *endpoint;

	void *buffer = NULL;

	find.pid = pid;

	if ((driver = lib_treeof(usb_driver_t, linkage, lib_rbFind(&usbd_common.drivers, &find.linkage))) == NULL) {
		TRACE("no driver");
		return -EINVAL;
	}

	if ((device = lib_treeof(usb_device_t, linkage, idtree_find(&usbd_common.devices, urb->device_id))) == NULL) {
		TRACE("no device");
		return -EINVAL;
	}

	if ((endpoint = lib_treeof(usb_endpoint_t, linkage, idtree_find(&device->pipes, urb->pipe))) == NULL) {
		TRACE("no endpoint");
		return -EINVAL;
	}

	if (urb->transfer_size) {
		buffer = mmap(NULL, (urb->transfer_size + SIZE_PAGE - 1) & ~(SIZE_PAGE - 1), PROT_WRITE | PROT_READ, MAP_ANONYMOUS | MAP_UNCACHED, OID_NULL, 0);
		if (inbuf != NULL)
			memcpy(buffer, inbuf, urb->transfer_size);
	}

	if (buffer == MAP_FAILED) {
		TRACE("no mem");
		return -ENOMEM;
	}

	int err = usb_handleUrb(urb, driver, device, endpoint, buffer);

	if (outbuf != NULL && urb->direction == usb_transfer_out)
		memcpy(outbuf, buffer, urb->transfer_size);

	return err;
}


void usb_eventCallback(int port_change)
{
	FUN_TRACE;
	usb_transfer_t *transfer;

	if ((transfer = usbd_common.active_transfers) != NULL) {
		do {

			if (transfer == NULL) {
				printf("should not happen\n");
				break;
			}

			if (usb_finished(transfer)) {
				TRACE("transfer finished %x", transfer->id);
				transfer->finished = 1;

				if (transfer->async)
					LIST_ADD_EX(&usbd_common.finished_transfers, transfer, finished_next, finished_prev);

				condBroadcast(transfer->cond);
			}
			transfer = transfer->next;
		}
		while (transfer != usbd_common.active_transfers);
	}

	if (port_change) {
		TRACE("port change");
		condSignal(usbd_common.port_cond);
	}

	TRACE("callback out");
}




int usb_countBytes(usb_transfer_t *transfer)
{
	size_t transferred_bytes = 0;
	usb_qtd_list_t *qtd;

	qtd = transfer->qtds;

	do {
		transferred_bytes += qtd->size - qtd->qtd->bytes_to_transfer;
		qtd = qtd->next;
	} while (qtd != transfer->qtds);

	return transferred_bytes;
}


void usb_signalDriver(usb_transfer_t *transfer)
{
	FUN_TRACE;

	usb_driver_t *driver;
	usb_event_t *event;
	msg_t msg = { 0 };

	if ((driver = transfer->endpoint->device->driver) == NULL)
		return;

	msg.type = mtDevCtl;

	event = (void *)msg.i.raw;
	event->type = usb_event_completion;
	event->completion.transfer_id = transfer->id;

	if (transfer->direction == usb_transfer_in) {
		msg.i.size = usb_countBytes(transfer);
		msg.i.data = transfer->transfer_buffer;
	}

	msgSend(transfer->endpoint->device->driver->port, &msg);
}


void usb_signalThread(void *arg)
{
	usb_transfer_t *transfer;

	mutexLock(usbd_common.common_lock);

	for (;;) {
		while ((transfer = usbd_common.finished_transfers) == NULL)
			condWait(usbd_common.async_cond, usbd_common.common_lock, 0);

		LIST_REMOVE(&usbd_common.active_transfers, transfer);
		LIST_REMOVE_EX(&usbd_common.finished_transfers, transfer, finished_next, finished_prev);
		usb_signalDriver(transfer);

		munmap(transfer->transfer_buffer, (transfer->transfer_size + SIZE_PAGE - 1) & ~(SIZE_PAGE - 1));
		usb_deleteTransfer(transfer);
	}
}








int usb_control(usb_device_t *device, int direction, setup_packet_t *setup, void *buffer, int size) {

	usb_urb_t urb = (usb_urb_t) {
		.type = usb_transfer_control,
		.direction = direction,
		.device_id = idtree_id(&device->linkage),
		.pipe = 0,
		.transfer_size = size,
		.async = 0,
		.setup = *setup,
	};

	return usb_handleUrb(&urb, device->driver, device, device->control_endpoint, buffer);
}


int usb_setAddress(usb_device_t *dev, unsigned char address)
{
	FUN_TRACE;

	setup_packet_t setup = (setup_packet_t) {
		.bmRequestType = REQUEST_DIR_HOST2DEV | REQUEST_TYPE_STANDARD | REQUEST_RECIPIENT_DEVICE,
		.bRequest = SET_ADDRESS,
		.wValue = address,
		.wIndex = 0,
		.wLength = 0,
	};

	return usb_control(dev, usb_transfer_out, &setup, NULL, 0);
}


int usb_getDescriptor(usb_device_t *dev, int descriptor, int index, char *buffer, int size)
{
	FUN_TRACE;

	setup_packet_t setup = (setup_packet_t) {
		.bmRequestType = REQUEST_DIR_DEV2HOST | REQUEST_TYPE_STANDARD | REQUEST_RECIPIENT_DEVICE,
		.bRequest = GET_DESCRIPTOR,
		.wValue = descriptor << 8 | index,
		.wIndex = 0,
		.wLength = size,
	};

	return usb_control(dev, usb_transfer_in, &setup, buffer, size);
}


int usb_getConfigurationDescriptor(usb_device_t *dev, configuration_desc_t *desc, int index, int length)
{
	return usb_getDescriptor(dev, DESC_CONFIG, index, (char *)desc, length);
}


int usb_getDeviceDescriptor(usb_device_t *dev, device_desc_t *desc)
{
	return usb_getDescriptor(dev, DESC_DEVICE, 0, (char *)desc, sizeof(*desc));
}


int usb_driverMatch1(usb_driver_t *driver, usb_device_t *device)
{
	usb_device_id_t *filter = &driver->filter;
	device_desc_t *descriptor = device->descriptor;

	return (filter->idVendor == USB_CONNECT_WILDCARD || filter->idVendor == descriptor->idVendor) &&
		(filter->idProduct == USB_CONNECT_WILDCARD || filter->idProduct == descriptor->idProduct) &&
		(filter->bcdDevice == USB_CONNECT_WILDCARD || filter->bcdDevice == descriptor->bcdDevice);
}


int usb_driverMatch2(usb_driver_t *driver, usb_device_t *device)
{
	usb_device_id_t *filter = &driver->filter;
	device_desc_t *descriptor = device->descriptor;

	return (filter->idVendor == USB_CONNECT_WILDCARD || filter->idVendor == descriptor->idVendor) &&
		(filter->idProduct == USB_CONNECT_WILDCARD || filter->idProduct == descriptor->idProduct);
}


int usb_driverMatch3(usb_driver_t *driver, usb_device_t *device)
{
	usb_device_id_t *filter = &driver->filter;
	device_desc_t *descriptor = device->descriptor;

	if (descriptor->bDeviceClass == 0xff) {
		return (filter->idVendor == USB_CONNECT_WILDCARD || filter->idVendor == descriptor->idVendor) &&
			(filter->bDeviceSubClass == USB_CONNECT_WILDCARD || filter->bDeviceSubClass == descriptor->bDeviceSubClass) &&
			(filter->bDeviceProtocol == USB_CONNECT_WILDCARD || filter->bDeviceProtocol == descriptor->bDeviceProtocol);
	}
	else {
		return (filter->bDeviceClass == USB_CONNECT_WILDCARD || filter->bDeviceClass == descriptor->bDeviceClass) &&
			(filter->bDeviceSubClass == USB_CONNECT_WILDCARD || filter->bDeviceSubClass == descriptor->bDeviceSubClass) &&
			(filter->bDeviceProtocol == USB_CONNECT_WILDCARD || filter->bDeviceProtocol == descriptor->bDeviceProtocol);
	}
}


int usb_driverMatch4(usb_driver_t *driver, usb_device_t *device)
{
	usb_device_id_t *filter = &driver->filter;
	device_desc_t *descriptor = device->descriptor;

	if (descriptor->bDeviceClass == 0xff) {
		return (filter->idVendor == USB_CONNECT_WILDCARD || filter->idVendor == descriptor->idVendor) &&
			(filter->bDeviceSubClass == USB_CONNECT_WILDCARD || filter->bDeviceSubClass == descriptor->bDeviceSubClass);
	}
	else {
		return (filter->bDeviceClass == USB_CONNECT_WILDCARD || filter->bDeviceClass == descriptor->bDeviceClass) &&
			(filter->bDeviceSubClass == USB_CONNECT_WILDCARD || filter->bDeviceSubClass == descriptor->bDeviceSubClass);
	}
}


usb_driver_t *usb_findDriver(usb_device_t *device)
{
	FUN_TRACE;

	const int (*usb_driverMatch[])(usb_driver_t *, usb_device_t *) = {
		usb_driverMatch1, usb_driverMatch2, usb_driverMatch3, usb_driverMatch4
	};

	rbnode_t *node;
	usb_driver_t *driver;
	int i;

	for (i = 0; i < 4; ++i) {
		for (node = lib_rbMinimum(usbd_common.drivers.root); node != NULL; node = lib_rbNext(node)) {
			driver = lib_treeof(usb_driver_t, linkage, node);
			if (usb_driverMatch[i](driver, device)) {
				TRACE("found driver");
				return driver;
			}
		}
	}
	return NULL;
}


int usb_connectDriver(usb_driver_t *driver, usb_device_t *device, configuration_desc_t *configuration)
{
	FUN_TRACE;

	msg_t msg;
	usb_event_t *event = (void *)msg.i.raw;
	usb_insertion_t *insertion = &event->insertion;

	memset(&msg, 0, sizeof(msg));
	msg.type = mtDevCtl;
	msg.i.data = configuration;
	msg.i.size = configuration->wTotalLength;

	event->type = usb_event_insertion;
	insertion->device_id = idtree_id(&device->linkage);
	memcpy(&insertion->descriptor, device->descriptor, sizeof(device_desc_t));

	return msgSend(driver->port, &msg);
}


usb_endpoint_t *usb_findPipe(usb_device_t *device, int pipe)
{
	return lib_treeof(usb_endpoint_t, linkage, idtree_find(&device->pipes, pipe));
}


int usb_openPipe(usb_device_t *device, endpoint_desc_t *descriptor)
{
	FUN_TRACE;

	usb_endpoint_t *pipe = malloc(sizeof(usb_endpoint_t));

	pipe->max_packet_len = 64; //*/descriptor->wMaxPacketSize;
	pipe->number = descriptor->bEndpointAddress & 0xf;
	pipe->next = pipe->prev = NULL;
	pipe->device = device;
	return idtree_alloc(&device->pipes, &pipe->linkage);
}


int usb_getConfiguration(usb_device_t *device, void *buffer, size_t bufsz)
{
	FUN_TRACE;

	configuration_desc_t *conf = dma_alloc64();

	usb_getConfigurationDescriptor(device, conf, 0, sizeof(configuration_desc_t));

	if (bufsz < conf->wTotalLength)
		return -ENOBUFS;

	usb_getConfigurationDescriptor(device, buffer, 0, conf->wTotalLength);

	dma_free64(conf);
	return EOK;
}


void usb_dumpDeviceDescriptor(FILE *stream, device_desc_t *descr)
{
	fprintf(stream, "DEVICE DESCRIPTOR:\n");
	fprintf(stream, "\tbLength: %d\n", descr->bLength);
	fprintf(stream, "\tbDescriptorType: %d\n", descr->bDescriptorType);
	fprintf(stream, "\tbcdUSB: %d\n", descr->bcdUSB);
	fprintf(stream, "\tbDeviceClass: %d\n", descr->bDeviceClass);
	fprintf(stream, "\tbDeviceSubClass: %d\n", descr->bDeviceSubClass);
	fprintf(stream, "\tbDeviceProtocol: %d\n", descr->bDeviceProtocol);
	fprintf(stream, "\tbMaxPacketSize0: %d\n", descr->bMaxPacketSize0);
	fprintf(stream, "\tidVendor: %d\n", descr->idVendor);
	fprintf(stream, "\tidProduct: %d\n", descr->idProduct);
	fprintf(stream, "\tbcdDevice: %d\n", descr->bcdDevice);
	fprintf(stream, "\tiManufacturer: %d\n", descr->iManufacturer);
	fprintf(stream, "\tiProduct: %d\n", descr->iProduct);
	fprintf(stream, "\tiSerialNumber: %d\n", descr->iSerialNumber);
	fprintf(stream, "\tbNumConfigurations: %d\n", descr->bNumConfigurations);
}


void usb_deviceAttach(void)
{
	FUN_TRACE;

	usb_device_t *dev;
	usb_endpoint_t *ep;
	usb_driver_t *driver;
	void *configuration;
	device_desc_t *ddesc = dma_alloc64();

	TRACE("reset");
	ehci_resetPort();

	dev = calloc(1, sizeof(usb_device_t));
	ep = calloc(1, sizeof(usb_endpoint_t));

	dev->control_endpoint = ep;
	dev->speed = full_speed;
	ep->number = 0;
	ep->max_packet_len = 64;
	ep->device = dev;

	idtree_init(&dev->pipes);
	idtree_alloc(&dev->pipes, &ep->linkage);

	TRACE("getting device descriptor");
	usb_getDeviceDescriptor(dev, ddesc);
	ehci_resetPort();

	dev->descriptor = ddesc;
	ep->max_packet_len = ddesc->bMaxPacketSize0;

	usb_dumpDeviceDescriptor(stderr, ddesc);

	TRACE("setting address");
	usb_setAddress(dev, 1);
	dev->address = 1;

	dev->control_endpoint->qh->device_addr = 1; /* fixme. */

	idtree_alloc(&usbd_common.devices, &dev->linkage);

	if ((driver = usb_findDriver(dev)) != NULL) {
		TRACE("got driver");
		configuration = mmap(NULL, SIZE_PAGE, PROT_WRITE | PROT_READ, MAP_ANONYMOUS | MAP_UNCACHED, OID_NULL, 0);
		usb_getConfiguration(dev, configuration, SIZE_PAGE);
		if (usb_connectDriver(driver, dev, configuration) < 0) {
			LIST_ADD(&usbd_common.orphan_devices, dev);
			dev->driver = NULL;
		}
		else {
			LIST_ADD(&driver->devices, dev);
			dev->driver = driver;
		}
		munmap(configuration, SIZE_PAGE);
	}
	else {
		TRACE("no driver");
		LIST_ADD(&usbd_common.orphan_devices, dev);
	}
}


void usb_deviceDetach(void)
{
	FUN_TRACE;
	usb_device_t *device = lib_treeof(usb_device_t, linkage, usbd_common.devices.root);

	if (device != NULL) {
		idtree_remove(&usbd_common.devices, &device->linkage);

		/* TODO: remove active transfers */

		if (device->driver != NULL) {
			LIST_REMOVE(&device->driver->devices, device);
			device->driver = NULL;
		}
		else {
			LIST_REMOVE(&usbd_common.orphan_devices, device);
		}

		free(device);
	}
}


void usb_portthr(void *arg)
{
	int attached = 0;

	mutexLock(usbd_common.common_lock);

	for (;;) {
		condWait(usbd_common.port_cond, usbd_common.common_lock, 0);
		FUN_TRACE;

		if (ehci_deviceAttached()) {
			if (attached) {
				TRACE("double attach");
			}
			else {
				usb_deviceAttach();
				attached = 1;
			}
		}
		else {
			if (!attached) {
				TRACE("double detach");
			}
			else {
				usb_deviceDetach();
				attached = 0;
			}
		}
	}
}



int usb_connect(usb_connect_t *c, unsigned pid)
{
	FUN_TRACE;

	const int (*usb_driverMatch[])(usb_driver_t *, usb_device_t *) = {
		usb_driverMatch1, usb_driverMatch2, usb_driverMatch3, usb_driverMatch4
	};

	int i;
	usb_driver_t *driver = malloc(sizeof(*driver));
	usb_device_t *device;
	void *configuration;

	if (driver == NULL)
		return -ENOMEM;

	driver->port = c->port;
	driver->filter = c->filter;
	driver->pid = pid;
	driver->devices = NULL;

	lib_rbInsert(&usbd_common.drivers, &driver->linkage);

	if (usbd_common.orphan_devices != NULL) {
		configuration = mmap(NULL, SIZE_PAGE, PROT_WRITE | PROT_READ, MAP_ANONYMOUS | MAP_UNCACHED, OID_NULL, 0);

		if (configuration == MAP_FAILED)
			return -ENOMEM;

		for (i = 0; i < 4; ++i) {
			device = usbd_common.orphan_devices;

			do {
				while (device != NULL && usb_driverMatch[i](driver, device)) {
					usb_getConfiguration(device, configuration, SIZE_PAGE);
					usb_connectDriver(driver, device, configuration);
					device->driver = driver;

					if (device == usbd_common.orphan_devices) {
						LIST_REMOVE(&usbd_common.orphan_devices, device);
						device = usbd_common.orphan_devices;
					}
					else {
						LIST_REMOVE(&device, device);
					}
				}
			}
			while (device != NULL && (device = device->next) != usbd_common.orphan_devices);
		}

		munmap(configuration, SIZE_PAGE);
	}

	return EOK;
}



int usb_open(usb_open_t *o, msg_t *msg)
{
	FUN_TRACE;
	usb_device_t *device;

	if ((device = lib_treeof(usb_device_t, linkage, idtree_find(&usbd_common.devices, o->device_id))) == NULL)
		return -EINVAL;

	return usb_openPipe(device, &o->endpoint);
}


void msgthr(void *arg)
{
	unsigned port = (int)arg;
	unsigned rid;
	msg_t msg;
	usb_msg_t *umsg;


	for (;;) {
		TRACE("receiving");
		if (msgRecv(port, &msg, &rid) < 0)
			continue;

		TRACE("received");
		mutexLock(usbd_common.common_lock);
		TRACE("got lock");

		if (msg.type == mtDevCtl) {
			umsg = (void *)msg.i.raw;

			switch (umsg->type) {
			case usb_msg_connect:
				msg.o.io.err = usb_connect(&umsg->connect, msg.pid);
				break;
			case usb_msg_urb:
				msg.o.io.err = usb_submitUrb(msg.pid, &umsg->urb, msg.i.data, msg.o.data);
				break;
			case usb_msg_open:
				msg.o.io.err = usb_open(&umsg->open, &msg);
				break;
			}
		}
		else {
			msg.o.io.err = -EINVAL;
		}
		mutexUnlock(usbd_common.common_lock);

		msgRespond(port, &msg, rid);
	}
}


int usb_driver_cmp(rbnode_t *n1, rbnode_t *n2)
{
	usb_driver_t *d1 = lib_treeof(usb_driver_t, linkage, n1);
	usb_driver_t *d2 = lib_treeof(usb_driver_t, linkage, n2);

	if (d1->pid > d2->pid)
		return 1;
	else if (d1->pid < d2->pid)
		return -1;

	return 0;
}


int main(int argc, char **argv)
{
	//asm volatile ("1: b 1b");

	FUN_TRACE;
	oid_t oid;
	portCreate(&usbd_common.port);

	mutexCreate(&usbd_common.common_lock);
	condCreate(&usbd_common.port_cond);
	condCreate(&usbd_common.async_cond);

	usbd_common.active_transfers = NULL;
	usbd_common.finished_transfers = NULL;
	usbd_common.orphan_devices = NULL;
	lib_rbInit(&usbd_common.drivers, usb_driver_cmp, NULL);
	idtree_init(&usbd_common.devices);

	ehci_init(usb_eventCallback, usbd_common.common_lock);

	oid.port = usbd_common.port;
	oid.id = 0;
	create_dev(&oid, "/dev/usb");

	beginthread(usb_portthr, 4, malloc(0x4000), 0x4000, NULL);
	beginthread(usb_signalThread, 4, malloc(0x4000), 0x4000, NULL);

	beginthread(msgthr, 4, malloc(0x4000), 0x4000, (void *)usbd_common.port);
	beginthread(msgthr, 4, malloc(0x4000), 0x4000, (void *)usbd_common.port);
	msgthr((void *)usbd_common.port);
	return 0;
}






#if 0






int usb_urb(usb_urb_t *u, msg_t *msg)
{
	FUN_TRACE;

	usb_driver_t find, *driver;
	usb_device_t *device;
	usb_endpoint_t *endpoint;
	setup_packet_t *setup;
	int err;

	void *async_buffer;

	find.pid = msg->pid;
	if ((driver = lib_treeof(usb_driver_t, linkage, lib_rbFind(&usbd_common.drivers, &find.linkage))) == NULL)
		return -EINVAL;

	if ((device = lib_treeof(usb_device_t, linkage, idtree_find(&usbd_common.devices, u->device_id))) == NULL)
		return -EINVAL;

	if ((endpoint = lib_treeof(usb_endpoint_t, linkage, idtree_find(&device->pipes, u->pipe))) == NULL)
		return -EINVAL;

	switch (u->type) {
	case usb_transfer_control:
		setup = dma_alloc64();
		*setup = u->setup;
		if (msg->i.size)
			err = usb_control(device, endpoint, setup, msg->i.data, -(int)msg->i.size);
		else if (msg->o.size)
			err = usb_control(device, endpoint, setup, msg->o.data, msg->o.size);
		else
			err = usb_control(device, endpoint, setup, NULL, 0);

		dma_free64(setup);
		break;

	case usb_transfer_bulk:
		if (msg->i.size)
			usb_stream(device, endpoint, transfer_bulk, out_token, msg->i.data, msg->i.size);

		if (msg->o.size)
			usb_stream(device, endpoint, transfer_bulk, in_token, msg->o.data, msg->o.size);

		if (u->async_in_size) {
			TRACE("urb %x", u->async_in_size);
			async_buffer = mmap(NULL, (u->async_in_size + SIZE_PAGE - 1) & ~(SIZE_PAGE - 1), PROT_WRITE | PROT_READ, MAP_ANONYMOUS | MAP_UNCACHED, OID_NULL, 0);

			memset(async_buffer, 0, (u->async_in_size + SIZE_PAGE - 1) & ~(SIZE_PAGE - 1));
			err = usb_asyncStream(device, endpoint, transfer_bulk, in_token, async_buffer, u->async_in_size);
		}

		err = EOK;
		break;

	case usb_transfer_interrupt:
		err = usb_interrupt(device, endpoint);
		break;

	case usb_transfer_isochronous:
	default:
		err = -ENOSYS;
	}

	return err;
}
















usb_queue_t *usb_allocQueue(usb_device_t *dev, usb_endpoint_t *ep, int transfer)
{
	FUN_TRACE;
	usb_queue_t *result;

	if (transfer == transfer_bulk && (result = ep->bulk) != NULL)
		return result; /* reuse, note: it is already linked */

	result = malloc(sizeof(usb_queue_t));

	if (result == NULL)
		return NULL;

	if ((result->qh = ehci_allocQh(dev->address, ep->number, transfer, ep->speed, ep->max_packet_len)) == NULL) {
		free(result);
		return NULL;
	}

	result->id = (unsigned)result->qh;
	result->async = 0;
	result->transfer_buffer = NULL;
	result->transfer_size = 0;
	result->device = dev;
	result->endpoint = ep;
	result->next = result->prev = NULL;
	result->transfers = NULL;
	result->prev = result->next = NULL;

	if (transfer == transfer_bulk)
		ep->bulk = result;

	LIST_ADD_EX(&dev->queues, result, dev_next, dev_prev);
	return result;
}


usb_transfer_t *usb_appendTransfer(usb_transfer_t **transfers, int token, char *buffer, size_t *size, int datax)
{
	FUN_TRACE;

	usb_transfer_t *transfer = malloc(sizeof(usb_transfer_t));

	if (transfer == NULL)
		return NULL;

	if ((transfer->qtd = ehci_allocQtd(token, buffer, size, datax)) == NULL) {
		free(transfer);
		return NULL;
	}

	LIST_ADD(transfers, transfer);
	return transfer;
}


void usb_linkTransfers(usb_queue_t *queue, usb_transfer_t *transfers)
{
	FUN_TRACE;
	usb_transfer_t *transfer = transfers->prev;
	transfer->qtd->ioc = 1;

	do {
		ehci_consQtd(transfer->qtd, queue->qh);
		transfer = transfer->prev;
	} while (transfer != transfers->prev);
}


void usb_deleteUnlinkedQueue(usb_queue_t *queue)
{
	FUN_TRACE;
	usb_transfer_t *transfer;

	if (queue->next != NULL)
		return; /* hack: dont unlink reused queues */

	while ((transfer = queue->transfers) != NULL) {
		ehci_freeQtd(transfer->qtd);
		LIST_REMOVE(&queue->transfers, transfer);
		free(transfer);
	}

	if (queue->device != NULL)
		LIST_REMOVE_EX(&queue->device->queues, queue, dev_next, dev_prev);

	ehci_freeQh(queue->qh);
	free(queue);
}


void usb_deleteQueue(usb_queue_t *queue)
{
	FUN_TRACE;
	ehci_unlinkQh(queue->prev->qh, queue->qh, queue->next->qh);
	LIST_REMOVE(&usbd_common.queues, queue);
	usb_deleteUnlinkedQueue(queue);
}


void usb_linkAsync(usb_queue_t *queue)
{
	FUN_TRACE;

	if (queue->next != NULL)
		return; /* hack: handle reused bulk queues */

	LIST_ADD(&usbd_common.queues, queue);

	if (queue == queue->next) {
		ehci_linkQh(queue->qh, queue->qh);
	}
	else {
		ehci_linkQh(queue->qh, queue->next->qh);
		ehci_linkQh(queue->prev->qh, queue->qh);
	}
}


void usb_linkInterrupt(usb_queue_t *queue)
{
	FUN_TRACE;

	LIST_ADD(&usbd_common.interrupt_queues, queue);
	ehci_insertPeriodic(queue->qh);
}


int usb_wait(usb_queue_t *queue, int timeout)
{
	int err = EOK;

	while (!ehci_qhFinished(queue->qh)) {
		if ((err = condWait(queue->device->cond, usbd_common.common_lock, timeout)) < 0) {
			TRACE("wait error %d", err);
			break;
		}
	}

	return err;
}


int usb_control(usb_device_t *dev, usb_endpoint_t *ep, setup_packet_t *packet, void *buffer, int ssize_in)
{
	FUN_TRACE;

	usb_queue_t *queue;
	usb_transfer_t *transfers = NULL;
	int data_token, control_token;
	size_t size;
	int dt, err;

	data_token = ssize_in <= 0 ? out_token : in_token;
	control_token = data_token == out_token ? in_token : out_token;

	if ((queue = usb_allocQueue(dev, ep, transfer_control)) == NULL)
		return -ENOMEM;

	size = sizeof(setup_packet_t);

	if (usb_appendTransfer(&transfers, setup_token, (char *)packet, &size, 0) == NULL) {
		usb_deleteUnlinkedQueue(queue);
		return -ENOMEM;
	}

	dt = 1;
	size = abs(ssize_in);

	while (size) {
		if (usb_appendTransfer(&transfers, data_token, buffer, &size, dt) == NULL) {
			usb_deleteUnlinkedQueue(queue);
			return -ENOMEM;
		}

		dt = !dt;
	}

	if (usb_appendTransfer(&transfers, control_token, NULL, NULL, 1) == NULL) {
		usb_deleteUnlinkedQueue(queue);
		return -ENOMEM;
	}

	usb_linkTransfers(queue, transfers);
	usb_linkAsync(queue);

	err = usb_wait(queue, USB_TIMEOUT);

	usb_deleteQueue(queue);

	return err;
}


int usb_setAddress(usb_device_t *dev, usb_endpoint_t *ep, unsigned char address)
{
	FUN_TRACE;

	int retval;
	setup_packet_t *setup = dma_alloc64();

	setup->bmRequestType = REQUEST_DIR_HOST2DEV | REQUEST_TYPE_STANDARD | REQUEST_RECIPIENT_DEVICE;
	setup->bRequest = SET_ADDRESS;
	setup->wValue = address;
	setup->wIndex = 0;
	setup->wLength = 0;

	retval = usb_control(dev, ep, setup, NULL, 0);
	dma_free64(setup);

	return retval;
}


int usb_getDescriptor(usb_device_t *dev, usb_endpoint_t *ep, int descriptor, int index, char *buffer, int size)
{
	FUN_TRACE;

	int retval;
	setup_packet_t *setup = dma_alloc64();

	setup->bmRequestType = REQUEST_DIR_DEV2HOST | REQUEST_TYPE_STANDARD | REQUEST_RECIPIENT_DEVICE;
	setup->bRequest = GET_DESCRIPTOR;
	setup->wValue = descriptor << 8 | index;
	setup->wIndex = 0;
	setup->wLength = size;

	retval = usb_control(dev, ep, setup, buffer, size);
	dma_free64(setup);

	return retval;
}


int usb_getConfigurationDescriptor(usb_device_t *dev, usb_endpoint_t *ep, configuration_desc_t *desc, int index, int length)
{
	return usb_getDescriptor(dev, ep, DESC_CONFIG, index, (char *)desc, length);
}


int usb_getDeviceDescriptor(usb_device_t *dev, usb_endpoint_t *ep, device_desc_t *desc)
{
	return usb_getDescriptor(dev, ep, DESC_DEVICE, 0, (char *)desc, sizeof(*desc));
}


int usb_setConfiguration(usb_device_t *dev, usb_endpoint_t *ep, int value)
{
	int retval;
	setup_packet_t *setup = dma_alloc64();

	setup->bmRequestType = REQUEST_DIR_HOST2DEV | REQUEST_TYPE_STANDARD | REQUEST_RECIPIENT_DEVICE;
	setup->bRequest = SET_CONFIGURATION;
	setup->wValue = value;
	setup->wIndex = 0;
	setup->wLength = 0;

	retval = usb_control(dev, ep, setup, NULL, 0);
	dma_free64(setup);

	return retval;
}


int usb_setInterface(usb_device_t *dev, usb_endpoint_t *ep, int alt, int index)
{
	int retval;
	setup_packet_t *setup = dma_alloc64();

	setup->bmRequestType = REQUEST_DIR_HOST2DEV | REQUEST_TYPE_STANDARD | REQUEST_RECIPIENT_INTERFACE;
	setup->bRequest = SET_INTERFACE;
	setup->wValue = alt;
	setup->wIndex = index;
	setup->wLength = 0;

	retval = usb_control(dev, ep, setup, NULL, 0);
	dma_free64(setup);

	return retval;
}


int usb_asyncStream(usb_device_t *dev, usb_endpoint_t *ep, int type, int token, void *buffer, size_t size)
{
	FUN_TRACE;
	usb_queue_t *queue;
	usb_transfers_t *transfers = NULL;

	if ((queue = usb_allocQueue(dev, ep, type)) == NULL)
		return -ENOMEM;

	queue->async = 1;
	queue->transfer_buffer = buffer;
	queue->transfer_size = size;

	while (size) {
		if (usb_appendTransfer(&transfers, token, buffer, &size, 0) == NULL) {
			usb_deleteUnlinkedQueue(queue);
			return -ENOMEM;
		}
	}

	usb_linkTransfers(queue, transfers);
	usb_linkAsync(queue);

	return queue->id;
}


int usb_stream(usb_device_t *dev, usb_endpoint_t *ep, int type, int token, void *buffer, size_t size)
{
	FUN_TRACE;

	//static int toggle = 0;
	int err;
	usb_queue_t *queue;
	usb_transfers_t *transfers = NULL;

	if ((queue = usb_allocQueue(dev, ep, type)) == NULL)
		return -ENOMEM;

	while (size) {
		if (usb_appendTransfer(&transfers, token, buffer, &size, 0) == NULL) {
			usb_deleteUnlinkedQueue(queue);
			return -ENOMEM;
		}
	}

	//toggle = !toggle;

	usb_linkTransfers(queue, transfers);
	usb_linkAsync(queue);

	err = usb_wait(queue, USB_TIMEOUT);
	usb_deleteQueue(queue);
	return err;
}


int usb_interrupt(usb_device_t *dev, usb_endpoint_t *ep)
{
	FUN_TRACE;
	int err;
	usb_queue_t *queue;
	void *buffer = dma_alloc64();
	size_t size = 64;
	usb_transfers_t *transfers = NULL;

	if ((queue = usb_allocQueue(dev, ep, transfer_interrupt)) == NULL)
		return -ENOMEM;

	queue->async = 1;
	queue->transfer_buffer = buffer;
	queue->transfer_size = 64;

	if (usb_appendTransfer(&transfers, in_token, buffer, &size, 0) == NULL) {
		usb_deleteUnlinkedQueue(queue);
		return -ENOMEM;
	}

	usb_linkTransfers(queue, transfers);
	usb_linkInterrupt(queue);

	return err;
}


void usb_dumpDeviceDescriptor(FILE *stream, device_desc_t *descr)
{
	fprintf(stream, "DEVICE DESCRIPTOR:\n");
	fprintf(stream, "\tbLength: %d\n", descr->bLength);
	fprintf(stream, "\tbDescriptorType: %d\n", descr->bDescriptorType);
	fprintf(stream, "\tbcdUSB: %d\n", descr->bcdUSB);
	fprintf(stream, "\tbDeviceClass: %d\n", descr->bDeviceClass);
	fprintf(stream, "\tbDeviceSubClass: %d\n", descr->bDeviceSubClass);
	fprintf(stream, "\tbDeviceProtocol: %d\n", descr->bDeviceProtocol);
	fprintf(stream, "\tbMaxPacketSize0: %d\n", descr->bMaxPacketSize0);
	fprintf(stream, "\tidVendor: %d\n", descr->idVendor);
	fprintf(stream, "\tidProduct: %d\n", descr->idProduct);
	fprintf(stream, "\tbcdDevice: %d\n", descr->bcdDevice);
	fprintf(stream, "\tiManufacturer: %d\n", descr->iManufacturer);
	fprintf(stream, "\tiProduct: %d\n", descr->iProduct);
	fprintf(stream, "\tiSerialNumber: %d\n", descr->iSerialNumber);
	fprintf(stream, "\tbNumConfigurations: %d\n", descr->bNumConfigurations);
}


void usb_dumpConfigurationDescriptor(FILE *stream, configuration_desc_t *desc)
{
	fprintf(stream, "CONFIGURATION DESCRIPTOR:\n");
	fprintf(stream, "\tbLength: %d\n", desc->bLength);
	fprintf(stream, "\tbDescriptorType: %d\n", desc->bDescriptorType);
	fprintf(stream, "\twTotalLength: %d\n", desc->wTotalLength);
	fprintf(stream, "\tbNumInterfaces: %d\n", desc->bNumInterfaces);
	fprintf(stream, "\tbConfigurationValue: %d\n", desc->bConfigurationValue);
	fprintf(stream, "\tiConfiguration: %d\n", desc->iConfiguration);
	fprintf(stream, "\tbmAttributes: %d\n", desc->bmAttributes);
	fprintf(stream, "\tbMaxPower: %d\n", desc->bMaxPower);
}


void usb_dumpInterfaceDescriptor(FILE *stream, interface_desc_t *desc)
{
	fprintf(stream, "INTERFACE DESCRIPTOR:\n");
	fprintf(stream, "\tbLength: %d\n", desc->bLength);
	fprintf(stream, "\tbDescriptorType: %d\n", desc->bDescriptorType);
	fprintf(stream, "\tbInterfaceNumber: %d\n", desc->bInterfaceNumber);
	fprintf(stream, "\tbAlternateSetting: %d\n", desc->bAlternateSetting);
	fprintf(stream, "\tbNumEndpoints: %d\n", desc->bNumEndpoints);
	fprintf(stream, "\tbInterfaceClass: %d\n", desc->bInterfaceClass);
	fprintf(stream, "\tbInterfaceSubClass: %d\n", desc->bInterfaceSubClass);
	fprintf(stream, "\tbInterfaceProtocol: %d\n", desc->bInterfaceProtocol);
	fprintf(stream, "\tiInterface: %d\n", desc->iInterface);
}


void usb_dumpEndpointDescriptor(FILE *stream, endpoint_desc_t *desc)
{
	fprintf(stream, "ENDPOINT DESCRIPTOR:\n");
	fprintf(stream, "\tbLength: %d\n", desc->bLength);
	fprintf(stream, "\tbDescriptorType: %d\n", desc->bDescriptorType);
	fprintf(stream, "\tbEndpointAddress: %d\n", desc->bEndpointAddress);
	fprintf(stream, "\tbmAttributes: %d\n", desc->bmAttributes);
	fprintf(stream, "\twMaxPacketSize: %d\n", desc->wMaxPacketSize);
	fprintf(stream, "\tbInterval: %d\n", desc->bInterval);
}


void usb_dumpDescriptor(FILE *stream, struct desc_header *desc)
{
	switch (desc->bDescriptorType) {
	case DESC_CONFIG:
		usb_dumpConfigurationDescriptor(stream, (void *)desc);
		break;

	case DESC_DEVICE:
		usb_dumpDeviceDescriptor(stream, (void *)desc);
		break;

	case DESC_INTERFACE:
		usb_dumpInterfaceDescriptor(stream, (void *)desc);
		break;

	case DESC_ENDPOINT:
		usb_dumpEndpointDescriptor(stream, (void *)desc);
		break;

	default:
		fprintf(stream, "UNRECOGNIZED DESCRIPTOR (%d)\n", desc->bDescriptorType);
		break;
	}
}


void usb_dumpConfiguration(FILE *stream, configuration_desc_t *desc)
{
	int remaining_size = desc->wTotalLength;
	struct desc_header *header = (void *)desc;

	while (remaining_size > 0) {
		usb_dumpDescriptor(stream, header);

		if (!header->bLength)
			break;

		remaining_size -= header->bLength;
		header = (struct desc_header *)((char *)header + header->bLength);
	}
}


int usb_driverMatch1(usb_driver_t *driver, usb_device_t *device)
{
	usb_device_id_t *filter = &driver->filter;
	device_desc_t *descriptor = device->descriptor;

	return (filter->idVendor == USB_CONNECT_WILDCARD || filter->idVendor == descriptor->idVendor) &&
		(filter->idProduct == USB_CONNECT_WILDCARD || filter->idProduct == descriptor->idProduct) &&
		(filter->bcdDevice == USB_CONNECT_WILDCARD || filter->bcdDevice == descriptor->bcdDevice);
}


int usb_driverMatch2(usb_driver_t *driver, usb_device_t *device)
{
	usb_device_id_t *filter = &driver->filter;
	device_desc_t *descriptor = device->descriptor;

	return (filter->idVendor == USB_CONNECT_WILDCARD || filter->idVendor == descriptor->idVendor) &&
		(filter->idProduct == USB_CONNECT_WILDCARD || filter->idProduct == descriptor->idProduct);
}


int usb_driverMatch3(usb_driver_t *driver, usb_device_t *device)
{
	usb_device_id_t *filter = &driver->filter;
	device_desc_t *descriptor = device->descriptor;

	if (descriptor->bDeviceClass == 0xff) {
		return (filter->idVendor == USB_CONNECT_WILDCARD || filter->idVendor == descriptor->idVendor) &&
			(filter->bDeviceSubClass == USB_CONNECT_WILDCARD || filter->bDeviceSubClass == descriptor->bDeviceSubClass) &&
			(filter->bDeviceProtocol == USB_CONNECT_WILDCARD || filter->bDeviceProtocol == descriptor->bDeviceProtocol);
	}
	else {
		return (filter->bDeviceClass == USB_CONNECT_WILDCARD || filter->bDeviceClass == descriptor->bDeviceClass) &&
			(filter->bDeviceSubClass == USB_CONNECT_WILDCARD || filter->bDeviceSubClass == descriptor->bDeviceSubClass) &&
			(filter->bDeviceProtocol == USB_CONNECT_WILDCARD || filter->bDeviceProtocol == descriptor->bDeviceProtocol);
	}
}


int usb_driverMatch4(usb_driver_t *driver, usb_device_t *device)
{
	usb_device_id_t *filter = &driver->filter;
	device_desc_t *descriptor = device->descriptor;

	if (descriptor->bDeviceClass == 0xff) {
		return (filter->idVendor == USB_CONNECT_WILDCARD || filter->idVendor == descriptor->idVendor) &&
			(filter->bDeviceSubClass == USB_CONNECT_WILDCARD || filter->bDeviceSubClass == descriptor->bDeviceSubClass);
	}
	else {
		return (filter->bDeviceClass == USB_CONNECT_WILDCARD || filter->bDeviceClass == descriptor->bDeviceClass) &&
			(filter->bDeviceSubClass == USB_CONNECT_WILDCARD || filter->bDeviceSubClass == descriptor->bDeviceSubClass);
	}
}


usb_driver_t *usb_findDriver(usb_device_t *device)
{
	FUN_TRACE;

	const int (*usb_driverMatch[])(usb_driver_t *, usb_device_t *) = {
		usb_driverMatch1, usb_driverMatch2, usb_driverMatch3, usb_driverMatch4
	};

	rbnode_t *node;
	usb_driver_t *driver;
	int i;

	for (i = 0; i < 4; ++i) {
		for (node = lib_rbMinimum(usbd_common.drivers.root); node != NULL; node = lib_rbNext(node)) {
			driver = lib_treeof(usb_driver_t, linkage, node);
			if (usb_driverMatch[i](driver, device)) {
				TRACE("found driver");
				return driver;
			}
		}
	}
	return NULL;
}


int usb_connectDriver(usb_driver_t *driver, usb_device_t *device, configuration_desc_t *configuration)
{
	FUN_TRACE;

	msg_t msg;
	usb_event_t *event = (void *)msg.i.raw;
	usb_insertion_t *insertion = &event->insertion;

	memset(&msg, 0, sizeof(msg));
	msg.type = mtDevCtl;
	msg.i.data = configuration;
	msg.i.size = configuration->wTotalLength;

	event->type = usb_event_insertion;
	insertion->device_id = idtree_id(&device->linkage);
	memcpy(&insertion->descriptor, device->descriptor, sizeof(device_desc_t));

	return msgSend(driver->port, &msg);
}


usb_endpoint_t *usb_findPipe(usb_device_t *device, int pipe)
{
	return lib_treeof(usb_endpoint_t, linkage, idtree_find(&device->pipes, pipe));
}


int usb_openPipe(usb_device_t *device, endpoint_desc_t *descriptor)
{
	FUN_TRACE;

	usb_endpoint_t *pipe = malloc(sizeof(usb_endpoint_t));
	pipe->speed = full_speed;

	pipe->max_packet_len = 64; //descriptor->wMaxPacketSize;
	pipe->number = descriptor->bEndpointAddress & 0xf;
	return idtree_alloc(&device->pipes, &pipe->linkage);
}


int usb_getConfiguration(usb_device_t *device, void *buffer, size_t bufsz)
{
	configuration_desc_t *conf = dma_alloc64();
	usb_endpoint_t control_endpt = {
		.number = 0,
		.speed = full_speed,
		.max_packet_len = 64,
	};

	usb_getConfigurationDescriptor(device, &control_endpt, conf, 0, sizeof(configuration_desc_t));

	if (bufsz < conf->wTotalLength)
		return -ENOBUFS;

	usb_getConfigurationDescriptor(device, &control_endpt, buffer, 0, conf->wTotalLength);
	dma_free64(conf);
	return EOK;
}


void usb_deviceAttach(void)
{
	FUN_TRACE;

	usb_device_t *dev;
	usb_endpoint_t *ep;
	usb_driver_t *driver;
	void *configuration;
	device_desc_t *ddesc = dma_alloc64();

	TRACE("reset");
	ehci_resetPort();

	dev = calloc(1, sizeof(usb_device_t));

	mutexCreate(&dev->lock);
	condCreate(&dev->cond);

	ep = calloc(1, sizeof(usb_endpoint_t));

	ep->number = 0;
	ep->speed = full_speed;
	ep->max_packet_len = 64;

	idtree_init(&dev->pipes);
	idtree_alloc(&dev->pipes, &ep->linkage);

	TRACE("getting device descriptor");
	usb_getDeviceDescriptor(dev, ep, ddesc);
	ehci_resetPort();

	dev->descriptor = ddesc;
	ep->max_packet_len = ddesc->bMaxPacketSize0;

	usb_dumpDeviceDescriptor(stderr, ddesc);

	TRACE("setting address");
	usb_setAddress(dev, ep, 1);
	dev->address = 1;

	idtree_alloc(&usbd_common.devices, &dev->linkage);

	if ((driver = usb_findDriver(dev)) != NULL) {
		TRACE("got driver");
		configuration = mmap(NULL, SIZE_PAGE, PROT_WRITE | PROT_READ, MAP_ANONYMOUS | MAP_UNCACHED, OID_NULL, 0);
		usb_getConfiguration(dev, configuration, SIZE_PAGE);
		if (usb_connectDriver(driver, dev, configuration) < 0) {
			LIST_ADD(&usbd_common.orphan_devices, dev);
			dev->driver = NULL;
		}
		else {
			LIST_ADD(&driver->devices, dev);
			dev->driver = driver;
		}
		munmap(configuration, SIZE_PAGE);
	}
	else {
		TRACE("no driver");
		LIST_ADD(&usbd_common.orphan_devices, dev);
	}
}


void usb_deviceDetach(void)
{
	FUN_TRACE;
	usb_device_t *device = lib_treeof(usb_device_t, linkage, usbd_common.devices.root);
	usb_queue_t *queue;

	if (device != NULL) {
		idtree_remove(&usbd_common.devices, &device->linkage);

		while (device->queues != NULL) {
			usb_deleteQueue(device->queues);
		}

		if (device->driver != NULL) {
			LIST_REMOVE(&device->driver->devices, device);
			device->driver = NULL;
		}
		else {
			LIST_REMOVE(&usbd_common.orphan_devices, device);
		}

		resourceDestroy(device->lock);
		resourceDestroy(device->cond);
		free(device);
	}
}


void usb_portthr(void *arg)
{
	int attached = 0;

	mutexLock(usbd_common.common_lock);

	for (;;) {
		condWait(usbd_common.port_cond, usbd_common.common_lock, 0);
		FUN_TRACE;

		if (ehci_deviceAttached()) {
			if (attached) {
				TRACE("double attach");
			}
			else {
				usb_deviceAttach();
				attached = 1;
			}
		}
		else {
			if (!attached) {
				TRACE("double detach");
			}
			else {
				usb_deviceDetach();
				attached = 0;
			}
		}
	}
}


void usb_signalDriver(usb_queue_t *queue)
{
	usb_driver_t *driver;
	usb_event_t *event;
	msg_t msg = { 0 };

	if ((driver = queue->device->driver) == NULL)
		return;

	msg.type = mtDevCtl;
	event = (void *)msg.i.raw;
	event->type = usb_event_interrupt;
	event->interrupt.transfer_id = queue->id;
	msg.i.size = queue->transfer_size;
	msg.i.data = queue->transfer_buffer;

	msgSend(queue->device->driver->port, &msg);


	if (queue->transfer_size > 64 /*hack*/) {
		munmap(queue->transfer_buffer, (queue->transfer_size + SIZE_PAGE - 1) & ~(SIZE_PAGE - 1));
		usb_deleteQueue(queue);
	}
	else {
		dma_free64(queue->transfer_buffer);
		LIST_REMOVE(&usbd_common.interrupt_queues, queue);
		usb_deleteUnlinkedQueue(queue);
	}
}


void usb_signalThread(void *arg)
{
	usb_queue_t *queue;

	mutexLock(usbd_common.common_lock);

	for (;;) {
		while ((queue = usbd_common.signal_queues) == NULL)
			condWait(usbd_common.signal_cond, usbd_common.common_lock, 0);

		LIST_REMOVE_EX(&usbd_common.signal_queues, queue, signal_next, signal_prev);
		TRACE("signaling driver");
		usb_signalDriver(queue);
	}
}


void usb_eventCallback(int port_change)
{
	FUN_TRACE;
	usb_queue_t *queue;

	mutexLock(usbd_common.common_lock);
	if ((queue = usbd_common.queues) != NULL) {
		do {
			if (ehci_qhFinished(queue->qh)) {
				TRACE("queue finished %p", queue);

				if (queue->async) {
					LIST_ADD_EX(&usbd_common.signal_queues, queue, signal_next, signal_prev);
					condSignal(usbd_common.signal_cond);
				}
				else {
					condSignal(queue->device->cond);
				}
			}
			queue = queue->next;
		}
		while (queue != usbd_common.queues);
	}

	if ((queue = usbd_common.interrupt_queues) != NULL) {
		do {
			if (ehci_qhFinished(queue->qh)) {
				TRACE("interrupt queue finished %p", queue);

				if (!queue->async)
					TRACE("dupa 1");

				LIST_ADD_EX(&usbd_common.signal_queues, queue, signal_next, signal_prev);
				condSignal(usbd_common.signal_cond);
			}
			queue = queue->next;
		}
		while (queue != usbd_common.interrupt_queues);
	}

	if (port_change) {
		TRACE("port change");
		condSignal(usbd_common.port_cond);
	}
	mutexUnlock(usbd_common.common_lock);
}


int usb_driver_cmp(rbnode_t *n1, rbnode_t *n2)
{
	usb_driver_t *d1 = lib_treeof(usb_driver_t, linkage, n1);
	usb_driver_t *d2 = lib_treeof(usb_driver_t, linkage, n2);

	if (d1->pid > d2->pid)
		return 1;
	else if (d1->pid < d2->pid)
		return -1;

	return 0;
}


int usb_connect(usb_connect_t *c, unsigned pid)
{
	FUN_TRACE;

	const int (*usb_driverMatch[])(usb_driver_t *, usb_device_t *) = {
		usb_driverMatch1, usb_driverMatch2, usb_driverMatch3, usb_driverMatch4
	};

	int i;
	usb_driver_t *driver = malloc(sizeof(*driver));
	usb_device_t *device;
	void *configuration;

	if (driver == NULL)
		return -ENOMEM;

	driver->port = c->port;
	driver->filter = c->filter;
	driver->pid = pid;
	driver->requests = NULL;
	driver->devices = NULL;

	lib_rbInsert(&usbd_common.drivers, &driver->linkage);

	if (usbd_common.orphan_devices != NULL) {
		configuration = mmap(NULL, SIZE_PAGE, PROT_WRITE | PROT_READ, MAP_ANONYMOUS | MAP_UNCACHED, OID_NULL, 0);

		if (configuration == MAP_FAILED)
			return -ENOMEM;

		for (i = 0; i < 4; ++i) {
			device = usbd_common.orphan_devices;

			do {
				while (device != NULL && usb_driverMatch[i](driver, device)) {
					usb_getConfiguration(device, configuration, SIZE_PAGE);
					usb_connectDriver(driver, device, configuration);

					if (device == usbd_common.orphan_devices) {
						LIST_REMOVE(&usbd_common.orphan_devices, device);
						device = usbd_common.orphan_devices;
					}
					else {
						LIST_REMOVE(&device, device);
					}
				}
			}
			while (device != NULL && (device = device->next) != usbd_common.orphan_devices);
		}

		munmap(configuration, SIZE_PAGE);
	}

	return EOK;
}


int usb_urb(usb_urb_t *u, msg_t *msg)
{
	FUN_TRACE;

	usb_driver_t find, *driver;
	usb_device_t *device;
	usb_endpoint_t *endpoint;
	setup_packet_t *setup;
	int err;

	void *async_buffer;

	find.pid = msg->pid;
	if ((driver = lib_treeof(usb_driver_t, linkage, lib_rbFind(&usbd_common.drivers, &find.linkage))) == NULL)
		return -EINVAL;

	if ((device = lib_treeof(usb_device_t, linkage, idtree_find(&usbd_common.devices, u->device_id))) == NULL)
		return -EINVAL;

	if ((endpoint = lib_treeof(usb_endpoint_t, linkage, idtree_find(&device->pipes, u->pipe))) == NULL)
		return -EINVAL;

	switch (u->type) {
	case usb_transfer_control:
		setup = dma_alloc64();
		*setup = u->setup;
		if (msg->i.size)
			err = usb_control(device, endpoint, setup, msg->i.data, -(int)msg->i.size);
		else if (msg->o.size)
			err = usb_control(device, endpoint, setup, msg->o.data, msg->o.size);
		else
			err = usb_control(device, endpoint, setup, NULL, 0);

		dma_free64(setup);
		break;

	case usb_transfer_bulk:
		if (msg->i.size)
			usb_stream(device, endpoint, transfer_bulk, out_token, msg->i.data, msg->i.size);

		if (msg->o.size)
			usb_stream(device, endpoint, transfer_bulk, in_token, msg->o.data, msg->o.size);

		if (u->async_in_size) {
			TRACE("urb %x", u->async_in_size);
			async_buffer = mmap(NULL, (u->async_in_size + SIZE_PAGE - 1) & ~(SIZE_PAGE - 1), PROT_WRITE | PROT_READ, MAP_ANONYMOUS | MAP_UNCACHED, OID_NULL, 0);

			memset(async_buffer, 0, (u->async_in_size + SIZE_PAGE - 1) & ~(SIZE_PAGE - 1));
			err = usb_asyncStream(device, endpoint, transfer_bulk, in_token, async_buffer, u->async_in_size);
		}

		err = EOK;
		break;

	case usb_transfer_interrupt:
		err = usb_interrupt(device, endpoint);
		break;

	case usb_transfer_isochronous:
	default:
		err = -ENOSYS;
	}

	return err;
}


int usb_open(usb_open_t *o, msg_t *msg)
{
	FUN_TRACE;
	usb_device_t *device;

	if ((device = lib_treeof(usb_device_t, linkage, idtree_find(&usbd_common.devices, o->device_id))) == NULL)
		return -EINVAL;

	return usb_openPipe(device, &o->endpoint);
}


void msgthr(void *arg)
{
	unsigned port = (int)arg;
	unsigned rid;
	msg_t msg;
	usb_msg_t *umsg;


	for (;;) {
		TRACE("receiving");
		if (msgRecv(port, &msg, &rid) < 0)
			continue;

		mutexLock(usbd_common.common_lock);

		if (msg.type == mtDevCtl) {
			umsg = (void *)msg.i.raw;

			switch (umsg->type) {
			case usb_msg_connect:
				msg.o.io.err = usb_connect(&umsg->connect, msg.pid);
				break;
			case usb_msg_urb:
				msg.o.io.err = usb_urb(&umsg->urb, &msg);
				break;
			case usb_msg_open:
				msg.o.io.err = usb_open(&umsg->open, &msg);
				break;
			}
		}
		else {
			msg.o.io.err = -EINVAL;
		}
		mutexUnlock(usbd_common.common_lock);

		msgRespond(port, &msg, rid);
	}
}


int main(int argc, char **argv)
{
	//asm volatile ("1: b 1b");

	FUN_TRACE;
	oid_t oid;
	portCreate(&usbd_common.port);

	mutexCreate(&usbd_common.common_lock);
	mutexCreate(&usbd_common.port_lock);
	condCreate(&usbd_common.port_cond);
	condCreate(&usbd_common.signal_cond);

	usbd_common.signal_queues = NULL;
	usbd_common.queues = NULL;
	usbd_common.interrupt_queues = NULL;
	usbd_common.orphan_devices = NULL;
	lib_rbInit(&usbd_common.drivers, usb_driver_cmp, NULL);
	idtree_init(&usbd_common.devices);

	ehci_init(usb_eventCallback, usbd_common.common_lock);

	oid.port = usbd_common.port;
	oid.id = 0;
	create_dev(&oid, "/dev/usb");

	beginthread(usb_portthr, 4, malloc(0x4000), 0x4000, NULL);
	beginthread(usb_signalThread, 4, malloc(0x4000), 0x4000, NULL);

	beginthread(msgthr, 4, malloc(0x4000), 0x4000, (void *)usbd_common.port);
	beginthread(msgthr, 4, malloc(0x4000), 0x4000, (void *)usbd_common.port);
	msgthr((void *)usbd_common.port);
	return 0;
}


#endif