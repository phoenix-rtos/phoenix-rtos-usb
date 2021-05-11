#
# Makefile for Phoenix-RTOS host server
#
# Copyright 2020 Phoenix Systems
#

NAME := hostsrv
LOCAL_SRCS := hostsrv.c
LOCAL_HEADERS := hostsrv.h
LIBS := libusbehci

include $(binary.mk)


NAME := libhostproxy
LOCAL_HEADERS := hostproxy.h
LOCAL_SRCS := hostproxy.c
DEPS := hostsrv

include $(static-lib.mk)
