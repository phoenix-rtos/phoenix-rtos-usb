/*
 * Phoenix-RTOS
 *
 * USB Host Server
 *
 * Copyright 2018, 2020 Phoenix Systems
 * Author: Jan Sikorski, Hubert Buczynski
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
#include <sys/stat.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <syslog.h>

#include <posix/idtree.h>
#include <posix/utils.h>

#include <dma.h>
#include <ehci.h>

#include <usb.h>
#include "hostsrv.h"


#define FUN_TRACE  //fprintf(stderr, "hostsrv trace: %s\n", __PRETTY_FUNCTION__)
#define TRACE(x, ...) //fprintf(stderr, "hostsrv: " x "\n", ##__VA_ARGS__)
#define TRACE_FAIL(x, ...) syslog(LOG_WARNING, x, ##__VA_ARGS__)


pid_t telit = 0;

typedef struct {
	rbnode_t linkage;
	unsigned pid;
	int port;
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

	usb_device_desc_t *descriptor;
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
	usb_setup_packet_t *setup;

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
} hostsrv_common;


usb_qtd_list_t *hostsrv_allocQtd(int token, char *buffer, size_t *size, int datax)
{
	//FUN_TRACE;

	usb_qtd_list_t *element = malloc(sizeof(usb_qtd_list_t));

	if ((element->qtd = ehci_allocQtd(token, buffer, size, datax)) == NULL)
		return NULL;

	element->size = ehci_qtdRemainingBytes(element->qtd);

	return element;
}


void hostsrv_addQtd(usb_transfer_t *transfer, int token, void *buffer, size_t *size, int datax)
{
	//FUN_TRACE;

	usb_qtd_list_t *el = hostsrv_allocQtd(token, buffer, size, datax);

	TRACE("allocated %p qtd", el->qtd);
	LIST_ADD(&transfer->qtds, el);
}


usb_transfer_t *hostsrv_allocTransfer(usb_endpoint_t *endpoint, int direction, int transfer_type, void *buffer, size_t size, int async)
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
		result->cond = hostsrv_common.async_cond;
	else
		condCreate(&result->cond);

	result->transfer_buffer = buffer;
	result->transfer_size = size;
	result->setup = NULL;
	result->qtds = NULL;
	return result;
}


void hostsrv_deleteTransfer(usb_transfer_t *transfer)
{
	//FUN_TRACE;

	usb_qtd_list_t *element, *next;

	if (transfer->setup != NULL)
		dma_free64(transfer->setup);

	if ((element = transfer->qtds) != NULL) {
		do {
			next = element->next;
			ehci_freeQtd(element->qtd);
			free(element);
		}
		while ((element = next) != transfer->qtds);
	}

	if (!transfer->async)
		resourceDestroy(transfer->cond);

	free(transfer);
}


void hostsrv_linkTransfer(usb_endpoint_t *endpoint, usb_transfer_t *transfer)
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
	LIST_ADD(&hostsrv_common.active_transfers, transfer);
	ehci_enqueue(endpoint->qh, transfer->qtds->qtd, transfer->qtds->prev->qtd);
}


int hostsrv_finished(usb_transfer_t *transfer)
{
	if (transfer->aborted)
		return 1;

	usb_qtd_list_t *qtd = transfer->qtds;
	int finished = ehci_qtdFinished(qtd->prev->qtd);
	int error = 0;

	do {
		if (ehci_qtdError(qtd->qtd)) {
			TRACE_FAIL("transaction error");
			error++;
		}

		if (ehci_qtdBabble(qtd->qtd)) {
			TRACE_FAIL("babble");
			error++;
		}

		qtd = qtd->next;
	} while (qtd != transfer->qtds);

	return error ? -1 : finished;
}


int hostsrv_handleUrb(usb_urb_t *urb, usb_driver_t *driver, usb_device_t *device, usb_endpoint_t *endpoint, void *buffer)
{
	FUN_TRACE;

	usb_transfer_t *transfer;
	size_t remaining_size;
	int datax = 1;
	char *transfer_buffer;
	int data_token = urb->direction == usb_transfer_out ? out_token : in_token;
	int control_token = data_token == out_token ? in_token : out_token;


	transfer = hostsrv_allocTransfer(endpoint, urb->direction, urb->type /* FIXME: should explicitly use enum from ehci.h */, buffer, urb->transfer_size, urb->async);

	if (urb->type == usb_transfer_control) {
		transfer->setup = dma_alloc64();
		*transfer->setup = urb->setup;
		remaining_size = sizeof(usb_setup_packet_t);
		hostsrv_addQtd(transfer, setup_token, transfer->setup, &remaining_size, 0);
	}

	remaining_size = transfer->transfer_size;

	while (remaining_size) {
		transfer_buffer = (char *)transfer->transfer_buffer + transfer->transfer_size - remaining_size;
		hostsrv_addQtd(transfer, data_token, transfer_buffer, &remaining_size, datax);
		datax = !datax;
	}

	if (urb->type == usb_transfer_control) {
		hostsrv_addQtd(transfer, control_token, NULL, NULL, 1);
	}

	if (transfer->qtds == NULL) {
		hostsrv_deleteTransfer(transfer);
		return EOK;
	}

	hostsrv_linkTransfer(endpoint, transfer);

	/* TODO; dont wait here */
	if (!transfer->async) {
		while (!transfer->finished && !transfer->aborted)
			condWait(transfer->cond, hostsrv_common.common_lock, 0);

		LIST_REMOVE(&hostsrv_common.active_transfers, transfer);
		hostsrv_deleteTransfer(transfer);

		if (transfer->aborted || transfer->finished < 0)
			return -EIO;

		return EOK;
	}
	else {
		return transfer->id;
	}
}


