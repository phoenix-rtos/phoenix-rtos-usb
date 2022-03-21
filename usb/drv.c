/*
 * Phoenix-RTOS
 *
 * USB Host Driver
 *
 * Copyright 2021 Phoenix Systems
 * Author: Maciej Purski
 *
 * This file is part of Phoenix-RTOS.
 *
 * %LICENSE%
 */


#include <errno.h>
#include <sys/msg.h>
#include <sys/threads.h>
#include <sys/list.h>
#include <stdlib.h>
#include <posix/utils.h>
#include <string.h>

#include <usb.h>
#include <usbdriver.h>

#include "drv.h"
#include "hcd.h"

struct {
	handle_t lock;
	usb_drv_t *drvs;
} usbdrv_common;


void usb_drvPipeFree(usb_drv_t *drv, usb_pipe_t *pipe)
{
	mutexLock(usbdrv_common.lock);
	if (drv != NULL)
		idtree_remove(&drv->pipes, &pipe->linkage);

	pipe->dev->hcd->ops->pipeDestroy(pipe->dev->hcd, pipe);
	free(pipe);
	mutexUnlock(usbdrv_common.lock);
}


static usb_pipe_t *usb_drvPipeFind(usb_drv_t *drv, int pipe)
{
	usb_pipe_t *res;

	res = lib_treeof(usb_pipe_t, linkage, idtree_find(&drv->pipes, pipe));

	return res;
}


static int usb_pipeAdd(usb_drv_t *drv, usb_pipe_t *pipe)
{
	if (idtree_alloc(&drv->pipes, &pipe->linkage) < 0)
		return -1;

	return 0;
}


static usb_pipe_t *usb_pipeAlloc(usb_drv_t *drv, usb_dev_t *dev, usb_endpoint_desc_t *desc)
{
	usb_pipe_t *pipe;

	if ((pipe = malloc(sizeof(usb_pipe_t))) == NULL)
		return NULL;

	pipe->dev = dev;
	pipe->num = desc->bEndpointAddress & 0xF;
	pipe->dir = (desc->bEndpointAddress & 0x80) ? usb_dir_in : usb_dir_out;
	pipe->maxPacketLen = desc->wMaxPacketSize;
	pipe->type = desc->bmAttributes & 0x3;
	pipe->interval = desc->bInterval;
	pipe->hcdpriv = NULL;
	pipe->drv = drv;

	return pipe;
}


static usb_pipe_t *_usb_drvPipeOpen(usb_drv_t *drv, hcd_t *hcd, int locationID, int ifaceID, int dir, int type)
{
	usb_endpoint_desc_t *desc;
	usb_pipe_t *pipe = NULL;
	usb_dev_t *dev;
	usb_iface_t *iface;
	int i;

	if ((dev = usb_devFind(hcd->roothub, locationID)) == NULL) {
		USB_LOG("usb: Fail to find device\n");
		return NULL;
	}

	if (dev->nifs < ifaceID) {
		USB_LOG("usb: Fail to find iface\n");
		return NULL;
	}

	iface = &dev->ifs[ifaceID];

	/* Driver and interface mismatch */
	if (iface->driver != drv)
		return NULL;

	desc = iface->eps;
	if (type == usb_transfer_control) {
		if ((pipe = malloc(sizeof(usb_pipe_t))) == NULL)
			return NULL;
		memcpy(pipe, dev->ctrlPipe, sizeof(usb_pipe_t));
		pipe->hcdpriv = NULL;
	}
	else {
		/* Search interface descriptor for this endpoint */
		for (i = 0; i < iface->desc->bNumEndpoints; i++) {
			if ((desc[i].bmAttributes & 0x3) == type && (desc[i].bEndpointAddress >> 7) == dir) {
				if ((pipe = usb_pipeAlloc(drv, dev, &desc[i])) == NULL)
					return NULL;
			}
		}
	}

	if (pipe != NULL && drv != NULL) {
		if (usb_pipeAdd(drv, pipe) != 0) {
			free(pipe);
			return NULL;
		}
	}

	return pipe;
}


usb_pipe_t *usb_pipeOpen(usb_dev_t *dev, int iface, int dir, int type)
{
	usb_pipe_t *pipe = NULL;

	mutexLock(usbdrv_common.lock);
	pipe = _usb_drvPipeOpen(NULL, dev->hcd, dev->locationID, iface, dir, type);
	mutexUnlock(usbdrv_common.lock);

	return pipe;
}


int usb_drvPipeOpen(usb_drv_t *drv, hcd_t *hcd, int locationID, int iface, int dir, int type)
{
	usb_pipe_t *pipe = NULL;
	int pipeId = -1;

	mutexLock(usbdrv_common.lock);
	if ((pipe = _usb_drvPipeOpen(drv, hcd, locationID, iface, dir, type)) != NULL)
		pipeId = pipe->linkage.id;
	mutexUnlock(usbdrv_common.lock);

	return pipeId;
}


int usb_drvTransfer(usb_drv_t *drv, usb_transfer_t *t, int pipeId)
{
	usb_pipe_t *pipe;
	int ret = -1;

	mutexLock(usbdrv_common.lock);
	pipe = usb_drvPipeFind(drv, pipeId);
	if (pipe != NULL) {
		t->pipe = pipe;
		t->type = pipe->type;
		ret = usb_transferSubmit(t, 0);
	}
	mutexUnlock(usbdrv_common.lock);

	return ret;
}


