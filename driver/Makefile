Q                := @
KERNEL_SRC       ?= /lib/modules/$(shell uname -r)/build
MODULE_DIR       := $(CURDIR)

CFILES           := main.c triple_parse.c tx.c
TARGET           := usb2cansocketcan.ko
obj-m            := usb2cansocketcan.o
usb2cansocketcan-y := $(CFILES:.c=.o)
# $(M) is the module directory handed to the kernel build system
ccflags-y        := -I$(M)/include -std=gnu99 -Wno-declaration-after-statement


default:
	$(MAKE) -C $(KERNEL_SRC) M=$(MODULE_DIR) modules
	$(Q)cp $(TARGET) ../

clean:
	$(MAKE) -C $(KERNEL_SRC) M=$(MODULE_DIR) clean
	$(Q)rm -f ../$(TARGET)