int hostsrv_submitUrb(int pid, usb_urb_t *urb, void *inbuf, void *outbuf)
{
	FUN_TRACE;

	usb_driver_t find, *driver;
	usb_device_t *device;
	usb_endpoint_t *endpoint;

	void *buffer = NULL;

	find.pid = pid;

	if ((driver = lib_treeof(usb_driver_t, linkage, lib_rbFind(&hostsrv_common.drivers, &find.linkage))) == NULL) {
		TRACE("no driver");
		return -EINVAL;
	}

	if ((device = lib_treeof(usb_device_t, linkage, idtree_find(&hostsrv_common.devices, urb->device_id))) == NULL) {
		TRACE("no device");
		return -EINVAL;
	}

	if ((endpoint = lib_treeof(usb_endpoint_t, linkage, idtree_find(&device->pipes, urb->pipe))) == NULL) {
		TRACE("no endpoint");
		return -EINVAL;
	}

	if (urb->transfer_size) {
		buffer = mmap(NULL, (urb->transfer_size + _PAGE_SIZE - 1) & ~(_PAGE_SIZE - 1), PROT_WRITE | PROT_READ, MAP_ANONYMOUS | MAP_UNCACHED, -1, 0);

		if (buffer == MAP_FAILED) {
			TRACE("no mem");
			return -ENOMEM;
		}

		if (inbuf != NULL)
			memcpy(buffer, inbuf, urb->transfer_size);
	}

	int err = hostsrv_handleUrb(urb, driver, device, endpoint, buffer);

	if (outbuf != NULL && urb->direction == usb_transfer_in)
		memcpy(outbuf, buffer, urb->transfer_size);

	if (buffer != NULL && !urb->async)
		munmap(buffer, (urb->transfer_size + _PAGE_SIZE - 1) & ~(_PAGE_SIZE - 1));

	return err;
}


int hostsrv_setAddress(usb_device_t *dev, unsigned char address);


