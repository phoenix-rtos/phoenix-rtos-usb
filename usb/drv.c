/*
 * Phoenix-RTOS
 *
 * USB Host Driver
 *
 * Copyright 2021, 2022 Phoenix Systems
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


static usb_pipe_t *_usb_pipeFind(usb_drv_t *drv, int pipeid)
{
	return lib_treeof(usb_pipe_t, linkage, idtree_find(&drv->pipes, pipeid));
}


static usb_transfer_t *_usb_transferFind(usb_drv_t *drv, int id)
{
	usb_transfer_t *t;

	t = lib_treeof(usb_transfer_t, linkage, idtree_find(&drv->urbs, id));
	if (t != NULL)
		t->refcnt++;

	return t;
}


static void _usb_transferPut(usb_transfer_t *t)
{
	t->refcnt--;
	if (t->refcnt == 0)
		usb_transferFree(t);
}


void usb_transferPut(usb_transfer_t *t)
{
	mutexLock(usbdrv_common.lock);
	_usb_transferPut(t);
	mutexUnlock(usbdrv_common.lock);
}


static int _usb_pipeAdd(usb_drv_t *drv, usb_pipe_t *pipe)
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
		if (_usb_pipeAdd(drv, pipe) != 0) {
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


static int _usb_drvTransfer(usb_drv_t *drv, usb_transfer_t *t)
{
	usb_pipe_t *pipe;

	pipe = _usb_pipeFind(drv, t->pipeid);
	if (pipe == NULL)
		return -EINVAL;

	t->type = pipe->type;

	return usb_transferSubmit(t, pipe, NULL);
}


int usb_drvTransfer(usb_drv_t *drv, usb_transfer_t *t, int pipeId)
{
	int ret;

	mutexLock(usbdrv_common.lock);
	ret = _usb_drvTransfer(drv, t);
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


static int _usb_urbCancel(usb_transfer_t *t, usb_pipe_t *pipe)
{
	hcd_t *hcd = pipe->dev->hcd;

	hcd->ops->transferDequeue(hcd, t);

	return 0;
}


static int _usb_urbFree(usb_transfer_t *t, usb_drv_t *drv, usb_pipe_t *pipe)
{
	/* Remove from the drv's urbs tree.
	 * No need to cancel the transfer, it will be
	 * cleaned up automatically, by the hcd thread.
	 */
	idtree_remove(&drv->urbs, &t->linkage);
	_usb_transferPut(t);

	return 0;
}


static void _usb_pipeFree(usb_drv_t *drv, usb_pipe_t *pipe)
{
	rbnode_t *n, *nn;
	usb_transfer_t *t;

	if (drv != NULL) {
		/* Free all preallocated urbs */
		n = lib_rbMinimum(drv->urbs.root);
		while (n != NULL) {
			t = lib_treeof(usb_transfer_t, linkage, n);
			nn = lib_rbNext(n);
			if (t->pipeid == usb_pipeid(pipe))
				_usb_urbFree(t, drv, pipe);
			n = nn;
		}

		idtree_remove(&drv->pipes, &pipe->linkage);
	}

	pipe->dev->hcd->ops->pipeDestroy(pipe->dev->hcd, pipe);
	free(pipe);
}


