#include <usbdriver.h>
#include <sys/msg.h>
#include <string.h>
#include <unistd.h>


int usb_connect(usb_device_id_t *id, int drvport)
{
	msg_t msg = { 0 };
	usb_msg_t *usb_msg = (usb_msg_t *)&msg.i.raw;
	oid_t oid;
	int ret = 0;

	while (lookup("/dev/usb", NULL, &oid) < 0)
		usleep(1000000);

	msg.type = mtDevCtl;
	usb_msg->type = usb_msg_connect;
	usb_msg->connect.port = drvport;
	memcpy(&usb_msg->connect.filter, id, sizeof(usb_device_id_t));

	if (msgSend(oid.port, &msg) < 0)
		return -1;

	return oid.port;
}


int usb_eventsWait(int port, usb_insertion_t *insertion, usb_deletion_t *deletion)
{
	msg_t msg;
	usb_msg_t *umsg;
	int ret;
	unsigned long rid;

	if (msgRecv(port, &msg, &rid) < 0)
		return -1;

	if (msg.type != mtDevCtl)
		return -1;

	umsg = (usb_msg_t *)msg.i.raw;
	switch (umsg->type) {
		case usb_msg_insertion:
			*insertion = umsg->insertion;
			ret = usb_msg_insertion;
			break;
		case usb_msg_deletion:
			*deletion = umsg->deletion;
			ret = usb_msg_deletion;
			break;
		default:
			ret = -1;
			break;
	}

	if (msgRespond(port, &msg, rid) < 0)
		return -1;

	return ret;
}
