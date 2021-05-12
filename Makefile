#
# Makefile for phoenix-rtos-usb
#
# Copyright 2019, 2020 Phoenix Systems
#
# %LICENSE%
#

include ../phoenix-rtos-build/Makefile.common
# FIXME: this include should be done by Makefile.common
include ../phoenix-rtos-build/Makefile.$(TARGET_SUFF)

.DEFAULT_GOAL := all

ifneq ($(filter %clean,$(MAKECMDGOALS)),)
$(info cleaning targets, make parallelism disabled)
.NOTPARALLEL:
endif

# should define DEFAULT_COMPONENTS and target-specific variables
include _targets/Makefile.$(TARGET_FAMILY)-$(TARGET_SUBFAMILY)

# default path for the programs to be installed in rootfs
DEFAULT_INSTALL_PATH := /sbin

# read out all components
ALL_MAKES := $(wildcard */Makefile) $(wildcard */*/Makefile)
include $(ALL_MAKES)

# create generic targets
.PHONY: all install clean
all: $(DEFAULT_COMPONENTS)
install: $(patsubst %,%-install,$(DEFAULT_COMPONENTS))
clean: $(patsubst %,%-clean,$(DEFAULT_COMPONENTS))