void hostsrv_resetDevice(usb_device_t *device)
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
	if ((transfer = hostsrv_common.active_transfers) != NULL) {
		do {
			transfer->aborted = 1;
			condSignal(transfer->cond);
		}
		while ((transfer = transfer->next) != hostsrv_common.active_transfers);
	}

	ehci_resetPort();

	device->address = 0;
	hostsrv_setAddress(device, 1 + idtree_id(&device->linkage));
	TRACE("reset: address is set");
	device->address = 1 + idtree_id(&device->linkage);
	ehci_qhSetAddress(device->control_endpoint->qh, device->address);
}


void hostsrv_resetThread(void *arg)
{
	mutexLock(hostsrv_common.common_lock);

	for (;;) {
		condWait(hostsrv_common.reset_cond, hostsrv_common.common_lock, 0);

		if (hostsrv_common.reset_device != NULL) {
			hostsrv_resetDevice(hostsrv_common.reset_device);
			hostsrv_common.reset_device = NULL;
		}
	}
}


void hostsrv_eventCallback(int port_change)
{
	FUN_TRACE;
	usb_transfer_t *transfer;
	int error;

	if ((transfer = hostsrv_common.active_transfers) != NULL) {
		do {
			if (!transfer->finished && (error = hostsrv_finished(transfer))) {
				TRACE("transfer finished %x", transfer->id);
				transfer->finished = error;

				if (transfer->async)
					LIST_ADD_EX(&hostsrv_common.finished_transfers, transfer, finished_next, finished_prev);

				ehci_continue(transfer->endpoint->qh, transfer->qtds->prev->qtd);
				condBroadcast(transfer->cond);
			}
			transfer = transfer->next;
		}
		while (transfer != hostsrv_common.active_transfers);
	}

	if (port_change) {
		TRACE("port change");
		condSignal(hostsrv_common.port_cond);
	}

	TRACE("callback out");
}


int hostsrv_countBytes(usb_transfer_t *transfer)
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


void hostsrv_signalDetach(usb_device_t *device)
{
	FUN_TRACE;

	usb_driver_t *driver;
	usb_event_t *event;
	msg_t msg = { 0 };

	if ((driver = device->driver) == NULL) {
		TRACE("detach: no driver!");
		return;
	}

	msg.type = mtRaw;

	event = (void *)msg.i.raw;
	event->type = usb_event_removal;
	event->device_id = idtree_id(&device->linkage);

	msgSend(device->driver->port, &msg);
}


void hostsrv_signalDriver(usb_transfer_t *transfer)
{
	FUN_TRACE;

	usb_driver_t *driver;
	usb_event_t *event;
	msg_t msg = { 0 };

	if ((driver = transfer->endpoint->device->driver) == NULL) {
		TRACE("no driver!");
		return;
	}

	msg.type = mtRaw;

	event = (void *)msg.i.raw;
	event->type = usb_event_completion;
	event->completion.transfer_id = transfer->id;
	event->completion.pipe = idtree_id(&transfer->endpoint->linkage);

	if (transfer->aborted)
		event->completion.error = 1;
	else if (transfer->finished < 0)
		event->completion.error = -EIO;
	else
		event->completion.error = EOK;

	if (transfer->direction == usb_transfer_in) {
		msg.i.size = hostsrv_countBytes(transfer);
		msg.i.data = transfer->transfer_buffer;
	}

	TRACE("signalling");
	msgSend(transfer->endpoint->device->driver->port, &msg);
	TRACE("signalling out");
}


void hostsrv_signalThread(void *arg)
{
	usb_transfer_t *transfer;

	mutexLock(hostsrv_common.common_lock);

	for (;;) {
		while ((transfer = hostsrv_common.finished_transfers) == NULL)
			condWait(hostsrv_common.async_cond, hostsrv_common.common_lock, 0);

		LIST_REMOVE(&hostsrv_common.active_transfers, transfer);
		LIST_REMOVE_EX(&hostsrv_common.finished_transfers, transfer, finished_next, finished_prev);

		mutexUnlock(hostsrv_common.common_lock);
		hostsrv_signalDriver(transfer);
		mutexLock(hostsrv_common.common_lock);

		munmap(transfer->transfer_buffer, (transfer->transfer_size + _PAGE_SIZE - 1) & ~(_PAGE_SIZE - 1));
		hostsrv_deleteTransfer(transfer);
	}
}


