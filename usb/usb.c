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

#include <hcd.h>
#include <hub.h>

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

static struct {
	handle_t common_lock, enumeratorLock;
	handle_t enumeratorCond;
	usb_hub_t *hubs;
	usb_bus_t *buses;
	hcd_ops_t *hcdOps;
	int nbuses;
	uint32_t port;
} usb_common;


int hcd_getInfo(const hcd_info_t **info);

static int hub_initPorts(usb_hub_t *hub)
{
	int i;

	hub->ports = malloc(sizeof(usb_port_t) * hub->nports);
	if (hub->ports == NULL)
		return -ENOMEM;

	for (i = 1; i <= hub->nports; i++) {
		hub->ports[i].num = i;
		hub->ports[i].device = NULL;
	}

	return 0;
}


void usb_transferFinished(usb_transfer_t *t)
{
	/* TODO: handle other types of transfers */
	condSignal(usb_common.enumeratorCond);
}


static int usb_getDescriptor(usb_device_t *dev, int descriptor, int index, char *buffer, int size)
{
	usb_setup_packet_t setup = (usb_setup_packet_t) {
		.bmRequestType = REQUEST_DIR_DEV2HOST | REQUEST_TYPE_STANDARD | REQUEST_RECIPIENT_DEVICE,
		.bRequest = REQ_GET_DESCRIPTOR,
		.wValue = descriptor << 8 | index,
		.wIndex = 0,
		.wLength = size,
	};
	usb_transfer_t t = (usb_transfer_t) {
		.endpoint = dev->ep0,
		.type = usb_transfer_control,
		.direction = usb_transfer_in,
		.setup = &setup,
		.buffer = buffer,
		.size = size
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
		.endpoint = dev->ep0,
		.type = usb_transfer_control,
		.direction = usb_transfer_out,
		.setup = &setup,
		.buffer = NULL,
		.size = 0
	};

	if (dev->hcd->ops->transferEnqueue(dev->hcd, &t) != 0)
		return -1;

	mutexLock(usb_common.enumeratorLock);
	condWait(usb_common.enumeratorCond, usb_common.enumeratorLock, 0);
	mutexUnlock(usb_common.enumeratorLock);

	dev->address = address;

	return 0;
}



static void usb_cleanup(void)
{
	usb_bus_t *bus = usb_common.buses;

	if (bus == NULL)
		return;

	/* Free all devices, hcd, ops, and bus structures */
	do {
		/* TODO */
		bus = bus->next;
	} while (bus != usb_common.buses);
}

/* TODO: remove bus structure */
static usb_bus_t *bus_create(int num)
{
	usb_bus_t *bus;

	bus = malloc(sizeof(usb_bus_t));
	if (bus == NULL)
		return NULL;

	bus->id = num;
	bus->ndevices = 0;
	bus->hcd = NULL;
	bus->devices = NULL;
	bus->maxaddr = 0;

	LIST_ADD(&usb_common.buses, bus);

	return bus;
}


static int dev_addressSet(usb_bus_t *bus, usb_device_t *dev)
{
	dev->address = (bus->maxaddr % 255) + 1;
	bus->maxaddr = dev->address;

	return 0;
}

static usb_device_t *dev_create(usb_bus_t *bus)
{
	usb_device_t *dev;

	if ((dev = malloc(sizeof(usb_device_t))) == NULL)
		return NULL;

	dev->address = 0;

	/* Create control endpoint */
	if ((dev->ep0 = malloc(sizeof(usb_endpoint_t))) == NULL) {
		free(dev);
		return NULL;
	}

	dev->hcd = bus->hcd;
	dev->ep0->max_packet_len = 64; /* Default value */
	dev->ep0->number = 0;
	dev->ep0->device = dev;
	dev->ep0->type = usb_ep_control;
	dev->ep0->direction = usb_ep_bi;

	LIST_ADD(&bus->devices, dev);

	return dev;
}

