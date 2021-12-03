#
# Makefile for Phoenix-RTOS USB host
#
# Copyright 2021 Phoenix Systems
#

NAME := usb
LOCAL_SRCS := usb.c dev.c drv.c hcd.c hub.c mem.c
LOCAL_HEADERS := hcd.h hub.h dev.h drv.h usbhost.h
LIBS := $(USB_HCD_LIBS)
DEPS := libusb

include $(binary.mk)
