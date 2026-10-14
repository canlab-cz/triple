# SPDX-License-Identifier: GPL-2.0
# Copyright (c) 2026 CANLAB s.r.o.
KERNEL_SRC       ?= /lib/modules/$(shell uname -r)/build
MODULE_DIR       := $(CURDIR)

TARGET           := usb2cansocketcan.ko
obj-m            := usb2cansocketcan.o
usb2cansocketcan-y := triple.o triple_proto.o
# $(M) is the module directory handed to the kernel build system
ccflags-y        := -I$(M)/include -Wno-declaration-after-statement

default:
	$(MAKE) -C $(KERNEL_SRC) M=$(MODULE_DIR) modules
	@cp $(TARGET) ../

clean:
	$(MAKE) -C $(KERNEL_SRC) M=$(MODULE_DIR) clean
	@rm -f ../$(TARGET)
