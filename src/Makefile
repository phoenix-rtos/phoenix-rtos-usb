#
# Makefile for Phoenix-RTOS imx6ull ehci
#
# Copyright 2018 Phoenix Systems
#

PROGS_SRCS = usbd.c
PROGS = usb
LDLIBS += -lehci-imx6ull

LIB := libusb.a
LIB_SRCS := libusb.c
INSTALL_HDRS := libusb.h usb.h usbd.h

# include after all dependencies are set
include $(TOPDIR)/Makefile.rules
