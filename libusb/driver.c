/*
 * Phoenix-RTOS
 *
 * libusb/driver.c
 *
 * Copyright 2021 Phoenix Systems
 * Author: Maciej Purski
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */

#include <errno.h>
#include <usbdriver.h>
#include <sys/msg.h>
#include <sys/mman.h>
#include <sys/list.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include "drv_msg.h"

#define LIBUSB_MAX_EVENTS 8

static struct {
	char stack[2048] __attribute__((aligned(8)));
	const usbdrv_handlers_t *handlers;
	unsigned srvport;
	unsigned drvport;
	handle_t lock;
	rbtree_t urbstree;
	idtree_t devstree;
	usbdrv_dev_t *devslist;
} usbdrv_common;


static void usbdrv_freeMsg(addr_t physaddr, size_t size)
{
	msg_t msg = { 0 };
	usbdrv_in_msg_t *imsg = (usbdrv_in_msg_t *)msg.i.raw;

	msg.type = mtDevCtl;
	imsg->type = usbdrv_msg_free;
	imsg->free.physaddr = physaddr;
	imsg->free.size = size;
	msgSend(usbdrv_common.srvport, &msg);
}


void usbdrv_free(void *ptr, size_t size)
{
	usbdrv_freeMsg(va2pa(ptr), size);

	/* size should be a multiple of a page size */
	size = (size + (_PAGE_SIZE - 1)) & ~(_PAGE_SIZE - 1);

	/* address must be aligned to a page size */
	ptr = (void *)((addr_t)ptr & ~(_PAGE_SIZE - 1));

	munmap(ptr, size);
}


void *usbdrv_alloc(size_t size)
{
	msg_t msg = { 0 };
	usbdrv_in_msg_t *imsg = (usbdrv_in_msg_t *)msg.i.raw;
	usbdrv_out_msg_t *omsg = (usbdrv_out_msg_t *)msg.o.raw;
	addr_t physaddr, offset;
	void *vaddr;
	int ret;

	msg.type = mtDevCtl;
	imsg->type = usbdrv_msg_alloc;
	imsg->alloc.size = size;

	if ((ret = msgSend(usbdrv_common.srvport, &msg)) != 0) {
		return NULL;
	}

	if (omsg->err != 0) {
		return NULL;
	}

	physaddr = omsg->alloc.physaddr;
	offset = physaddr % _PAGE_SIZE;

	/* size should be a multiple of a page size */
	size = (size + (_PAGE_SIZE - 1)) & ~(_PAGE_SIZE - 1);

	/* physical address must be aligned to a page size */
	physaddr -= offset;

	/* TODO: Check if we have already mapped this page */
	vaddr = mmap(NULL, size, PROT_WRITE | PROT_READ, MAP_UNCACHED, OID_PHYSMEM, physaddr);
	if (vaddr == MAP_FAILED) {
		usbdrv_freeMsg(omsg->alloc.physaddr, size);
		return NULL;
	}

	vaddr = (void *)((addr_t)vaddr + offset);

	return vaddr;
}


static void usbdrv_pipeCloseMsg(int pipeid)
{

}


void usbdrv_pipeClose(usbdrv_pipe_t *pipe)
{
	free(pipe);
	/* TODO: Send close message */
}


usbdrv_pipe_t *usbdrv_pipeOpen(usbdrv_dev_t *dev, usb_transfer_type_t type, usb_dir_t dir)
{
	msg_t msg = { 0 };
	usbdrv_in_msg_t *imsg = (usbdrv_in_msg_t *)msg.i.raw;
	usbdrv_out_msg_t *omsg = (usbdrv_out_msg_t *)msg.o.raw;
	usbdrv_pipe_t *pipe;
	int ret;

	msg.type = mtDevCtl;
	imsg->type = usbdrv_msg_open;

	imsg->open.bus = dev->bus;
	imsg->open.dev = dev->dev;
	imsg->open.iface = dev->interface;
	imsg->open.type = type;
	imsg->open.dir = dir;
	imsg->open.locationID = dev->locationID;

	ret = msgSend(usbdrv_common.srvport, &msg);
	if (ret != 0 || omsg->err < 0) {
		return NULL;
	}

	pipe = malloc(sizeof(usbdrv_pipe_t));
	if (pipe == NULL) {
		usbdrv_pipeCloseMsg(omsg->open.id);
		return NULL;
	}

	pipe->bus = dev->bus;
	pipe->dev = dev->dev;
	pipe->iface = dev->interface;
	pipe->locationID = dev->locationID;
	pipe->id = omsg->open.id;
	pipe->type = type;
	pipe->dir = dir;

	return pipe;
}


