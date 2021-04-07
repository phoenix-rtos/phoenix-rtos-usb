#
# Makefile for Phoenix-RTOS usb class lib
#
# Copyright 2020-2021 Phoenix Systems
#


$(PREFIX_A)libusbclass.a: $(addprefix $(PREFIX_O)client/, cdc_client.o hid_client.o) $(PREFIX_A)libusbclient.a $(PREFIX_A)libusbhid.a $(PREFIX_A)libusbcdc.a
	$(ARCH)

$(PREFIX_H)cdc_client.h: client/cdc_client.h
	$(HEADER)

$(PREFIX_H)hid_client.h: client/hid_client.h
	$(HEADER)

all: $(PREFIX_A)libusbclass.a $(addprefix $(PREFIX_H), cdc_client.h hid_client.h)
