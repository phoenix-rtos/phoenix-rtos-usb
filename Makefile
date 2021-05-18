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

NAME := libusb
LOCAL_HEADERS := $(wildcard *.h)
LOCAL_SRCS := cdc_client.c hid_client.c
include $(static-lib.mk)

NAME := usb
LOCAL_HEADERS := usbhost.h
LOCAL_SRCS := usb.c
LIBS := libusbehci
include $(binary.mk)

# should define DEFAULT_COMPONENTS and target-specific variables
include _targets/Makefile.$(TARGET_FAMILY)-$(TARGET_SUBFAMILY)

# create generic targets
.PHONY: all install clean
all: $(DEFAULT_COMPONENTS)
install: $(patsubst %,%-install,$(DEFAULT_COMPONENTS))
clean: $(patsubst %,%-clean,$(ALL_COMPONENTS))
