
# Comment/uncomment the following line to enable/disable debugging
DEBUG = y

ifeq ($(DEBUG),y)
  DEBFLAGS = -O -g -DSCULLV_DEBUG # "-O" is needed to expand inlines
else
  DEBFLAGS = -O2
endif

EXTRACFLAGS += $(DEBFLAGS) -I$(LDDINC)

TARGET = tfs

ifneq ($(KERNELRELEASE),)

tfs-objs := super.o inode.o alloc.o dir.o file.o

obj-m	:= tfs.o

else

KERNELDIR ?= /lib/modules/$(shell uname -r)/build
#KERNELDIR ?= /lib/modules/2.6.28.10/build
PWD       := $(shell pwd)

modules:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) LDDINC=$(PWD) modules

endif


install:
	install -d $(INSTALLDIR)
	install -c $(TARGET).o $(INSTALLDIR)

clean:
	rm -rf *.o *~ core .depend .*.cmd *.ko *.mod.c .tmp_versions


depend .depend dep:
	$(CC) $(CFLAGS) -M *.c > .depend

ifeq (.depend,$(wildcard .depend))
include .depend
endif
