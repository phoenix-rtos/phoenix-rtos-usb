#
# Makefile for Phoenix-RTOS host server
#
# Copyright 2020 Phoenix Systems
#


$(PREFIX_A)libhostproxy.a: $(PREFIX_O)host/hostproxy.o
	$(ARCH)
	
$(PREFIX_PROG)hostsrv: $(PREFIX_O)host/hostsrv.o $(PREFIX_A)libusbehci.a
	$(LINK) 
	
$(PREFIX_H)hostproxy.h: host/hostproxy.h
	$(HEADER)
	
$(PREFIX_H)hostsrv.h: host/hostsrv.h
	$(HEADER)

all: $(PREFIX_PROG_STRIPPED)hostsrv $(PREFIX_A)libhostproxy.a $(addprefix $(PREFIX_H), hostproxy.h hostsrv.h)