static int usbdrv_urbSubmitSync(usbdrv_in_submit_t *submit, void *data)
{
	msg_t msg = { 0 };
	usbdrv_in_msg_t *umsg = (usbdrv_in_msg_t *)msg.i.raw;
	int ret;

	msg.type = mtDevCtl;
	umsg->type = usbdrv_msg_submit;

	if ((ret = msgSend(usbdrv_common.srvport, &msg)) != 0)
		return ret;

	return msg.o.io.err;
}


ssize_t usbdrv_transferControl(usbdrv_pipe_t *pipe, usb_setup_packet_t *setup, void *data, unsigned int timeout)
{
	msg_t msg = { 0 };
	usbdrv_in_msg_t *imsg = (usbdrv_in_msg_t *)msg.i.raw;
	usbdrv_out_msg_t *omsg = (usbdrv_out_msg_t *)msg.o.raw;
	usbdrv_in_submit_t *insubmit = &imsg->submit;
	usbdrv_out_submit_t *outsubmit = &omsg->submit;
	int ret;

	if (pipe->type != usb_transfer_control) {
		return -EINVAL;
	}

	msg.type = mtDevCtl;
	imsg->type = usbdrv_msg_submit;

	memcpy(&insubmit->setup, setup, sizeof(usb_setup_packet_t));

	insubmit->pipeid = pipe->id;
	insubmit->timeout = timeout;
	insubmit->size = setup->wLength;
	insubmit->type = usb_transfer_control;
	insubmit->size = setup->wLength;

	if ((setup->bmRequestType & REQUEST_DIR_HOST2DEV) != 0) {
		insubmit->dir = usb_dir_in;
	}
	else {
		insubmit->dir = usb_dir_out;
	}

	/* data stage in control transfer is optional */
	if (data != NULL) {
		insubmit->physaddr = va2pa(data);
	}

	ret = msgSend(usbdrv_common.srvport, &msg);
	if (ret < 0) {
		return ret;
	}

	if (omsg->err != 0) {
		return omsg->err;
	}
	else {
		return outsubmit->transferred;
	}
}


ssize_t usbdrv_transferBulk(usbdrv_pipe_t *pipe, void *data, size_t size, unsigned int timeout)
{
	msg_t msg = { 0 };
	usbdrv_in_msg_t *imsg = (usbdrv_in_msg_t *)msg.i.raw;
	usbdrv_out_msg_t *omsg = (usbdrv_out_msg_t *)msg.o.raw;
	usbdrv_in_submit_t *insubmit = &imsg->submit;
	usbdrv_out_submit_t *outsubmit = &omsg->submit;
	int ret;

	if (pipe->type != usb_transfer_bulk) {
		printf("PIPE type wrong\n");
		return -EINVAL;
	}

	msg.type = mtDevCtl;
	imsg->type = usbdrv_msg_submit;

	insubmit->pipeid = pipe->id;
	insubmit->timeout = timeout;
	insubmit->size = size;
	insubmit->physaddr = va2pa(data);
	insubmit->type = usb_transfer_bulk;
	insubmit->dir = pipe->dir;

	ret = msgSend(usbdrv_common.srvport, &msg);
	if (ret < 0) {
		return ret;
	}

	if (omsg->err != 0) {
		return omsg->err;
	}
	else {
		return outsubmit->transferred;
	}
}


int usbdrv_setConfiguration(usbdrv_pipe_t *pipe, int conf)
{
	usb_setup_packet_t setup = (usb_setup_packet_t) {
		.bmRequestType = REQUEST_DIR_HOST2DEV | REQUEST_TYPE_STANDARD | REQUEST_RECIPIENT_DEVICE,
		.bRequest = REQ_SET_CONFIGURATION,
		.wValue = conf,
		.wIndex = 0,
		.wLength = 0,
	};

	return usbdrv_transferControl(pipe, &setup, NULL, 1000);
}


int usbdrv_clearFeatureHalt(usbdrv_pipe_t *pipe)
{
	usb_setup_packet_t setup = (usb_setup_packet_t) {
		.bmRequestType = REQUEST_DIR_HOST2DEV | REQUEST_TYPE_STANDARD | REQUEST_RECIPIENT_DEVICE,
		.bRequest = REQ_CLEAR_FEATURE,
		.wValue = USB_ENDPOINT_HALT,
		.wIndex = pipe->epnum,
		.wLength = 0,
	};

	return usbdrv_transferControl(pipe, &setup, NULL, 1000);
}