int hostsrv_control(usb_device_t *device, int direction, usb_setup_packet_t *setup, void *buffer, int size)
{
	usb_urb_t urb = (usb_urb_t) {
		.type = usb_transfer_control,
		.direction = direction,
		.device_id = idtree_id(&device->linkage),
		.pipe = 0,
		.transfer_size = size,
		.async = 0,
		.setup = *setup,
	};

	return hostsrv_handleUrb(&urb, device->driver, device, device->control_endpoint, buffer);
}


int hostsrv_setAddress(usb_device_t *dev, unsigned char address)
{
	FUN_TRACE;

	usb_setup_packet_t setup = (usb_setup_packet_t) {
		.bmRequestType = REQUEST_DIR_HOST2DEV | REQUEST_TYPE_STANDARD | REQUEST_RECIPIENT_DEVICE,
		.bRequest = REQ_SET_ADDRESS,
		.wValue = address,
		.wIndex = 0,
		.wLength = 0,
	};

	return hostsrv_control(dev, usb_transfer_out, &setup, NULL, 0);
}


int hostsrv_getDescriptor(usb_device_t *dev, int descriptor, int index, char *buffer, int size)
{
	FUN_TRACE;

	usb_setup_packet_t setup = (usb_setup_packet_t) {
		.bmRequestType = REQUEST_DIR_DEV2HOST | REQUEST_TYPE_STANDARD | REQUEST_RECIPIENT_DEVICE,
		.bRequest = REQ_GET_DESCRIPTOR,
		.wValue = descriptor << 8 | index,
		.wIndex = 0,
		.wLength = size,
	};

	return hostsrv_control(dev, usb_transfer_in, &setup, buffer, size);
}


int hostsrv_getConfigurationDescriptor(usb_device_t *dev, usb_configuration_desc_t *desc, int index, int length)
{
	return hostsrv_getDescriptor(dev, USB_DESC_CONFIG, index, (char *)desc, length);
}


int hostsrv_getDeviceDescriptor(usb_device_t *dev, usb_device_desc_t *desc)
{
	return hostsrv_getDescriptor(dev, USB_DESC_DEVICE, 0, (char *)desc, sizeof(*desc));
}


int hostsrv_driverMatch1(usb_driver_t *driver, usb_device_t *device)
{
	usb_device_id_t *filter = &driver->filter;
	usb_device_desc_t *descriptor = device->descriptor;

	return (filter->idVendor == USB_CONNECT_WILDCARD || filter->idVendor == descriptor->idVendor) &&
		(filter->idProduct == USB_CONNECT_WILDCARD || filter->idProduct == descriptor->idProduct) &&
		(filter->bcdDevice == USB_CONNECT_WILDCARD || filter->bcdDevice == descriptor->bcdDevice);
}


int hostsrv_driverMatch2(usb_driver_t *driver, usb_device_t *device)
{
	usb_device_id_t *filter = &driver->filter;
	usb_device_desc_t *descriptor = device->descriptor;

	return (filter->idVendor == USB_CONNECT_WILDCARD || filter->idVendor == descriptor->idVendor) &&
		(filter->idProduct == USB_CONNECT_WILDCARD || filter->idProduct == descriptor->idProduct);
}


