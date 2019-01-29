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


#define FUN_TRACE  //fprintf(stderr, "usbd trace: %s\n", __PRETTY_FUNCTION__)
#define TRACE(x, ...) //fprintf(stderr, "usbd: " x "\n", ##__VA_ARGS__)


pid_t telit = 0;

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
	volatile int aborted;

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
	handle_t async_cond, port_cond, reset_cond;

	usb_device_t *reset_device;
} usbd_common;


usb_qtd_list_t *usb_allocQtd(int token, char *buffer, size_t *size, int datax)
{
	//FUN_TRACE;

	usb_qtd_list_t *element = malloc(sizeof(usb_qtd_list_t));

	if ((element->qtd = ehci_allocQtd(token, buffer, size, datax)) == NULL)
		return NULL;

	element->size = ehci_qtdRemainingBytes(element->qtd);

	return element;
}


void usb_addQtd(usb_transfer_t *transfer, int token, void *buffer, size_t *size, int datax)
{
	//FUN_TRACE;

	usb_qtd_list_t *el = usb_allocQtd(token, buffer, size, datax);

	TRACE("alloced %p qtd", el->qtd);
	LIST_ADD(&transfer->qtds, el);
}


usb_transfer_t *usb_allocTransfer(usb_endpoint_t *endpoint, int direction, int transfer_type, void *buffer, size_t size, int async)
{
	//FUN_TRACE;

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
	result->aborted = 0;

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
	//FUN_TRACE;

	usb_qtd_list_t *element, *next;

	if (transfer->setup != NULL)
		dma_free64(transfer->setup);

	if (transfer->endpoint->qh != NULL)
		ehci_dequeue(transfer->endpoint->qh, transfer->qtds->qtd, transfer->qtds->prev->qtd);

	element = transfer->qtds;
	do {
		next = element->next;
		ehci_freeQtd(element->qtd);
		free(element);
	}
	while ((element = next) != transfer->qtds);

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
	if (transfer->aborted)
		return 1;

	usb_qtd_list_t *qtd = transfer->qtds;
	int finished = ehci_qtdFinished(qtd->prev->qtd);
	int error = 0;

	do {
		error |= ehci_qtdError(qtd->qtd) | ehci_qtdBabble(qtd->qtd);
		qtd = qtd->next;
	} while (qtd != transfer->qtds);

	return error ? -1 : finished;
}

volatile int resetting = 0;
int usb_handleUrb(usb_urb_t *urb, usb_driver_t *driver, usb_device_t *device, usb_endpoint_t *endpoint, void *buffer)
{
	FUN_TRACE;

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


		if (resetting) {
			// asm volatile("1:  b 1b");
			// ehci_activate(endpoint->qh);
			TRACE("WAIT");
		}


		while (!transfer->finished && !transfer->aborted)
			condWait(transfer->cond, usbd_common.common_lock, 0);

		if (resetting) {
			//asm volatile("1:  b 1b");
			//ehci_activate(endpoint->qh);
			TRACE("END");
		}

		LIST_REMOVE(&usbd_common.active_transfers, transfer);
		usb_deleteTransfer(transfer);
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

		if (buffer == MAP_FAILED) {
			TRACE("no mem");
			return -ENOMEM;
		}

		if (inbuf != NULL)
			memcpy(buffer, inbuf, urb->transfer_size);
	}

	int err = usb_handleUrb(urb, driver, device, endpoint, buffer);

	if (outbuf != NULL && urb->direction == usb_transfer_in)
		memcpy(outbuf, buffer, urb->transfer_size);

	if (buffer != NULL && !urb->async)
		munmap(buffer, (urb->transfer_size + SIZE_PAGE - 1) & ~(SIZE_PAGE - 1));

	return err;
}


int usb_setAddress(usb_device_t *dev, unsigned char address);
void usb_signalDetach(usb_device_t *device);

void usb_resetDevice(usb_device_t *device)
{
	FUN_TRACE;

	usb_endpoint_t *ep;
	ehci_unlinkQh(device->control_endpoint->qh);
	device->control_endpoint->qh = NULL; /* FIXME: leak */

	if ((ep = device->endpoints) != NULL) {
		do {
			if (ep->qh != NULL)
				ehci_unlinkQh(ep->qh);
			ep->qh = NULL; /* FIXME: leak */
		}
		while ((ep = ep->next) != device->endpoints);
	}

	usb_transfer_t *transfer;
	if ((transfer = usbd_common.active_transfers) != NULL) {
		do {
			transfer->aborted = 1;
			condSignal(transfer->cond);
		}
		while ((transfer = transfer->next) != usbd_common.active_transfers);
	}

	ehci_resetPort();

	device->address = 0;
	usb_setAddress(device, 1 + idtree_id(&device->linkage));
	TRACE("reset: address is set");
	device->address = 1 + idtree_id(&device->linkage);
	ehci_qhSetAddress(device->control_endpoint->qh, device->address);

	usb_signalDetach(device);
}


