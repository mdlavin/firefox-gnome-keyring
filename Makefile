PACKAGE          ?= firefox-gnome-keyring
VERSION          ?= $(shell git describe --tags 2>/dev/null || date +dev-%s)
# max/min compatibility versions to set, only if "xulrunner" tool is not available
XUL_VER_MIN      ?= 6.0.1
XUL_VER_MAX      ?= 6.*
# package distribution variables
FULLNAME         ?= $(PACKAGE)-$(VERSION)
ARCHIVENAME      ?= $(FULLNAME)


# xulrunner tools. use = not ?= so we don't execute on every invocation
XUL_PKG_NAME     = $(shell (pkg-config --atleast-version=2 libxul && echo libxul) \
                        || (pkg-config libxul2                    && echo libxul2))

# compilation flags

# if pkgconfig file for libxul is available, use it
ifdef XUL_PKG_NAME
XUL_CFLAGS       := `pkg-config --cflags $(XUL_PKG_NAME)` -DMOZ_NO_MOZALLOC
XUL_LDFLAGS      := `pkg-config --libs $(XUL_PKG_NAME) | sed 's/xpcomglue_s/xpcomglue_s_nomozalloc/' | sed 's/-lmozalloc//'`
endif

GNOME_CFLAGS     := `pkg-config --cflags gnome-keyring-1`
GNOME_LDFLAGS    := `pkg-config --libs gnome-keyring-1`
CXXFLAGS         += -fno-rtti -fno-exceptions -shared -fPIC -g -std=gnu++0x

# determine xul version from "mozilla-config.h" include file
XUL_VERSION      = $(shell echo '\#include "mozilla-config.h"'| \
                     g++ $(XUL_CFLAGS) $(CXXFLAGS) -x c++ -w -E -fdirectives-only - | \
                     sed -n -e 's/\#[[:space:]]*define[[:space:]]\+MOZILLA_VERSION[[:space:]]\+\"\(.*\)\"/\1/gp')

# construct Mozilla architectures string
ARCH             := $(shell uname -m)
ARCH             := $(shell echo ${ARCH} | sed 's/i686/x86/')
PLATFORM         := $(shell uname)_$(ARCH)-gcc3

TARGET           := libgnomekeyring.so
XPI_TARGET       := gnome-keyring_password_integration-$(VERSION).xpi

BUILD_FILES      := \
xpi/platform/$(PLATFORM)/components/$(TARGET) \
xpi/install.rdf \
xpi/chrome.manifest


.PHONY: all build build-xpi tarball
all: build

build: build-xpi

build-xpi: $(XPI_TARGET)

$(XPI_TARGET): $(BUILD_FILES)
	cd xpi && zip -rq ../$@ *

xpi/platform/$(PLATFORM)/components/$(TARGET): $(TARGET)
	mkdir -p xpi/platform/$(PLATFORM)/components
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
	$(CXX) $< -g -Wall -o $@ \
	    $(XUL_CFLAGS) $(XUL_LDFLAGS) $(GNOME_CFLAGS) $(GNOME_LDFLAGS) $(CXXFLAGS)
	chmod +x $@

tarball:
	git archive --format=tar \
	    --prefix=$(FULLNAME)/ HEAD \
	    | gzip - > $(ARCHIVENAME).tar.gz

.PHONY: clean-all clean
clean:
	rm -f $(TARGET)
	rm -f $(XPI_TARGET)
	rm -f -r xpi

clean-all: clean
	rm -f *.xpi
	rm -f *.tar.gz