int hostsrv_driverMatch3(usb_driver_t *driver, usb_device_t *device)
{
	usb_device_id_t *filter = &driver->filter;
	usb_device_desc_t *descriptor = device->descriptor;

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


int hostsrv_driverMatch4(usb_driver_t *driver, usb_device_t *device)
{
	usb_device_id_t *filter = &driver->filter;
	usb_device_desc_t *descriptor = device->descriptor;

	if (descriptor->bDeviceClass == 0xff) {
		return (filter->idVendor == USB_CONNECT_WILDCARD || filter->idVendor == descriptor->idVendor) &&
			(filter->bDeviceSubClass == USB_CONNECT_WILDCARD || filter->bDeviceSubClass == descriptor->bDeviceSubClass);
	}
	else {
		return (filter->bDeviceClass == USB_CONNECT_WILDCARD || filter->bDeviceClass == descriptor->bDeviceClass) &&
			(filter->bDeviceSubClass == USB_CONNECT_WILDCARD || filter->bDeviceSubClass == descriptor->bDeviceSubClass);
	}
}


usb_driver_t *hostsrv_findDriver(usb_device_t *device)
{
	FUN_TRACE;

	const int (*usb_driverMatch[])(usb_driver_t *, usb_device_t *) = {
		hostsrv_driverMatch1, hostsrv_driverMatch2, hostsrv_driverMatch3, hostsrv_driverMatch4
	};

	rbnode_t *node;
	usb_driver_t *driver;
	int i;

	for (i = 0; i < 4; ++i) {
		for (node = lib_rbMinimum(hostsrv_common.drivers.root); node != NULL; node = lib_rbNext(node)) {
			driver = lib_treeof(usb_driver_t, linkage, node);
			if (usb_driverMatch[i](driver, device)) {
				TRACE("found driver");
				return driver;
			}
		}
	}
	return NULL;
}


int hostsrv_connectDriver(usb_driver_t *driver, usb_device_t *device, usb_configuration_desc_t *configuration)
{
	FUN_TRACE;

	msg_t msg;
	usb_event_t *event = (void *)msg.i.raw;
	usb_insertion_t *insertion = &event->insertion;

	memset(&msg, 0, sizeof(msg));
	msg.type = mtRaw;
	msg.i.data = configuration;
	msg.i.size = configuration->wTotalLength;

	event->type = usb_event_insertion;
	event->device_id = idtree_id(&device->linkage);
	memcpy(&insertion->descriptor, device->descriptor, sizeof(usb_device_desc_t));

	return msgSend(driver->port, &msg);
}


usb_endpoint_t *hostsrv_findPipe(usb_device_t *device, int pipe)
{
	return lib_treeof(usb_endpoint_t, linkage, idtree_find(&device->pipes, pipe));
}


int hostsrv_openPipe(usb_device_t *device, usb_endpoint_desc_t *descriptor)
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


int hostsrv_getConfiguration(usb_device_t *device, void *buffer, size_t bufsz)
{
	FUN_TRACE;

	usb_configuration_desc_t *conf = dma_alloc64();

	hostsrv_getConfigurationDescriptor(device, conf, 0, sizeof(usb_configuration_desc_t));

	if (bufsz < conf->wTotalLength)
		return -ENOBUFS;

	hostsrv_getConfigurationDescriptor(device, buffer, 0, conf->wTotalLength);

	dma_free64(conf);
	return EOK;
}


void hostsrv_dumpDeviceDescriptor(FILE *stream, usb_device_desc_t *descr)
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


int hostsrv_deviceAttach(void)
{
	FUN_TRACE;

	usb_device_t *dev;
	usb_endpoint_t *ep;
	usb_driver_t *driver;
	void *configuration;
	usb_device_desc_t *ddesc = dma_alloc64();

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
	if (hostsrv_getDeviceDescriptor(dev, ddesc) < 0) {
		TRACE_FAIL("getting device descriptor");
		free(dev);
		free(ep);
		dma_free64(ddesc);
		ehci_resetPort();
		return -EIO;
	}

	ehci_resetPort();

	dev->descriptor = ddesc;
	ep->max_packet_len = ddesc->bMaxPacketSize0;

	if (0) {
		hostsrv_dumpDeviceDescriptor(stderr, ddesc);
	}

	TRACE("setting address");
	idtree_alloc(&hostsrv_common.devices, &dev->linkage);

	hostsrv_setAddress(dev, 1 + idtree_id(&dev->linkage));
	dev->address = 1 + idtree_id(&dev->linkage);
	ehci_qhSetAddress(dev->control_endpoint->qh, dev->address);

	if ((driver = hostsrv_findDriver(dev)) != NULL) {
		TRACE("got driver");
		configuration = mmap(NULL, _PAGE_SIZE, PROT_WRITE | PROT_READ, MAP_ANONYMOUS | MAP_UNCACHED, -1, 0);
		hostsrv_getConfiguration(dev, configuration, _PAGE_SIZE);
		if (hostsrv_connectDriver(driver, dev, configuration) < 0) {
			LIST_ADD(&hostsrv_common.orphan_devices, dev);
			dev->driver = NULL;
		}
		else {
			LIST_ADD(&driver->devices, dev);
			dev->driver = driver;
		}
		munmap(configuration, _PAGE_SIZE);
	}
	else {
		TRACE("no driver");
		LIST_ADD(&hostsrv_common.orphan_devices, dev);
	}

	return EOK;
}


void hostsrv_deviceDetach(void)
{
	FUN_TRACE;
	usb_device_t *device = lib_treeof(usb_device_t, linkage, hostsrv_common.devices.root);

	if (device != NULL) {
		TRACE_FAIL("device detached");
		idtree_remove(&hostsrv_common.devices, &device->linkage);

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
			hostsrv_signalDetach(device);
			device->driver = NULL;
		}
		else {
			LIST_REMOVE(&hostsrv_common.orphan_devices, device);
		}

		free(device);
	}
}