static usb_device_t *dev_destroy(usb_device_t *dev)
{

}


static usb_hub_t *hub_create(usb_device_t *dev, hcd_t *hcd)
{
	usb_hub_t *hub;

	hub = malloc(sizeof(usb_hub_t));
	if (hub == NULL)
		return NULL;

	LIST_ADD(&usb_common.hubs, hub);
	hub->dev = dev;
	hub->hcd = hcd;

	return hub;
}


void hcd_register(hcd_ops_t *ops)
{
	LIST_ADD(&usb_common.hcdOps, ops);
}


static const hcd_ops_t *hcd_lookup(const char *type)
{
	hcd_ops_t *ops = usb_common.hcdOps;

	if (ops == NULL)
		return NULL;

	do {
		if (!strcmp(type, ops->type))
			return ops;
		ops = ops->next;
	} while (ops != usb_common.hcdOps);

	return NULL;
}


static hcd_t *hcd_create(usb_bus_t *bus, const hcd_ops_t *ops, const hcd_info_t *info)
{
	hcd_t *hcd;

	hcd = malloc(sizeof(hcd_t));
	if (hcd == NULL)
		return NULL;

	hcd->info = info;
	hcd->priv = NULL;
	hcd->transfers = NULL;
	hcd->ops = ops;

	bus->hcd = hcd;
	hcd->bus = bus;

	return hcd;
}


static int hcd_init(void)
{
	const hcd_info_t *info;
	const hcd_ops_t *ops;
	hcd_t *hcd;
	usb_bus_t *bus;
	usb_device_t *dev;
	usb_hub_t *hub;
	int nhcd, i;

	if ((nhcd = hcd_getInfo(&info)) <= 0) {
		fprintf(stderr, "usb: No hcd available!\n");
		return -EINVAL;
	}

	for (i = 0; i < nhcd; i++) {
		if ((ops = hcd_lookup(info[i].type)) == NULL) {
			fprintf(stderr, "usb: Can't find driver for hcd: %s\n", info->type);
			usb_cleanup();
			return -EINVAL;
		}

		if ((bus = bus_create(usb_common.nbuses++)) == NULL) {
			fprintf(stderr, "usb: Can't create a bus - out of memory!\n");
			usb_cleanup();
			return -ENOMEM;
		}

		if ((hcd = hcd_create(bus, ops, &info[i])) == NULL) {
			fprintf(stderr, "usb: Can't create a hcd - out of memory!\n");
			usb_cleanup();
			return -ENOMEM;
		}

		/* Create root hub */
		if ((dev = dev_create(bus)) == NULL) {
			fprintf(stderr, "usb: Can't create a hub - out of memory!\n");
			usb_cleanup();
			return -ENOMEM;
		}

		if ((hub = hub_create(dev, hcd)) == NULL) {
			fprintf(stderr, "usb: Can't create a hub - out of memory!\n");
			usb_cleanup();
			return -ENOMEM;
		}
		hcd->roothub = hub;

		if (hcd->ops->init(hcd) != 0) {
			fprintf(stderr, "usb: Error initializing host controller!\n");
			usb_cleanup();
			return -EINVAL;
		}

		if (hub_initPorts(hub) != 0) {
			fprintf(stderr, "usb: Can't create root hub ports - out of memory!\n");
			usb_cleanup();
			return -EINVAL;
		}
	}

	return 0;
}


static int usb_devsList(char *buffer, size_t size)
{
	usb_bus_t *bus = usb_common.buses;
	usb_device_t *dev;
	size_t ret, bytes = 0;

	while (size > 0) {
		dev = bus->devices;
		do {
			ret = snprintf(buffer, size, "Bus %03d Device %03d: %s\n",
			               bus->id, dev->address, dev->name);
			buffer += ret;
			bytes += ret;
			size -= ret;
			dev = dev->next;
		} while (dev != bus->devices && size > 0);

		bus = bus->next;
		if (bus == usb_common.buses)
			break;
	}

	return bytes;
}


