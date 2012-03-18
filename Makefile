PACKAGE          ?= mozilla-gnome-keyring
VERSION          ?= $(shell git describe --tags 2>/dev/null || date +dev-%s)
# max/min compatibility versions to set, only if "xulrunner" tool is not available
XUL_VER_MIN      ?= 10.0.1
XUL_VER_MAX      ?= 10.*
# package distribution variables
FULLNAME         ?= $(PACKAGE)-$(VERSION)
ARCHIVENAME      ?= $(FULLNAME)


# xulrunner tools. use = not ?= so we don't execute on every invocation
XUL_PKG_NAME     = $(shell (pkg-config --atleast-version=2 libxul && echo libxul) \
                        || (pkg-config libxul2                    && echo libxul2))

# compilation flags

# if pkgconfig file for libxul is available, use it
ifdef XUL_PKG_NAME
XUL_CFLAGS       := `pkg-config --cflags $(XUL_PKG_NAME)`
XUL_LDFLAGS      := `pkg-config --libs $(XUL_PKG_NAME)`
XPCOM_ABI_FLAGS  += -Wl`pkg-config --libs-only-L $(XUL_PKG_NAME) | sed -e 's/-L\(\S*\).*/,-rpath=\1/'`
endif

GNOME_CFLAGS     := `pkg-config --cflags gnome-keyring-1`
GNOME_LDFLAGS    := `pkg-config --libs gnome-keyring-1`
CXXFLAGS         += -Wall -fno-rtti -fno-exceptions -fPIC -std=gnu++0x

# determine xul version from "mozilla-config.h" include file
XUL_VERSION      = $(shell echo '\#include "mozilla-config.h"'| \
                     $(CXX) $(XUL_CFLAGS) $(CXXFLAGS) -shared -x c++ -w -E -fdirectives-only - | \
                     sed -n -e 's/\#[[:space:]]*define[[:space:]]\+MOZILLA_VERSION[[:space:]]\+\"\(.*\)\"/\1/gp')

# platform-specific handling
# lazy variables, instantiated properly in a sub-make since make doesn't
# support dynamically adjusting the dependency tree during its run
PLATFORM         ?= unknown

TARGET           := libgnomekeyring.so
XPI_TARGET       := $(FULLNAME).xpi
BUILD_FILES      := \
xpi/platform/$(PLATFORM)/components/$(TARGET) \
xpi/install.rdf \
xpi/chrome.manifest


.PHONY: all build build-xpi tarball
all: build

build: build-xpi

build-xpi: xpcom_abi
ifeq "$(PLATFORM)" "unknown"
# set PLATFORM properly in a sub-make
	$(MAKE) $(XPI_TARGET) PLATFORM=`./xpcom_abi || echo unknown`
else
	$(MAKE) $(XPI_TARGET)
endif

$(XPI_TARGET): $(BUILD_FILES)
	cd xpi && zip -rq ../$@ *

xpi/platform/%/components/$(TARGET): $(TARGET)
	mkdir -p $(@D)
	cp -a $< $@

xpi/install.rdf: install.rdf Makefile
	mkdir -p xpi
	XUL_VER_MIN=$(XUL_VERSION); \
	XUL_VER_MAX=`echo $(XUL_VERSION) | sed -rn -e 's/([^.]+).*/\1.*/gp'`; \
	sed -e 's/$${PLATFORM}/'$(PLATFORM)'/g' \
	    -e 's/$${VERSION}/'$(VERSION)'/g' \
	    -e 's/$${XUL_VER_MIN}/'"$${XUL_VER_MIN:-$(XUL_VER_MIN)}"'/g' \
	    -e 's/$${XUL_VER_MAX}/'"$${XUL_VER_MAX:-$(XUL_VER_MAX)}"'/g' \
	    $< > $@

xpi/chrome.manifest: chrome.manifest Makefile
	mkdir -p xpi
	sed -e 's/$${PLATFORM}/'$(PLATFORM)'/g' \
	    $< > $@

$(TARGET): GnomeKeyring.cpp GnomeKeyring.h Makefile
	$(CXX) $< -o $@ -shared \
	    $(XUL_CFLAGS) $(XUL_LDFLAGS) $(GNOME_CFLAGS) $(GNOME_LDFLAGS) $(CXXFLAGS)
	chmod +x $@

xpcom_abi: xpcom_abi.cpp Makefile
	$(CXX) $< -o $@ $(XUL_CFLAGS) $(XUL_LDFLAGS) $(XPCOM_ABI_FLAGS) $(CXXFLAGS)

tarball:
	git archive --format=tar \
	    --prefix=$(FULLNAME)/ HEAD \
	    | gzip - > $(ARCHIVENAME).tar.gz

.PHONY: clean-all clean
clean:
	rm -f $(TARGET)
	rm -f $(XPI_TARGET)
	rm -f xpcom_abi
	rm -f -r xpi

clean-all: clean
	rm -f *.xpi
	rm -f *.tar.gz
