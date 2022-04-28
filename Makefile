#
# Makefile for phoenix-rtos-usb
#
# Copyright 2019, 2020 Phoenix Systems
#
# %LICENSE%
#

include ../phoenix-rtos-build/Makefile.common

.DEFAULT_GOAL := all

# default path for the programs to be installed in rootfs
DEFAULT_INSTALL_PATH := /sbin

DEFAULT_COMPONENTS := libusb
ALL_COMPONENTS := libusb usb

ALL_MAKES := $(wildcard */Makefile)
include $(ALL_MAKES)

# create generic targets
.PHONY: all install clean
all: $(DEFAULT_COMPONENTS)
install: $(patsubst %,%-install,$(DEFAULT_COMPONENTS))
clean: $(patsubst %,%-clean,$(ALL_COMPONENTS))
