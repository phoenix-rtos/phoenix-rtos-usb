#
# Makefile for phoenix-rtos-usb
#
# Copyright 2018 Phoenix Systems
#
# %LICENSE%
#

SIL ?= @
MAKEFLAGS += --no-print-directory

#TARGET ?= ia32-qemu
#TARGET ?= armv7-stm32
TARGET ?= arm-imx

# Compliation options for various architectures
TARGET_FAMILY = $(firstword $(subst -, ,$(TARGET)-))
include Makefile.$(TARGET_FAMILY)

### output target and sources ###
PROG := usb
SRCS := $(wildcard src/*.c)
LDLIBS := -lehci-imx6ull $(LDLIBS)

### build/install dirs ###
BUILD_PREFIX ?= ../build/$(TARGET)
BUILD_PREFIX := $(abspath $(BUILD_PREFIX))
BUILD_DIR ?= $(BUILD_PREFIX)/$(notdir $(TOPDIR))
BUILD_DIR := $(abspath $(BUILD_DIR))

PREFIX_O := $(BUILD_DIR)/$(PROG)/
PREFIX_A ?= $(BUILD_PREFIX)/lib/
PREFIX_H ?= $(BUILD_PREFIX)/include/
PREFIX_PROG ?= $(BUILD_PREFIX)/prog/
PREFIX_PROG_STRIPPED ?= $(BUILD_PREFIX)/prog.stripped/

# setup paths
CFLAGS += -I$(PREFIX_H)
LDFLAGS += -L$(PREFIX_A)

### intermediate targets ###
PROG_OBJS += $(patsubst %,$(PREFIX_O)%,$(SRCS:.c=.o))
PROG_UNSTRIPPED := $(patsubst %,$(PREFIX_PROG)%,$(PROG))
PROG_STRIPPED   := $(patsubst %,$(PREFIX_PROG_STRIPPED)%,$(PROG))

# try to resolve static libs to provice correct rebuild dependencies
PSMK_LDPATH := $(subst ",,$(patsubst -L%,%,$(filter -L%,$(LDFLAGS)))) $(shell $(CC) $(CFLAGS) -print-search-dirs |grep "libraries: " |tr : " ")
PSMK_RESOLVED_LDLIBS := $(filter-out -l%,$(LDLIBS)) $(foreach lib,$(patsubst -l%,lib%.a,$(LDLIBS)),$(foreach ldpath,$(PSMK_LDPATH),$(wildcard $(ldpath)/$(lib))))

# generic rules
$(PREFIX_O)%.o: %.c
	@mkdir -p $(@D)
	$(SIL)(printf " CC  %-24s\n" "$<")
	$(SIL)$(CC) -c $(CFLAGS) "$<" -o "$@"
	$(SIL)$(CC) -M  -MD -MP -MF $(PREFIX_O)$*.c.d -MT "$@" $(CFLAGS) $<


$(PROG_UNSTRIPPED): $(PROG_OBJS) $(PSMK_RESOLVED_LDLIBS)
	@mkdir -p $(@D)
	@(printf " LD  %-24s\n" "$(@F)")
	$(SIL)$(LD) $(LDFLAGS) -o "$@" $(PROG_OBJS) $(LDLIBS)

$(PROG_STRIPPED): $(PROG_UNSTRIPPED)
	@mkdir -p $(@D)
	@(printf " STR %-24s  \n" "$(@F)")
	$(SIL)$(STRIP) -s -o "$@" "$<"


### default target ###
# suppress 'nothing to be done'
all: $(PROG_UNSTRIPPED) $(PROG_STRIPPED) $(PROG_OBJS)
	@echo > /dev/null;

clean:
	$(SIL)rm -rf $(PROG_OBJS) $(PROG_UNSTRIPPED) $(PROG_STRIPPED)

# include file dependencies
ALL_D := $(wildcard $(PREFIX_O)*.d)
-include $(ALL_D)

.PHONY: all clean
