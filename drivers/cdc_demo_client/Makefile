#
# Makefile for Phoenix-RTOS cdc demo
#
# Copyright 2020 Phoenix Systems
#


$(PREFIX_PROG)cdc-demo: $(PREFIX_O)drivers/cdc_demo_client/cdc_demo.o $(PREFIX_A)libusbclass.a $(PREFIX_A)libusbclient.a $(PREFIX_A)libusbcdc.a
	$(LINK)

all: $(PREFIX_PROG_STRIPPED)cdc-demo