void hostsrv_portthr(void *arg)
{
	int attached = 0;

	mutexLock(hostsrv_common.common_lock);

	for (;;) {
		condWait(hostsrv_common.port_cond, hostsrv_common.common_lock, 0);
		FUN_TRACE;

		if (ehci_deviceAttached()) {
			if (attached) {
				TRACE_FAIL("double attach");
			}
			else {
				if (hostsrv_deviceAttach() == EOK)
					attached = 1;
			}
		}
		else {
			if (!attached) {
				TRACE_FAIL("double detach");
			}
			else {
				hostsrv_deviceDetach();
				attached = 0;
			}
		}
	}
}


int hostsrv_connect(usb_connect_t *c, unsigned pid)
{
	FUN_TRACE;

	const int (*usb_driverMatch[])(usb_driver_t *, usb_device_t *) = {
		hostsrv_driverMatch1, hostsrv_driverMatch2, hostsrv_driverMatch3, hostsrv_driverMatch4
	};

	int i;
	usb_driver_t *driver = malloc(sizeof(*driver));
	usb_device_t *device;
	void *configuration;

	if (driver == NULL)
		return -ENOMEM;

	driver->port = portGet(c->port);
	driver->filter = c->filter;
	driver->pid = pid;
	driver->devices = NULL;

	lib_rbInsert(&hostsrv_common.drivers, &driver->linkage);

	if (hostsrv_common.orphan_devices != NULL) {
		configuration = mmap(NULL, _PAGE_SIZE, PROT_WRITE | PROT_READ, MAP_ANONYMOUS | MAP_UNCACHED, -1, 0);

		if (configuration == MAP_FAILED)
			return -ENOMEM;

		for (i = 0; i < 4; ++i) {
			device = hostsrv_common.orphan_devices;

			do {
				while (device != NULL && usb_driverMatch[i](driver, device)) {
					hostsrv_getConfiguration(device, configuration, _PAGE_SIZE);
					hostsrv_connectDriver(driver, device, configuration);
					device->driver = driver;

					LIST_REMOVE(&hostsrv_common.orphan_devices, device);
					LIST_ADD(&driver->devices, device);

					device = hostsrv_common.orphan_devices;
				}
			}
			while (device != NULL && (device = device->next) != hostsrv_common.orphan_devices);
		}

		munmap(configuration, _PAGE_SIZE);
	}

	telit = pid;

	return EOK;
}