void usb_resetThread(void *arg)
{
	mutexLock(usbd_common.common_lock);

	for (;;) {
		condWait(usbd_common.reset_cond, usbd_common.common_lock, 0);

		if (usbd_common.reset_device != NULL) {
			usb_resetDevice(usbd_common.reset_device);
			usbd_common.reset_device = NULL;
		}
	}
}


void usb_eventCallback(int port_change)
{
	FUN_TRACE;
	usb_transfer_t *transfer;
	int error;

	if ((transfer = usbd_common.active_transfers) != NULL) {
		do {
			if ((error = usb_finished(transfer))) {
				TRACE("transfer finished %x", transfer->id);
				transfer->finished = error;

				if (transfer->async)
					LIST_ADD_EX(&usbd_common.finished_transfers, transfer, finished_next, finished_prev);

#if 1
				if (error < 0) {
					TRACE("err async%d", transfer->async);

					resetting = 1;
					usbd_common.reset_device = transfer->endpoint->device;
					condSignal(usbd_common.reset_cond);

					return;
				}
#endif
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
		transferred_bytes += qtd->size - ehci_qtdRemainingBytes(qtd->qtd);
		qtd = qtd->next;
	} while (qtd != transfer->qtds);

	return transferred_bytes;
}


void usb_signalDetach(usb_device_t *device)
{
	FUN_TRACE;

	usb_driver_t *driver;
	usb_event_t *event;
	msg_t msg = { 0 };

	if ((driver = device->driver) == NULL) {
		TRACE("detach: no driver!");
		return;
	}

	msg.type = mtDevCtl;

	event = (void *)msg.i.raw;
	event->type = usb_event_removal;
	event->removal.device_id = idtree_id(&device->linkage);

	msgSend(device->driver->port, &msg);
}


void usb_signalDriver(usb_transfer_t *transfer)
{
	FUN_TRACE;

	usb_driver_t *driver;
	usb_event_t *event;
	msg_t msg = { 0 };

	if ((driver = transfer->endpoint->device->driver) == NULL) {
		TRACE("no driver!");
		return;
	}

	msg.type = mtDevCtl;

	event = (void *)msg.i.raw;
	event->type = usb_event_completion;
	event->completion.transfer_id = transfer->id;
	event->completion.pipe = idtree_id(&transfer->endpoint->linkage);

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

	usb_endpoint_t *pipe = calloc(1, sizeof(usb_endpoint_t));

	pipe->max_packet_len = 64; //*/descriptor->wMaxPacketSize;
	pipe->number = descriptor->bEndpointAddress & 0xf;
	pipe->next = pipe->prev = NULL;
	pipe->device = device;
	pipe->qh = NULL;

	LIST_ADD(&device->endpoints, pipe);

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
	idtree_alloc(&usbd_common.devices, &dev->linkage);

	usb_setAddress(dev, 1 + idtree_id(&dev->linkage));
	dev->address = 1 + idtree_id(&dev->linkage);
	ehci_qhSetAddress(dev->control_endpoint->qh, dev->address);

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

		ehci_unlinkQh(device->control_endpoint->qh);

		usb_endpoint_t *ep = device->endpoints;
		if (ep != NULL) {
			do
				if (ep->qh != NULL)
					ehci_unlinkQh(ep->qh);
			while ((ep = ep->next) != device->endpoints);
		}


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

	telit = pid;

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

			if (umsg->type == usb_msg_clear) {
				resetting = 0;
			}
			else if (!resetting) {
				switch (umsg->type) {
				case usb_msg_connect:
					msg.o.io.err = usb_connect(&umsg->connect, msg.pid);
					break;
				case usb_msg_urb:
					msg.o.io.err = usb_submitUrb(msg.pid, &umsg->urb, msg.i.data, msg.o.data);
					break;
				case usb_msg_open:
					msg.o.io.err = usb_open(&umsg->open, &msg);
				default:
					break;
				}
			}
			else {
				msg.o.io.err = -EINVAL;
			}
		}
		else {
			TRACE("msgthr: inval");
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
	condCreate(&usbd_common.reset_cond);

	usbd_common.active_transfers = NULL;
	usbd_common.finished_transfers = NULL;
	usbd_common.orphan_devices = NULL;
	usbd_common.reset_device = NULL;
	lib_rbInit(&usbd_common.drivers, usb_driver_cmp, NULL);
	idtree_init(&usbd_common.devices);

	ehci_init(usb_eventCallback, usbd_common.common_lock);

	oid.port = usbd_common.port;
	oid.id = 0;
	create_dev(&oid, "/dev/usb");

	beginthread(usb_portthr, 4, malloc(0x4000), 0x4000, NULL);
	beginthread(usb_signalThread, 4, malloc(0x4000), 0x4000, NULL);
	beginthread(usb_resetThread, 4, malloc(0x4000), 0x4000, NULL);

	beginthread(msgthr, 4, malloc(0x4000), 0x4000, (void *)usbd_common.port);
	beginthread(msgthr, 4, malloc(0x4000), 0x4000, (void *)usbd_common.port);
	beginthread(msgthr, 4, malloc(0x4000), 0x4000, (void *)usbd_common.port);
	msgthr((void *)usbd_common.port);
	return 0;
}

