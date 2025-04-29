/*
 * Phoenix-RTOS
 *
 * Libusb driver interface
 *
 * USB low-level information API for userspace applications
 *
 * Copyright 2025 Phoenix Systems
 * Author: Adam Greloch
 *
 * %LICENSE%
 */

#include <string.h>

#include <usbdriver.h>
#include <usbinternal.h>

#include <usbdevinfo.h>

#include "log.h"


int usb_devinfoGet(oid_t oid, usb_devinfo_desc_t *desc)
{
	int err;
	oid_t hostOid;
	msg_t msg = { 0 };
	usb_msg_t *imsg = (usb_msg_t *)msg.i.raw;

	usb_hostLookup(&hostOid);

	msg.type = mtDevCtl;
	imsg->type = usb_msg_devdesc;
	imsg->devdesc.oid = oid;
	msg.o.data = desc;
	msg.o.size = sizeof(usb_devinfo_desc_t);

	err = msgSend(hostOid.port, &msg);
	if (err < 0) {
		log_error("msgSend failed: %d\n", err);
		return err;
	}

	if (msg.o.err < 0) {
		log_error("msg.o.err=%d\n", msg.o.err);
		return msg.o.err;
	}

	return 0;
}
