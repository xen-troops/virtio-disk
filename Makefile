TARGET = demu

OBJS :=	device.o \
	pci.o \
	demu.o

CFLAGS  = -I$(shell pwd)/include

# _GNU_SOURCE for asprintf.
CFLAGS += -D_LARGEFILE_SOURCE -D_LARGEFILE64_SOURCE -D_GNU_SOURCE 
CFLAGS += -Wall -Werror -g -O1 

ifeq ($(shell uname),Linux)
LDLIBS := -lutil -lrt
endif

LDLIBS += -lxenstore -lxenctrl

# Get gcc to generate the dependencies for us.
CFLAGS   += -Wp,-MD,$(@D)/.$(@F).d

SUBDIRS  = $(filter-out ./,$(dir $(OBJS) $(LIBS)))
DEPS     = .*.d

LDFLAGS := -g 

all: $(TARGET)

$(TARGET): $(LIBS) $(OBJS)
	gcc -o $@ $(LDFLAGS) $(OBJS) $(LIBS) $(LDLIBS)

%.o: %.c
	gcc -o $@ $(CFLAGS) -c $<

.PHONY: ALWAYS

clean:
	$(foreach dir,$(SUBDIRS),make -C $(dir) clean)
	rm -f $(OBJS)
	rm -f $(DEPS)
	rm -f $(TARGET)
	rm -f TAGS

.PHONY: TAGS
TAGS:
	find . -name \*.[ch] | etags -

-include $(DEPS)

print-%:
	echo $($*)