static void usb_msgthr(void *arg)
{
	unsigned port = (int)arg;
	unsigned long rid;
	msg_t msg;
	usb_msg_t *umsg;

	for (;;) {
		if (msgRecv(port, &msg, &rid) < 0)
			continue;

		mutexLock(usb_common.common_lock);
		switch (msg.type) {
			case mtRead:
				msg.o.io.err = usb_devsList(msg.o.data, msg.o.size);
				break;
			case mtDevCtl:
				umsg = (void *)msg.i.raw;
				break;
			default:
				fprintf(stderr, "usb: unsupported msg type\n");
				msg.o.io.err = -EINVAL;
		}
		mutexUnlock(usb_common.common_lock);

		msgRespond(port, &msg, rid);
	}
}

static int usb_portCleanFeatures(usb_hub_t *hub, int port, uint32_t change)
{
	int i;

	for (i = 0; i < 5; i++) {
		if (change & (1 << i)) {
			if (hub->clearPortFeature(hub, port, USB_PORT_FEAT_C_CONNECTION + i) != 0)
				return -1;
		}
	}

	return 0;
}

void usb_dumpDeviceDescriptor(FILE *stream, usb_device_desc_t *descr)
{
	fprintf(stream, "DEVICE DESCRIPTOR:\n");
	fprintf(stream, "\tbLength: %d\n", descr->bLength);
	fprintf(stream, "\tbDescriptorType: %d\n", descr->bDescriptorType);
	fprintf(stream, "\tbcdUSB: %d\n", descr->bcdUSB);
	fprintf(stream, "\tbDeviceClass: %d\n", descr->bDeviceClass);
	fprintf(stream, "\tbDeviceSubClass: %d\n", descr->bDeviceSubClass);
	fprintf(stream, "\tbDeviceProtocol: %d\n", descr->bDeviceProtocol);
	fprintf(stream, "\tbMaxPacketSize0: %d\n", descr->bMaxPacketSize0);
	fprintf(stream, "\tidVendor: %x\n", descr->idVendor);
	fprintf(stream, "\tidProduct: %x\n", descr->idProduct);
	fprintf(stream, "\tbcdDevice: %d\n", descr->bcdDevice);
	fprintf(stream, "\tiManufacturer: %d\n", descr->iManufacturer);
	fprintf(stream, "\tiProduct: %d\n", descr->iProduct);
	fprintf(stream, "\tiSerialNumber: %d\n", descr->iSerialNumber);
	fprintf(stream, "\tbNumConfigurations: %d\n", descr->bNumConfigurations);
}


static int usb_getDeviceDescriptor(usb_device_t *dev)
{
	if (usb_getDescriptor(dev, USB_DESC_DEVICE, 0, (char *)&dev->descriptor, sizeof(usb_device_desc_t)) != 0) {
		fprintf(stderr, "usb: Fail to get device descriptor\n");
		return -1;
	}

	usb_dumpDeviceDescriptor(stderr, &dev->descriptor);

	return 0;
}


static int usb_getConfigurationDescriptor(usb_device_t *dev)
{
	usb_configuration_desc_t desc;

	if (usb_getDescriptor(dev, USB_DESC_CONFIG, 0, (char *)&desc, sizeof(desc)) != 0) {
		fprintf(stderr, "usb: Fail to get device descriptor\n");
		return -1;
	}

	return 0;
}


static int usb_portReset(usb_hub_t *hub, int port, usb_port_status_t *status)
{
	if (hub->setPortFeature(hub, port, USB_PORT_FEAT_RESET) != 0)
		return -1;

	if (hub->getPortStatus(hub, port, status) != 0)
		return -1;

	if ((status->wPortChange & USB_PORT_FEAT_C_RESET) == 0)
		return -1;

	if (usb_portCleanFeatures(hub, port, status->wPortChange) != 0)
		return -1;

	return 0;
}


