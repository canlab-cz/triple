# Host-side test of the Triple wire codec (driver/triple_proto.c).
CC      ?= gcc
CFLAGS  ?= -std=gnu99 -Wall -Wextra -Wno-unused-parameter
DRV     := ../../driver

codec_test: codec_test.c $(DRV)/triple_proto.c $(DRV)/include/triple_proto.h
	$(CC) $(CFLAGS) -I $(DRV)/include -o $@ codec_test.c $(DRV)/triple_proto.c

test: codec_test
	./codec_test

clean:
	rm -f codec_test

.PHONY: test clean