int hostsrv_submitReset(int device_id)
{
	usb_device_t *device;

	if ((device = lib_treeof(usb_device_t, linkage, idtree_find(&hostsrv_common.devices, device_id))) == NULL)
		return -EINVAL;

	hostsrv_resetDevice(device);
	return EOK;
}


int hostsrv_open(usb_open_t *o, msg_t *msg)
{
	FUN_TRACE;
	usb_device_t *device;

	if ((device = lib_treeof(usb_device_t, linkage, idtree_find(&hostsrv_common.devices, o->device_id))) == NULL)
		return -EINVAL;

	return hostsrv_openPipe(device, &o->endpoint);
}


void msgthr(void *arg)
{
	unsigned port = (int)arg;
	unsigned rid;
	msg_t msg;
	usb_msg_t *umsg;
	int error;

	for (;;) {
		if (msgRecv(port, &msg, &rid) < 0)
			continue;

		mutexLock(hostsrv_common.common_lock);
		if (msg.type == mtRaw) {
			umsg = (void *)msg.i.raw;

			error = EOK;
			switch (umsg->type) {
			case usb_msg_connect:
				msg.o.io = hostsrv_connect(&umsg->connect, msg.pid);
				break;
			case usb_msg_urb:
				msg.o.io = hostsrv_submitUrb(msg.pid, &umsg->urb, msg.i.data, msg.o.data);
				break;
			case usb_msg_open:
				msg.o.io = hostsrv_open(&umsg->open, &msg);
				break;
			case usb_msg_reset:
				msg.o.io = hostsrv_submitReset(umsg->reset.device_id);
				break;
			default:
				TRACE_FAIL("unsupported usb_msg type");
				error = -EINVAL;
				break;
			}
		}
		else {
			TRACE_FAIL("unsupported msg type");
			error = -ENOTSUP;
		}
		mutexUnlock(hostsrv_common.common_lock);

		msgRespond(port, error, &msg, rid);
	}
}


int hostsrv_driverCmp(rbnode_t *n1, rbnode_t *n2)
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
	FUN_TRACE;

	if ((hostsrv_common.port = portCreate(365)) < 0) {
		debug("usb host: could not create port\n");
		exit(-1);
	}

	create_dev(hostsrv_common.port, 0, "/dev/usb", S_IFCHR | 0640);

	if (fork())
		_exit(0);
	setsid();

	mutexCreate(&hostsrv_common.common_lock);
	condCreate(&hostsrv_common.port_cond);
	condCreate(&hostsrv_common.async_cond);
	condCreate(&hostsrv_common.reset_cond);

	openlog("hostsrv", LOG_CONS, LOG_DAEMON);

	hostsrv_common.active_transfers = NULL;
	hostsrv_common.finished_transfers = NULL;
	hostsrv_common.orphan_devices = NULL;
	hostsrv_common.reset_device = NULL;
	lib_rbInit(&hostsrv_common.drivers, hostsrv_driverCmp, NULL);
	idtree_init(&hostsrv_common.devices);

	ehci_init(hostsrv_eventCallback, hostsrv_common.common_lock);


	beginthread(hostsrv_portthr, 4, malloc(0x4000), 0x4000, NULL);
	beginthread(hostsrv_signalThread, 4, malloc(0x4000), 0x4000, NULL);
	beginthread(hostsrv_resetThread, 4, malloc(0x4000), 0x4000, NULL);

	beginthread(msgthr, 4, malloc(0x4000), 0x4000, (void *)hostsrv_common.port);
	beginthread(msgthr, 4, malloc(0x4000), 0x4000, (void *)hostsrv_common.port);
	beginthread(msgthr, 4, malloc(0x4000), 0x4000, (void *)hostsrv_common.port);

	printf("hostsrv: initialized\n");
	msgthr((void *)hostsrv_common.port);
	return 0;
}