usbdrv_urb_t *usbdrv_urbAlloc(usbdrv_pipe_t *pipe)
{
	msg_t msg = { 0 };
	usbdrv_in_msg_t *inmsg = (usbdrv_in_msg_t *)msg.i.raw;
	usbdrv_out_msg_t *outmsg = (usbdrv_out_msg_t *)msg.o.raw;
	usbdrv_in_urballoc_t *inalloc = &inmsg->urballoc;
	usbdrv_out_urballoc_t *outalloc = &outmsg->urballoc;
	usbdrv_urb_t *urb;
	int ret;

	urb = malloc(sizeof(usbdrv_urb_t));
	if (urb == NULL) {
		return -ENOMEM;
	}

	msg.type = mtDevCtl;
	inmsg->type = usbdrv_msg_urballoc;

	inalloc->pipeid = pipe->id;
	inalloc->type = pipe->type;
	inalloc->dir = pipe->dir;

	ret = msgSend(usbdrv_common.srvport, &msg);
	if (ret != 0) {
		return ret;
	}

	if (outmsg->err != 0) {
		free(urb);
		return outmsg->err;
	}

	urb->id = outalloc->urbid;
	urb->pipeid = pipe->id;
	lib_rbInsert(&usbdrv_common.urbstree, urb);

	return urb;
}


int usbdrv_transferBulkAsync(usbdrv_urb_t *urb, void *data, size_t size, unsigned int timeout)
{
	msg_t msg = { 0 };
	usbdrv_in_msg_t *umsg = (usbdrv_in_msg_t *)msg.i.raw;
	int ret;
	usbdrv_in_urbcmd_t *urbcmd = &umsg->urbcmd;

	urbcmd->pipeid = urb->pipeid;
	urbcmd->urbid = urb->id;
	urbcmd->cmd = urbcmd_submit;
	urbcmd->physaddr = va2pa(data);
	urbcmd->size = size;

	msg.type = mtDevCtl;
	umsg->type = usbdrv_msg_urbcmd;

	ret = msgSend(usbdrv_common.srvport, &msg);
	if (ret < 0) {
		return ret;
	}

	return 0;
}


int usbdrv_transferControlAsync(usbdrv_urb_t *urb, usb_setup_packet_t *setup, void *data, size_t size, unsigned int timeout)
{
	msg_t msg = { 0 };
	usbdrv_in_msg_t *umsg = (usbdrv_in_msg_t *)msg.i.raw;
	int ret;
	usbdrv_in_urbcmd_t *urbcmd = &umsg->urbcmd;

	urbcmd->pipeid = urb->pipeid;
	urbcmd->urbid = urb->id;
	urbcmd->cmd = urbcmd_submit;
	urbcmd->physaddr = va2pa(data);
	urbcmd->size = size;
	memcpy(&urbcmd->setup, setup, sizeof(usb_setup_packet_t));

	msg.type = mtDevCtl;
	umsg->type = usbdrv_msg_urbcmd;

	ret = msgSend(usbdrv_common.srvport, &msg);
	if (ret < 0) {
		return ret;
	}

	return 0;
}


int usbdrv_urbFree(usbdrv_urb_t *urb)
{
	msg_t msg = { 0 };
	usbdrv_in_msg_t *umsg = (usbdrv_in_msg_t *)msg.i.raw;
	int ret;
	usbdrv_in_urbcmd_t *urbcmd = &umsg->urbcmd;

	urbcmd->pipeid = urb->pipeid;
	urbcmd->urbid = urb->id;
	urbcmd->cmd = urbcmd_free;

	msg.type = mtDevCtl;
	umsg->type = usbdrv_msg_urbcmd;

	msgSend(usbdrv_common.srvport, &msg);

	lib_rbRemove(&usbdrv_common.urbstree, urb);
	free(urb);

	return 0;
}


static void usbdrv_handleInsertion(usbdrv_event_t *event)
{
	usbdrv_dev_t *dev;
	oid_t oid;

	dev = malloc(sizeof(usbdrv_dev_t));
	if (dev == NULL) {
		fprintf(stderr, "libusb: Out of memory!\n");
		return;
	}

	dev->bus = event->bus;
	dev->dev = event->dev;
	dev->interface = event->interface;
	dev->locationID = event->locationID;
	dev->refcnt = 1;

	mutexLock(usbdrv_common.lock);
	if (idtree_alloc(&usbdrv_common.devstree, &dev->node) < 0) {
		free(dev);
		return;
	}
	LIST_ADD(&usbdrv_common.devslist, dev);

	if (usbdrv_common.handlers->insertion(dev) != 0) {
		idtree_remove(&usbdrv_common.devstree, &dev->node);
		LIST_REMOVE(&usbdrv_common.devslist, dev);
		free(dev);
	}
	mutexUnlock(usbdrv_common.lock);
}


