#
# Makefile for Phoenix-RTOS libusb
#
# Copyright 2021 Phoenix Systems
#

NAME := libusb
LOCAL_PATH := $(call my-dir)
HEADERS := $(wildcard $(LOCAL_PATH)*.h)
LOCAL_SRCS := cdc_client.c hid_client.c driver.c
LOCAL_CFLAGS := -I$(LOCAL_PATH)
include $(static-lib.mk)
