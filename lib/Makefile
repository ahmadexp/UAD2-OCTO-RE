CC ?= cc
AR ?= ar
CPPFLAGS += -I../include/uapi
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Werror

.PHONY: all clean

all: libuad2compute.a uad2ctl

libuad2compute.a: uad2_compute.o
	$(AR) rcs $@ $^

uad2ctl: uad2ctl.o libuad2compute.a
	$(CC) $(LDFLAGS) -o $@ uad2ctl.o libuad2compute.a

clean:
	rm -f -- *.o *.a uad2ctl
