# Makefile for famez

CONFIG_FAMEZ ?= m

KERNELBASE = /lib/modules/$(shell uname -r)
KERNELDIR ?= $(KERNELBASE)/build
SHELL=/bin/bash
PWD:=$(shell /bin/pwd)

# wait_queue_head(), among others, didn't show up until 4.13
VMUST:=4
PMIN:=13
V:=$(shell make --no-print-directory -C ${KERNELDIR} kernelversion | cut -d. -f1)
P:=$(shell make --no-print-directory -C ${KERNELDIR} kernelversion | cut -d. -f2)

obj-$(CONFIG_FAMEZ) += famez.o famez_bridge.o

# famez_pci_base.c has the MODULE declarations

famez-objs := famez_pci_base.o famez_config.o famez_IVSHMSG.o \
	famez_register.o famez_MSI-X.o

famez_bridge-objs := fz_bridge.o

RUNNING_ARCH := $(shell dpkg-architecture -qDEB_BUILD_ARCH_CPU 2>/dev/null)

all:	modules

modules:	versioncheck
ifeq "$(RUNNING_ARCH)" "amd64"
	make $(CFLAGS) -C $(KERNELDIR) M=$(PWD) ARCH=x86 modules
else
	make $(CFLAGS) -C $(KERNELDIR) M=$(PWD) modules
endif

modules_install:	modules
	INSTALL_MOD_DIR=genz/famez sudo -E make V=1 -C $(KERNELDIR) M=$(PWD) modules_install
	sudo -E depmod -a

clean:
ifeq "$(architecture)" "amd64"
	make -C $(KERNELDIR) M=$(PWD) ARCH=x86 clean
else
	make -C $(KERNELDIR) M=$(PWD) clean
endif

versioncheck:
	@[ $V -eq ${VMUST} -a $P -ge ${PMIN} ] && exit 0; \
	 echo Kernel headers are $V.$P, need \>= ${VMUST}.${PMIN}; exit 1

