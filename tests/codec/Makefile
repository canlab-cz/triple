# Host-side round-trip test of the Triple wire codec (driver/triple_parse.c).
# The shim/ directory provides stand-ins for the few kernel headers the codec uses.
CC      ?= gcc
CFLAGS  ?= -std=gnu99 -Wall -Wextra -Wno-unused-parameter -Wno-unused-function
DRV     := ../../driver

codec_test: codec_test.c $(DRV)/triple_parse.c $(DRV)/include/triple_helper.h $(DRV)/include/triple_parse.h
	$(CC) $(CFLAGS) -I shim -I $(DRV)/include -o $@ codec_test.c $(DRV)/triple_parse.c

test: codec_test
	./codec_test

clean:
	rm -f codec_test

.PHONY: test clean
