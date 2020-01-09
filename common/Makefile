#
# Makefile for Phoenix-RTOS hid and cdc API
#
# Copyright 2020 Phoenix Systems
#


$(PREFIX_A)libusbhid.a: 
	$(ARCH)

$(PREFIX_A)libusbcdc.a: 
	$(ARCH)

$(PREFIX_H)hid.h: common/hid.h
	$(HEADER)

$(PREFIX_H)cdc.h: common/cdc.h
	$(HEADER)

all: $(PREFIX_A)libusbhid.a $(PREFIX_H)hid.h $(PREFIX_A)libusbcdc.a $(PREFIX_H)cdc.h