static void usb_portStatusChanged(usb_hub_t *hub, int port)
{
	usb_port_status_t status;
	usb_device_t *dev;

	if (hub->getPortStatus(hub, port, &status) != 0) {
		fprintf(stderr, "usb: getPortStatus port %d failed!\n", port);
		return;
	}

	if (usb_portCleanFeatures(hub, port, status.wPortChange) != 0) {
		fprintf(stderr, "usb: portCleanFeatures failed on port %d!\n", port);
		return;
	}

	if (status.wPortChange & USB_PORT_STAT_C_CONNECTION) {
		if (status.wPortStatus & USB_PORT_STAT_CONNECTION) {
			/* Device connected */
			if (usb_portReset(hub, port, &status) < 0) {
				fprintf(stderr, "usb: fail to reset port %d\n", port);
				return;
			}

			/* Reset feature cleaned - begin enumeration */
			if ((dev = dev_create(hub->hcd->bus)) == NULL) {
				fprintf(stderr, "usb: fail to create device!\n");
				return;
			}

			if (status.wPortStatus & USB_PORT_STAT_HIGH_SPEED)
				dev->speed = usb_high_speed;
			else if (status.wPortStatus & USB_PORT_STAT_LOW_SPEED)
				dev->speed = usb_low_speed;
			else
				dev->speed = usb_full_speed;

			hub->ports[port - 1].device = dev;

			if (usb_getDeviceDescriptor(dev) != 0) {
				fprintf(stderr, "usb: Fail to get device descriptor\n");
				dev_destroy(dev);
				return;
			}

			if (usb_portReset(hub, port, &status) != 0) {
				fprintf(stderr, "usb: Fail to reset port\n");
				dev_destroy(dev);
				return;
			}
			dev->ep0->max_packet_len = dev->descriptor.bMaxPacketSize0;

			if (usb_setAddress(dev, 1) != 0) {
				fprintf(stderr, "usb: Fail to set device address\n");
				dev_destroy(dev);
				return;
			}

			if (usb_getDeviceDescriptor(dev) != 0) {
				fprintf(stderr, "usb: Fail to get device descriptor\n");
				dev_destroy(dev);
				return;
			}
			/* RESET DEVICE */
			/* Configure the device */
		} else {
			/* Device disconnected */
			printf("DEVICE DISCONNTECTED\n");
			/* device destroy */
		}
	}
}


static void usb_enumerator(void *arg)
{
	usb_hub_t *hub;
	uint32_t portmask = 0;
	int n;

	for (;;) {
		sleep(1);
		hub = usb_common.hubs;
		if (hub == NULL)
			exit(1);

		do {
			hub->statusChanged(hub, &portmask);
			if (portmask > 1) {
				/* Port status changed */
				for (n = 1; n <= hub->nports; n++) {
					if (portmask & (1 << n))
						usb_portStatusChanged(hub, n);
				}
			}
			else if (portmask == 1) {
				/* Hub status changed */
			}
			hub = hub->next;
		} while (hub != usb_common.hubs);
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

	if (condCreate(&usb_common.enumeratorCond) != 0) {
		fprintf(stderr, "usb: Can't create mutex!\n");
		return -EINVAL;
	}

	if (portCreate(&usb_common.port) != 0) {
		fprintf(stderr, "usb: Can't create port!\n");
		return -EINVAL;
	}

	oid.port = usb_common.port;
	oid.id = 0;

	if (create_dev(&oid, "/dev/usb") != 0) {
		fprintf(stderr, "usb: Can't create dev!\n");
		return -EINVAL;
	}

	if (hcd_init() != 0) {
		fprintf(stderr, "usbsrv: hcd_init failed\n");
		return -EINVAL;
	}

	beginthread(usb_enumerator, 4, malloc(0x1000), 0x1000, NULL);

	usb_msgthr((void *) usb_common.port);

	return 0;
}