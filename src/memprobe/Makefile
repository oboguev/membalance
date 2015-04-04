# development system requires the following packages: libxen-dev uuid-dev
# target deployment system requires: libxenstore (e.g. libxenstore3.0)

.PHONY: all clean rebuild misc \
        install uninstall \
        install-common uninstall-common \
        install-unsupported uninstall-unsupported \
        install-SuSE uninstall-SuSE \
        install-Ubuntu uninstall-Ubuntu

.DEFAULT_GOAL := all

DIST := $(shell lsb_release -i)
DIST := $(subst Distributor ID:,,$(DIST))
DIST := $(strip $(DIST))

ifeq ($(DIST),Ubuntu) 
    INSTALL-DIST := install-Ubuntu
    UNINSTALL-DIST := uninstall-Ubuntu
endif

ifeq ($(DIST),SUSE LINUX) 
    INSTALL-DIST := install-SuSE
    UNINSTALL-DIST := uninstall-SuSE
endif

ifeq ($(INSTALL-DIST),)
    INSTALL-DIST := install-unsupported
endif 

ifeq ($(UNINSTALL-DIST),)
    UNINSTALL-DIST := uninstall-unsupported
endif 

MAKE_FLAGS = --no-print-directory

CFLAGS_M = 
CFLAGS_D =
CFLAGS_I =
CFLAGS_W = -Wall -Wno-unused-but-set-variable
CFLAGS_O = 
CFLAGS_G = 
LDFLAGS = -shared-libgcc

ifeq ($(DEBUG),)
    DEBUG = 0
endif

ifeq ($(DEBUG),1)
    CFLAGS_G += -g -ggdb -g3
    CFLAGS_O = -O0
    CFLAGS_O += -fno-unsafe-loop-optimizations -fno-strict-overflow
endif

ifeq ($(DEBUG),0)
    CFLAGS_O = -O2 -finline-functions -fgcse-after-reload -fpredictive-commoning
    CFLAGS_O += -fno-unsafe-loop-optimizations -fno-strict-overflow
    #CFLAGS_O += -flto
    #LDGLAGS += -flto -fwhole-program #$(CFLAGS_O)
endif

ifeq ($(DEVEL),1)
    CFLAGS_D += -DDEVEL
endif

CFLAGS = $(CFLAGS_M) $(CFLAGS_D) $(CFLAGS_I) $(CFLAGS_W) $(CFLAGS_O) $(CFLAGS_G)

rebuild: 
	@$(MAKE) $(MAKE_FLAGS) clean
	@$(MAKE) $(MAKE_FLAGS) all

all: memprobed misc

memprobed: memprobed.c
	gcc -o memprobed $(CFLAGS) $(LDFLAGS) memprobed.c -lrt -lxenstore
ifeq ($(DEBUG),0)
	strip --strip-all memprobed
endif

clean:
	rm -f memprobed

misc:
	chmod a+x xenfs.sh

install: $(INSTALL-DIST)

uninstall: uninstall-common

install-common: memprobed
	install -p memprobed /sbin

uninstall-common: $(UNINSTALL-DIST)
	rm -f /sbin/memprobed

install-Ubuntu: install-common
	update-rc.d -f memprobe remove
	install -p distro/Ubuntu/etc/init.d/memprobe /etc/init.d
	update-rc.d memprobe defaults

uninstall-Ubuntu:
	update-rc.d -f memprobe remove
	rm -f /etc/init.d/memprobe

install-SuSE: install-common
	[ -e /etc/init.d/memprobe ] && chkconfig --del memprobe || exit 0
	install -p distro/SuSE/etc/init.d/memprobe /etc/init.d
	chkconfig memprobe on

uninstall-SuSE:
	[ -e /etc/init.d/memprobe ] && chkconfig memprobe off || exit 0
	[ -e /etc/init.d/memprobe ] && chkconfig --del memprobe || exit 0
	rm -f /etc/init.d/memprobe

install-unsupported:
	@echo "Installation for this Linux distribution is not supported"
	exit 1

uninstall-unsupported:
	@echo "Uninstallation for this Linux distribution is not supported"
	exit 1

