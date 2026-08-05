obj-m += uad2_identity.o
obj-m += uad2_compute.o

ccflags-y += -I$(src)/../include/uapi

.PHONY: all clean

all:
	$(MAKE) -C /lib/modules/$(shell uname -r)/build M=$(CURDIR) modules

clean:
	$(MAKE) -C /lib/modules/$(shell uname -r)/build M=$(CURDIR) clean
