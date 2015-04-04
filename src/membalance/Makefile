# development system requires the following packages: libxen-dev uuid-dev
#             also requires Xen source tree pointed by environment variable XEN

# target deployment system requires: Xen installation with XL toolkit 
#                                    (including package libxen)

# do not run this file in parallel mode (-j), since GNU make does not allow
# correct execution for rules with multiple targets during parallel make
# (such as RPC generator below), and does not have target groups similar
# to Solaris make or IBM clearmake
.NOTPARALLEL:

.PHONY: all clean rebuild skel \
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

ifeq ($(XEN),)
    XEN=~/gits/xen
endif

LIBXL_INC = $(XEN)/tools/libxl

ifeq (,$(wildcard $(LIBXL_INC)/libxlutil.h))
    $(error Please point XEN to the location of Xen source tree)
endif

ifeq ($(CCC),)
    CCC = gcc
endif

ifeq ($(CXX),)
    CXX = g++
endif

MAKE_FLAGS = --no-print-directory

CFLAGS_M = 
CFLAGS_D =
CFLAGS_I =
CFLAGS_W = -Wall -Wno-unused-but-set-variable -Wno-unused-function
CFLAGS_O = 
CFLAGS_G = 
LDFLAGS = -static-libstdc++ -shared-libgcc

ifeq ($(DEBUG),)
    DEBUG = 0
endif

ifeq ($(DEBUG),1)
    CFLAGS_G += -g -ggdb -g3
    CFLAGS_O = -O0
    CFLAGS_O += -fno-unsafe-loop-optimizations -fno-strict-overflow
endif

ifeq ($(DEBUG),0)
    CFLAGS_O = -O2 -flto -finline-functions -fgcse-after-reload -fpredictive-commoning
    CFLAGS_O += -fno-unsafe-loop-optimizations -fno-strict-overflow
    LDGLAGS += -flto -fwhole-program
endif

ifeq ($(DEVEL),1)
    CFLAGS_D += -DDEVEL
endif

LDGLAGS += $(CFLAGS_O)
CXXFLAGS = $(CFLAGS_M) $(CFLAGS_D) $(CFLAGS_I) $(CFLAGS_W) $(CFLAGS_O) $(CFLAGS_G)

SRCS = membalanced.cpp sched.cpp xen.cpp xenstore.cpp domain.cpp util.cpp \
       config.cpp config_parser.cpp rcmd_server.cpp membalancectl.cpp test.cpp

HDRS = membalanced.h config.h config_def.h config_parser.h domain.h \
       domain_info.h test.h

RPC_GEN_SRCS = rcmd_clnt.c rcmd_svc.c rcmd_xdr.c
RPC_GEN_HDRS = rcmd.h
RPC_OPTIONS = -M -N -K -1

LDLIBS = -lrt -lxenstore -lxenlight -lxlutil -lxenctrl -luuid

OBJS := $(patsubst %.cpp,%.o,$(SRCS)) 
RPC_GEN_OBJS := $(patsubst %.c,%.o,$(RPC_GEN_SRCS)) 
ALL_OBJS := $(OBJS) $(RPC_GEN_OBJS)

rebuild:
	@$(MAKE) $(MAKE_FLAGS) clean
	@$(MAKE) $(MAKE_FLAGS) all

all: membalanced membalancectl

membalanced: $(ALL_OBJS)
	$(CXX) $(LDFLAGS) $(ALL_OBJS) $(LDLIBS) -o membalanced
ifeq ($(DEBUG),0)
	strip --strip-all membalanced
endif

$(OBJS): %.o: %.cpp $(HDRS) $(RPC_GEN_HDRS)
	$(CXX) -idirafter $(LIBXL_INC) $(CXXFLAGS) -c $< -o $@

# generate RPC stubs, skeletons and XDR only
$(RPC_GEN_OBJS): %.o: %.c $(RPC_GEN_HDRS)
	$(CCC) $(CXXFLAGS) -Wno-unused-variable -c $< -o $@

$(RPC_GEN_SRCS) $(RPC_GEN_HDRS): rcmd.x rcmd_clnt_x.c
	rm -f $(RPC_GEN_SRCS) $(RPC_GEN_HDRS)
	rpcgen -h $(RPC_OPTIONS) rcmd.x -o rcmd.h
	rpcgen -c $(RPC_OPTIONS) rcmd.x -o rcmd_xdr.c
	rpcgen -l $(RPC_OPTIONS) rcmd.x -o rcmd_clnt.c
	cat rcmd_clnt_x.c >>rcmd_clnt.c
	rpcgen -m $(RPC_OPTIONS) rcmd.x -o rcmd_svc.c

skel:
	rm -rf skel
	mkdir skel
	cd skel && \
	cp ../rcmd.x . && \
	rm -f $(RPC_GEN_SRCS) $(RPC_GEN_HDRS) && \
	rpcgen -a $(RPC_OPTIONS) rcmd.x

membalancectl: membalanced
	ln -f membalanced membalancectl

clean:
	rm -f membalanced membalancectl $(RPC_GEN_SRCS) $(RPC_GEN_HDRS) *.o
	rm -rf skel

install: $(INSTALL-DIST)

uninstall: uninstall-common

install-common: membalanced
	install -p distro/all/etc/membalance.conf /etc
	install -p membalanced /sbin
	ln -f /sbin/membalanced /sbin/membalancectl

uninstall-common: $(UNINSTALL-DIST)
	rm -f /sbin/memsharctl
	rm -f /sbin/membalanced
	rm -f /etc/membalance.conf

install-Ubuntu: install-common
	update-rc.d -f membalance remove
	install -p distro/Ubuntu/etc/init.d/membalance /etc/init.d
	update-rc.d membalance defaults

uninstall-Ubuntu:
	update-rc.d -f membalance remove
	rm -f /etc/init.d/membalance

install-SuSE: install-common
	[ -e /etc/init.d/membalance ] && chkconfig --del membalance || exit 0
	install -p distro/SuSE/etc/init.d/membalance /etc/init.d
	chkconfig membalance on

uninstall-SuSE:
	[ -e /etc/init.d/membalance ] && chkconfig membalance off || exit 0
	[ -e /etc/init.d/membalance ] && chkconfig --del membalance || exit 0
	rm -f /etc/init.d/membalance

install-unsupported:
	@echo "Installation for this Linux distribution is not supported"
	exit 1

uninstall-unsupported:
	@echo "Uninstallation for this Linux distribution is not supported"
	exit 1

