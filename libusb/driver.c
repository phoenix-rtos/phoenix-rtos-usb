#include <errno.h>
#include <usbdriver.h>
#include <sys/msg.h>
#include <string.h>
#include <unistd.h>

static struct {
	unsigned port;
} usbdrv_common;

int usb_connect(const usb_device_id_t *filters, int nfilters, unsigned drvport)
{
	msg_t msg = { 0 };
	usb_msg_t *umsg = (usb_msg_t *)&msg.i.raw;
	oid_t oid;

	while (lookup("/dev/usb", NULL, &oid) < 0)
		usleep(1000000);

	msg.type = mtDevCtl;
	msg.i.size = sizeof(usb_device_id_t) * nfilters;
	msg.i.data = (void *)filters;

	umsg->type = usb_msg_connect;
	umsg->connect.port = drvport;
	umsg->connect.nfilters = nfilters;

	if (msgSend(oid.port, &msg) < 0)
		return -1;

	usbdrv_common.port = oid.port;

	return oid.port;
}


int usb_eventsWait(int port, msg_t *msg)
{
	unsigned long rid;

	if (msgRecv(port, msg, &rid) < 0)
		return -1;

	if (msg->type != mtDevCtl)
		return -1;

	if (msgRespond(port, msg, rid) < 0)
		return -1;

	return 0;
}


int usb_open(usb_insertion_t *dev, usb_transfer_type_t type, usb_dir_t dir)
{
	int ret;
	msg_t msg;
	usb_msg_t *umsg = (usb_msg_t *)msg.i.raw;
	int res;

	msg.type = mtDevCtl;
	umsg->type = usb_msg_open;

	umsg->open.bus = dev->bus;
	umsg->open.dev = dev->dev;
	umsg->open.iface = dev->interface;
	umsg->open.type = type;
	umsg->open.dir = dir;

	if ((ret = msgSend(usbdrv_common.port, &msg)) != 0)
		return ret;

	return *(int *)msg.o.raw;
}


static int usb_urbSubmitSync(usb_urb_t *urb, void *data)
{
	int ret;
	msg_t msg;
	usb_msg_t *umsg = (usb_msg_t *)msg.i.raw;

	msg.type = mtDevCtl;
	umsg->type = usb_msg_urb;

	memcpy(&umsg->urb, urb, sizeof(usb_urb_t));

	msg.i.data = data;
	msg.i.size = urb->size;

	if ((ret = msgSend(usbdrv_common.port, &msg)) != 0)
		return ret;

	return msg.o.io.err;
}


int usb_transferControl(unsigned pipe, usb_setup_packet_t *setup, void *data, size_t size, usb_dir_t dir)
{
	usb_urb_t urb = {
		.pipe = pipe,
		.setup = *setup,
		.dir = dir,
		.size = size
	};

	return usb_urbSubmitSync(&urb, data);
}


int usb_transferBulk(unsigned pipe, void *data, size_t size, usb_dir_t dir)
{
	usb_urb_t urb = {
		.pipe = pipe,
		.dir = dir,
		.size = size
	};

	return usb_urbSubmitSync(&urb, data);
}


int usb_setConfiguration(unsigned pipe, int conf)
{
	usb_setup_packet_t setup = (usb_setup_packet_t) {
		.bmRequestType = REQUEST_DIR_HOST2DEV | REQUEST_TYPE_STANDARD | REQUEST_RECIPIENT_DEVICE,
		.bRequest = REQ_SET_CONFIGURATION,
		.wValue = conf,
		.wIndex = 0,
		.wLength = 0,
	};

	return usb_transferControl(pipe, &setup, NULL, 0, usb_dir_out);
}


int usb_clearFeatureHalt(unsigned pipe, int ep)
{
	usb_setup_packet_t setup = (usb_setup_packet_t) {
		.bmRequestType = REQUEST_DIR_HOST2DEV | REQUEST_TYPE_STANDARD | REQUEST_RECIPIENT_DEVICE,
		.bRequest = REQ_CLEAR_FEATURE,
		.wValue = USB_ENDPOINT_HALT,
		.wIndex = ep,
		.wLength = 0,
	};

	return usb_transferControl(pipe, &setup, NULL, 0, usb_dir_out);
}


usb_modeswitch_t *usb_modeswitchFind(uint16_t vid, uint16_t pid, const usb_modeswitch_t *modes, int nmodes)
{
	int i;

	for (i = 0; i < nmodes; i++) {
		if (vid == modes[i].vid && pid == modes[i].pid)
			return &modes[i];
	}

	return NULL;
}


int usb_modeswitchHandle(usb_insertion_t *insertion, usb_modeswitch_t *mode)
{
	int pipeCtrl, pipeIn, pipeOut;

	if ((pipeCtrl = usb_open(insertion, usb_transfer_control, usb_dir_bi)) < 0)
		return -EINVAL;

	if (usb_setConfiguration(pipeCtrl, 1) != 0)
		return -EINVAL;

	if ((pipeIn = usb_open(insertion, usb_transfer_bulk, usb_dir_in)) < 0)
		return -EINVAL;

	if ((pipeOut = usb_open(insertion, usb_transfer_bulk, usb_dir_out)) < 0)
		return -EINVAL;

	fprintf(stderr, "usb: Performing modeswitch\n");
	if (usb_transferBulk(pipeOut, mode->msg, sizeof(mode->msg), usb_dir_out) < 0)
		return -1;
	fprintf(stderr, "usb: modeswitch msg write success\n");

#if 0
	/* TODO: optimize switch time by using Halt Feature */
	if (mode->scsiresp) {
		if (usb_transferBulk(dev->pipeIn, &csw, sizeof(csw), usb_dir_in) < 0)
			return -1;

		if (csw.sig != CSW_SIG || csw.tag != ((umass_cbw_t *)m->msg)->tag || csw.status != 0) {
			fprintf(stderr, "umass: transmit fail\n");
			return -1;
		}
	}

	if (usb_clearFeatureHalt(dev->pipeCtrl, 1) != 0)
		return -1;

	if (usb_clearFeatureHalt(dev->pipeCtrl, 129) != 0)
		return -1;
#endif

	return 0;
}