static usbdrv_dev_t *_usbdrv_devFind(int locationID, int interface)
{
	usbdrv_dev_t *ret = NULL, *tmp = usbdrv_common.devslist;

	do {
		if (tmp->locationID == locationID && tmp->interface == interface) {
			ret = tmp;
			break;
		}

		tmp = tmp->next;
	} while (tmp != usbdrv_common.devslist);
}


static usbdrv_dev_t *usbdrv_devFind(int locationID, int interface)
{
	usbdrv_dev_t *ret;

	mutexLock(usbdrv_common.lock);
	ret = _usbdrv_devFind(locationID, interface);
	if (ret != NULL) {
		ret->refcnt++;
	}
	mutexUnlock(usbdrv_common.lock);

	return ret;
}


static usbdrv_urb_t *usbdrv_urbFind(int urbid)
{
	usbdrv_urb_t find;

	find.id = urbid;

	return lib_treeof(usbdrv_urb_t, node, lib_rbFind(&usbdrv_common.devstree, &find.node));
}


usbdrv_dev_t *usbdrv_devGet(int id)
{
	usbdrv_dev_t *dev;

	mutexLock(usbdrv_common.lock);
	dev = lib_treeof(usbdrv_dev_t, node, idtree_find(&usbdrv_common.devstree, id));
	if (dev != NULL) {
		dev->refcnt++;
	}
	mutexUnlock(usbdrv_common.lock);

	return dev;
}


static void _usbdrv_devPut(usbdrv_dev_t *dev)
{
	dev->refcnt--;
	if (dev->refcnt == 0) {
		usbdrv_common.handlers->deletion(dev);
		free(dev);
	}
}


void usbdrv_devPut(usbdrv_dev_t *dev)
{
	mutexLock(usbdrv_common.lock);
	_usbdrv_devPut(dev);
	mutexUnlock(usbdrv_common.lock);
}


int usbdrv_devID(usbdrv_dev_t *dev)
{
	return dev->node.id;
}


static void usbdrv_handleDeletion(usbdrv_event_t *event)
{
	usbdrv_dev_t *dev;

	mutexLock(usbdrv_common.lock);
	dev = _usbdrv_devFind(event->locationID, event->interface);
	if (dev == NULL) {
		return;
	}

	idtree_remove(&usbdrv_common.devstree, &dev->node);
	LIST_REMOVE(&usbdrv_common.devslist, dev);
	_usbdrv_devPut(dev);
	mutexUnlock(usbdrv_common.lock);
}


static void usbdrv_handleCompletion(usbdrv_event_t *event)
{
	usbdrv_dev_t *dev;
	usbdrv_urb_t *urb;

	dev = usbdrv_devFind(event->locationID, event->interface);
	if (dev == NULL) {
		return;
	}

	urb = usbdrv_urbFind(event->urbid);
	if (urb == NULL) {
		return;
	}

	usbdrv_common.handlers->completion(dev, urb, urb->vaddr, event->transferred, event->status);
}


static void usbdrv_thread(void *arg)
{
	msg_t msg = { 0 };
	usbdrv_in_msg_t *inmsg = (usbdrv_in_msg_t *)msg.i.raw;
	usbdrv_out_msg_t *outmsg = (usbdrv_out_msg_t *)msg.o.raw;
	usbdrv_event_t *events;
	unsigned long rid;
	int err;
	int i;

	msg.type = mtDevctl;
	inmsg->type = usbdrv_msg_wait;
	inmsg->wait.maxevents = LIBUSB_MAX_EVENTS;

	events = malloc(sizeof(usbdrv_event_t) * LIBUSB_MAX_EVENTS);
	if (events == NULL) {
		fprintf(stderr, "libusb: Out of memory\n");
		return;
	}

	msg.o.data = events;
	msg.o.size = sizeof(usbdrv_event_t) * LIBUSB_MAX_EVENTS;

	for (;;) {
		if (msgSend(usbdrv_common.srvport, &msg) < 0) {
			return;
		}

		/* Handle a stream of events */
		for (i = 0; i < outmsg->wait.nevents; i++) {
			switch (events[i].type) {
				case usbdrv_insertion_event:
					usbdrv_handleInsertion(&events[i]);
					break;
				case usbdrv_deletion_event:
					usbdrv_handleDeletion(&events[i]);
					break;
				case usbdrv_completion_event:
					usbdrv_handleCompletion(&events[i]);
					break;
				default:
					fprintf(stderr, "libusb: Error when receiving event from host\n");
					break;
			}
		}
	}

}


