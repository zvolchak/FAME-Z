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

obj-$(CONFIG_FAMEZ) += famez.o

famez-objs := famez_config.o famez_bridge.o famez_MSI-X.o

RUNNING_ARCH := $(shell dpkg-architecture -qDEB_BUILD_ARCH_CPU 2>/dev/null)

all:	versioncheck
ifeq "$(RUNNING_ARCH)" "amd64"
	make $(CFLAGS) -C $(KERNELDIR) M=$(PWD) ARCH=x86 modules
else
	make $(CFLAGS) -C $(KERNELDIR) M=$(PWD) modules
endif

clean:
ifeq "$(architecture)" "amd64"
	make -C $(KERNELDIR) M=$(PWD) ARCH=x86 clean
else
	make -C $(KERNELDIR) M=$(PWD) clean
endif

install:
	INSTALL_MOD_DIR=l4tm make -C $(KERNELDIR) M=$(PWD) modules_install
	depmod -a

versioncheck:
	@[ $V -eq ${VMUST} -a $P -ge ${PMIN} ] && exit 0; \
	 echo This build needs ${VMUST}.${PMIN} or greater, not $V.$P; exit 1

