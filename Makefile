###########################################################
# Makefile
#
# Copyright 2004, Stefan Siegl <ssiegl@gmx.de>, Germany
# 
# This is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Publice License,
# version 2 or any later. The license is contained in the COPYING
# file that comes with the fuse4hurd distribution.
#
# Makefile to compile fuse4hurd library
#/

CFLAGS=-D_FILE_OFFSET_BITS=64 -DHAVE_CONFIG_H -D_GNU_SOURCE -Wall -ggdb
LDFLAGS=$(CFLAGS)

OBJS=main.o netfs.o node.o netnode.o

all: libfuse.a
	make -C example all

clean:
	rm -f libfuse.a
	rm -f $OBJS

libfuse.a: $(OBJS)
	ar rcsv $@ $(OBJS)

%.d:%.c
	$(CC) -MM -I. $(CFLAGS) $< > .$@.dep
	sed 's,\($*\)\.o[ :]*,\1.o $@ : ,g' < .$@.dep > $@
	rm -f .$@.dep

-include $(OBJS:.o=.d)

.c.o:
	$(CC) -I. $(CFLAGS) -c $<