int usb_drvUnbind(usb_drv_t *drv, usb_dev_t *dev, int iface)
{
	msg_t msg = { 0 };
	usb_msg_t *umsg = (usb_msg_t *)msg.i.raw;
	usb_pipe_t *pipe;
	rbnode_t *n, *nn;

	mutexLock(usbdrv_common.lock);

	n = lib_rbMinimum(drv->pipes.root);
	while (n != NULL) {
		pipe = lib_treeof(usb_pipe_t, linkage, n);
		nn = lib_rbNext(n);
		if (pipe->dev == dev)
			_usb_pipeFree(drv, pipe);
		n = nn;
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


usb_drv_t *_usb_drvFind(pid_t pid)
{
	usb_drv_t *drv, *res = NULL;

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

	return res;
}


usb_drv_t *usb_drvFind(int pid)
{
	usb_drv_t *drv;

	mutexLock(usbdrv_common.lock);
	drv = _usb_drvFind(pid);
	mutexUnlock(usbdrv_common.lock);

	return drv;
}


void usb_drvAdd(usb_drv_t *drv)
{
	mutexLock(usbdrv_common.lock);
	idtree_init(&drv->pipes);
	idtree_init(&drv->urbs);
	LIST_ADD(&usbdrv_common.drvs, drv);
	mutexUnlock(usbdrv_common.lock);
}


static usb_transfer_t *usb_transferAlloc(int sync, int type, usb_setup_packet_t *setup, usb_dir_t dir, size_t size, char *buf)
{
	usb_transfer_t *t;

	if ((t = calloc(1, sizeof(usb_transfer_t))) == NULL)
		return NULL;

	t->async = !sync;
	t->direction = dir;
	t->transferred = 0;
	t->size = size;
	t->state = urb_idle;
	t->type = type;

	if (size > 0) {
		if ((t->buffer = usb_alloc(t->size)) == NULL) {
			free(t);
			return NULL;
		}
	}

	if (type == usb_transfer_control) {
		t->setup = usb_alloc(sizeof(usb_setup_packet_t));
		if (t->setup == NULL) {
			usb_free(t->buffer, t->size);
			free(t);
			return NULL;
		}
		memcpy(t->setup, setup, sizeof(usb_setup_packet_t));
	}

	if (dir == usb_dir_out && size > 0)
		memcpy(t->buffer, buf, t->size);

	return t;
}


void usb_transferFree(usb_transfer_t *t)
{
	usb_free(t->buffer, t->size);
	usb_free(t->setup, sizeof(usb_setup_packet_t));
	free(t);
}


static int _usb_urbSubmit(usb_transfer_t *t, usb_pipe_t *pipe)
{
	if (t->state != urb_idle)
		return -EBUSY;

	t->state = urb_ongoing;
	t->pipeid = usb_pipeid(pipe);

	if (usb_transferSubmit(t, pipe, NULL) < 0) {
		t->state = urb_idle;
		return -EIO;
	}

	return 1;
}


static int _usb_handleUrbcmd(msg_t *msg)
{
	usb_msg_t *umsg = (usb_msg_t *)msg->i.raw;
	usb_urbcmd_t *urbcmd = &umsg->urbcmd;
	usb_transfer_t *t;
	usb_pipe_t *pipe;
	usb_drv_t *drv;
	int ret;

	drv = _usb_drvFind(msg->pid);
	if (drv == NULL)
		return -EINVAL;

	pipe = _usb_pipeFind(drv, urbcmd->pipeid);
	if (pipe == NULL)
		return -EINVAL;

	t = _usb_transferFind(drv, urbcmd->urbid);
	if (t == NULL)
		return -EINVAL;

	switch (urbcmd->cmd) {
		case urbcmd_submit:
			if (t->type == usb_transfer_control) {
				memcpy(t->setup, &urbcmd->setup, sizeof(urbcmd->setup));
			}
			ret = _usb_urbSubmit(t, pipe);
			break;
		case urbcmd_cancel:
			ret = _usb_urbCancel(t, pipe);
			break;
		case urbcmd_free:
			ret = _usb_urbFree(t, drv, pipe);
			break;
		default:
			ret = -EINVAL;
	}

	if (ret <= 0) {
		_usb_transferPut(t);
		return ret;
	}
	else {
		/* Transfer submitted, transfer reference handed to hcd */
		return 0;
	}
}


int usb_handleUrbcmd(msg_t *msg)
{
	int ret;

	mutexLock(usbdrv_common.lock);
	ret = _usb_handleUrbcmd(msg);
	mutexUnlock(usbdrv_common.lock);

	return ret;
}


static int _usb_handleUrb(msg_t *msg, unsigned int port, unsigned long rid)
{
	usb_msg_t *umsg = (usb_msg_t *)msg->i.raw;
	usb_urb_t *urb = &umsg->urb;
	usb_drv_t *drv;
	usb_transfer_t *t;
	int ret = 0;

	if ((drv = _usb_drvFind(msg->pid)) == NULL) {
		USB_LOG("usb: driver pid %d does not exist!\n", msg->pid);
		return -EINVAL;
	}

	t = usb_transferAlloc(urb->sync, urb->type, &urb->setup, urb->dir, urb->size, msg->i.data);
	if (t == NULL)
		return -ENOMEM;

	t->port = drv->port;
	t->pipeid = urb->pipe;

	/* For async urbs only allocate resources. The transfer would be executed,
	 * upon receiving usb_submit_t msg later */
	if (!urb->sync) {
		if (idtree_alloc(&drv->urbs, &t->linkage) < 0) {
			usb_transferFree(t);
			return -ENOMEM;
		}

		t->refcnt = 1;
		ret = t->linkage.id;
	}
	else {
		t->rid = rid;
		t->pid = msg->pid;

		if (_usb_drvTransfer(drv, t) < 0) {
			usb_transferFree(t);
			return -EINVAL;
		}
	}

	/* For async urbs, respond immediately and send usb_completion msg later.
	 * For sync ones, block the sender, and respond later */
	return ret;
}


int usb_handleUrb(msg_t *msg, unsigned int port, unsigned long rid)
{
	int ret;

	mutexLock(usbdrv_common.lock);
	ret = _usb_handleUrb(msg, port, rid);
	mutexUnlock(usbdrv_common.lock);

	return ret;
}


void usb_drvPipeFree(usb_drv_t *drv, usb_pipe_t *pipe)
{
	mutexLock(usbdrv_common.lock);

	_usb_pipeFree(drv, pipe);

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