static int usb_drvcmp(usb_device_desc_t *dev, usb_interface_desc_t *iface, usb_device_id_t *filter)
{
	int match = usbdrv_match;

	if (filter->dclass != USBDRV_ANY) {
		if ((dev->bDeviceClass != 0 && dev->bDeviceClass == filter->dclass) ||
				(dev->bDeviceClass == 0 && iface->bInterfaceClass == filter->dclass))
			match |= usbdrv_class_match;
		else
			return usbdrv_nomatch;
	}

	if (filter->subclass != USBDRV_ANY) {
		if ((dev->bDeviceSubClass != 0 && dev->bDeviceSubClass == filter->subclass) ||
				(dev->bDeviceSubClass == 0 && iface->bInterfaceSubClass == filter->subclass))
			match |= usbdrv_subclass_match;
		else
			return usbdrv_nomatch;
	}

	if (filter->protocol != USBDRV_ANY) {
		if ((dev->bDeviceProtocol != 0 && dev->bDeviceProtocol == filter->protocol) ||
				(dev->bDeviceProtocol == 0 && iface->bInterfaceProtocol == filter->protocol))
			match |= usbdrv_protocol_match;
		else
			return usbdrv_nomatch;
	}

	if (filter->vid != USBDRV_ANY) {
		if (dev->idVendor == filter->vid)
			match |= usbdrv_vid_match;
		else
			return usbdrv_nomatch;
	}

	if (filter->pid != USBDRV_ANY) {
		if (dev->idProduct == filter->pid)
			match |= usbdrv_pid_match;
		else
			return usbdrv_nomatch;
	}

	return match;
}


static usb_drv_t *usb_drvMatchIface(usb_dev_t *dev, usb_iface_t *iface)
{
	usb_drv_t *drv, *best = NULL;
	int i, match, bestmatch = 0;

	mutexLock(usbdrv_common.lock);
	drv = usbdrv_common.drvs;
	if (drv == NULL) {
		mutexUnlock(usbdrv_common.lock);
		return NULL;
	}

	do {
		for (i = 0; i < drv->nfilters; i++) {
			match = usb_drvcmp(&dev->desc, iface->desc, &drv->filters[i]);
			if (match > bestmatch) {
				bestmatch = match;
				best = drv;
			}
		}
	} while ((drv = drv->next) != usbdrv_common.drvs);
	mutexUnlock(usbdrv_common.lock);

	return best;
}


int usb_drvUnbind(usb_drv_t *drv, usb_dev_t *dev, int iface)
{
	msg_t msg = { 0 };
	usb_msg_t *umsg = (usb_msg_t *)msg.i.raw;
	usb_pipe_t *pipe;
	rbnode_t *n;

	mutexLock(usbdrv_common.lock);
	while ((n = lib_rbMinimum(drv->pipes.root)) != NULL) {
		pipe = lib_treeof(usb_pipe_t, linkage, n);
		if (pipe->dev == dev) {
			lib_rbRemove(&drv->pipes, n);
			dev->hcd->ops->pipeDestroy(dev->hcd, pipe);
			free(pipe);
		}
	}

	mutexUnlock(usbdrv_common.lock);
	msg.type = mtDevCtl;
	umsg->type = usb_msg_deletion;
	umsg->deletion.bus = dev->hcd->num;
	umsg->deletion.dev = dev->address;
	umsg->deletion.interface = iface;

	/* TODO: use non blocking version of msgSend */
	return msgSend(drv->port, &msg);
}


int usb_drvBind(usb_dev_t *dev)
{
	usb_drv_t *drv;
	msg_t msg = { 0 };
	usb_msg_t *umsg = (usb_msg_t *)msg.i.raw;
	int i;

	msg.type = mtDevCtl;
	umsg->type = usb_msg_insertion;
	umsg->insertion.bus = dev->hcd->num;
	umsg->insertion.dev = dev->address;
	umsg->insertion.descriptor = dev->desc;
	umsg->insertion.locationID = dev->locationID;

	for (i = 0; i < dev->nifs; i++) {
		if ((drv = usb_drvMatchIface(dev, &dev->ifs[i])) != NULL) {
			dev->ifs[i].driver = drv;
			umsg->insertion.interface = i;
			msgSend(drv->port, &msg);
		}
		/* TODO: Make a device orphaned */
	}

	return 0;
}


usb_drv_t *usb_drvFind(int pid)
{
	usb_drv_t *drv, *res = NULL;

	mutexLock(usbdrv_common.lock);
	drv = usbdrv_common.drvs;
	if (drv != NULL) {
		do {
			if (drv->pid == pid) {
				res = drv;
				break;
			}

			drv = drv->next;
		} while (drv != usbdrv_common.drvs);
	}
	mutexUnlock(usbdrv_common.lock);

	return res;
}


void usb_drvAdd(usb_drv_t *drv)
{
	mutexLock(usbdrv_common.lock);
	idtree_init(&drv->pipes);
	LIST_ADD(&usbdrv_common.drvs, drv);
	mutexUnlock(usbdrv_common.lock);
}


int usb_drvInit(void)
{
	if (mutexCreate(&usbdrv_common.lock) != 0) {
		USB_LOG("usbdrv: Can't create mutex!\n");
		return -ENOMEM;
	}

	return 0;
}