static int usbdrv_urbcmp(rbnode_t *node1, rbnode_t *node2)
{
	usbdrv_urb_t *urb1 = lib_treeof(usbdrv_urb_t, node, node1);
	usbdrv_urb_t *urb2 = lib_treeof(usbdrv_urb_t, node, node2);

	if (urb1->id > urb2->id) {
		return 1;
	}
	else if (urb1->id < urb2->id) {
		return -1;
	}
	else {
		return 0;
	}
}


int usbdrv_connect(const usbdrv_handlers_t *handlers, const usbdrv_devid_t *filters, unsigned int nfilters)
{
	msg_t msg = { 0 };
	usbdrv_in_msg_t *umsg = (usbdrv_in_msg_t *)&msg.i.raw;
	oid_t oid;

	while (lookup("/dev/usb", NULL, &oid) < 0) {
		usleep(1000000);
	}

	msg.type = mtDevCtl;
	msg.i.size = sizeof(*filters) * nfilters;
	msg.i.data = (void *)filters;

	umsg->type = usbdrv_msg_connect;
	umsg->connect.port = usbdrv_common.drvport;
	umsg->connect.nfilters = nfilters;

	if (msgSend(oid.port, &msg) < 0) {
		return -1;
	}

	if (mutexCreate(&usbdrv_common.lock) != 0) {
		return -1;
	}

	idtree_init(&usbdrv_common.devstree);
	lib_rbInit(&usbdrv_common.devstree, usbdrv_urbcmp, NULL);

	if (beginthread(usbdrv_thread, 3, usbdrv_common.stack, sizeof(usbdrv_common.stack), NULL) != 0) {
		resourceDestroy(usbdrv_common.drvport);
		return -1;
	}

	usbdrv_common.srvport = oid.port;

	return 0;
}


const usbdrv_modeswitch_t *usbdrv_modeswitchFind(uint16_t vid, uint16_t pid, const usbdrv_modeswitch_t *modes, int nmodes)
{
	int i;

	for (i = 0; i < nmodes; i++) {
		if (vid == modes[i].vid && pid == modes[i].pid)
			return &modes[i];
	}

	return NULL;
}


void usbdrv_dumpDeviceDescriptor(FILE *stream, usb_device_desc_t *descr)
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


void usbdrv_dumpConfigurationDescriptor(FILE *stream, usb_configuration_desc_t *descr)
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





void usbdrv_dumpEndpointDesc(FILE *stream, usb_endpoint_desc_t *descr)
{
	fprintf(stream, "ENDPOINT DESCRIPTOR:\n");
	fprintf(stream, "\tbLength: %d\n", descr->bLength);
	fprintf(stream, "\tbDescriptorType: 0x%x\n", descr->bDescriptorType);
	fprintf(stream, "\tbEndpointAddress: %d\n", descr->bEndpointAddress);
	fprintf(stream, "\tbmAttributes: 0x%x\n", descr->bmAttributes);
	fprintf(stream, "\twMaxPacketSize: %d\n", descr->wMaxPacketSize);
	fprintf(stream, "\tbInterval: %d\n", descr->bInterval);
}


void usbdrv_dumpStringDesc(FILE *stream, usb_string_desc_t *descr)
{
	fprintf(stream, "STRING DESCRIPTOR:\n");
	fprintf(stream, "\tbLength: %d\n", descr->bLength);
	fprintf(stream, "\tbDescriptorType: 0x%x\n", descr->bDescriptorType);
	fprintf(stream, "\twData: %.*s\n", descr->bLength - 2, descr->wData);
}


int usbdrv_modeswitchHandle(usbdrv_dev_t *dev, const usbdrv_modeswitch_t *mode)
{
	char msg[sizeof(mode->msg)];
	usbdrv_pipe_t *pipeCtrl, *pipeIn, *pipeOut;

	if ((pipeCtrl = usbdrv_pipeOpen(dev, usb_transfer_control, 0)) < 0)
		return -EINVAL;

	if (usbdrv_setConfiguration(pipeCtrl, 1) != 0)
		return -EINVAL;

	if ((pipeIn = usbdrv_pipeOpen(dev, usb_transfer_bulk, usb_dir_in)) < 0)
		return -EINVAL;

	if ((pipeOut = usbdrv_pipeOpen(dev, usb_transfer_bulk, usb_dir_out)) < 0)
		return -EINVAL;

	memcpy(msg, mode->msg, sizeof(msg));
	if (usbdrv_transferBulk(pipeOut, msg, sizeof(msg), 1000) < 0)
		return -EINVAL;

	return 0;
}
