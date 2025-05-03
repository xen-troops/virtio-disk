TARGET = virtio-disk
INSTALL = install
PREFIX=/usr/bin

OBJS :=	device.o \
	xs_dev.o \
	pci.o \
	demu.o


OBJS	+= virtio/blk.o
OBJS	+= virtio/core.o
OBJS	+= virtio/mmio.o
OBJS	+= virtio/pci.o
OBJS	+= virtio/mmio-legacy.o
OBJS	+= virtio/mmio-modern.o
OBJS	+= virtio/pci-modern.o

OBJS	+= disk/core.o
OBJS	+= disk/blk.o
OBJS	+= disk/raw.o
OBJS	+= disk/qcow.o
#OBJS	+= disk/aio.o

OBJS	+= util/init.o
OBJS	+= util/rbtree.o
OBJS	+= util/read-write.o
OBJS	+= util/util.o

#CC  := $(CROSS_COMPILE)gcc
#LD  := $(CROSS_COMPILE)ld

CFLAGS  = -I. -I$(shell pwd)/include

# _GNU_SOURCE for asprintf.
CFLAGS += -D_LARGEFILE_SOURCE -D_LARGEFILE64_SOURCE -D_GNU_SOURCE #-DMAP_IN_ADVANCE -DCONFIG_HAS_AIO

CFLAGS += -Wall -Werror -g -O1

ifeq ($(shell uname),Linux)
LDLIBS := -lutil -lrt
endif

LDLIBS += -lxenstore -lxenctrl -lpthread \
	-lxenforeignmemory -lxenevtchn -lxendevicemodel -lxengnttab #-laio

# Get gcc to generate the dependencies for us.
CFLAGS   += -Wp,-MD,$(@D)/.$(@F).d

SUBDIRS  = $(filter-out ./,$(dir $(OBJS) $(LIBS)))
DEPS     = .*.d

LDFLAGS += -g

all: $(TARGET)

$(TARGET): $(LIBS) $(OBJS)
	$(CC) -o $@ $(LDFLAGS) $(OBJS) $(LIBS) $(LDLIBS)

%.o: %.c
	$(CC) -o $@ $(CFLAGS) -c $<

.PHONY: ALWAYS

clean:
#	$(foreach dir,$(SUBDIRS),make -C $(dir) clean)
	rm -f $(OBJS)
	rm -f $(DEPS)
	rm -f $(TARGET)

.PHONY: install
install: $(TARGET)
	$(INSTALL) -d $(DESTDIR)$(PREFIX)
	$(INSTALL) -m 755 $(TARGET) $(DESTDIR)$(PREFIX)/$(TARGET)

-include $(DEPS)

print-%:
	echo $($*)
