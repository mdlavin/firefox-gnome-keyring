XUL_PKG_NAME     := $(shell (pkg-config --atleast-version=2 libxul && echo libxul) || (pkg-config libxul2 && echo libxul2))
XULRUNNER        := $(shell find -L $$(dirname $$(pkg-config --libs-only-L $(XUL_PKG_NAME) | tail -c+3)) -name xulrunner)
# versions to set if "xulrunner" tool is not available
XUL_VER_MIN      ?= 6.0.1
XUL_VER_MAX      ?= 6.*

# compilation flags
XUL_CFLAGS       := `pkg-config --cflags $(XUL_PKG_NAME) gnome-keyring-1` -DMOZ_NO_MOZALLOC
XUL_LDFLAGS      := `pkg-config --libs $(XUL_PKG_NAME) | sed 's/xpcomglue_s/xpcomglue_s_nomozalloc/' | sed 's/-lmozalloc//'`
GNOME_LDFLAGS    := `pkg-config --libs gnome-keyring-1`
CPPFLAGS         += -fno-rtti -fno-exceptions -shared -fPIC -g -std=gnu++0x

# construct Mozilla architectures string
ARCH             := $(shell uname -m)
ARCH             := $(shell echo ${ARCH} | sed 's/i686/x86/')
PLATFORM         := $(shell uname)_$(ARCH)-gcc3

VERSION          := $(shell git describe --tags 2>/dev/null || date +dev-%s)
TARGET           := libgnomekeyring.so
XPI_TARGET       := gnome-keyring_password_integration-$(VERSION).xpi

BUILD_FILES      := \
xpi/platform/$(PLATFORM)/components/$(TARGET) \
xpi/install.rdf \
xpi/chrome.manifest


.PHONY: all build build-xpi
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
	XUL_VER_MIN=`$(XULRUNNER) --gre-version`; \
	XUL_VER_MAX=`$(XULRUNNER) --gre-version | sed -rn -e 's/([^.]+).*/\1.*/gp'`; \
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
	test -n $(XUL_PKG_NAME) || { echo "libxul missing" && false; }
	$(CXX) $< -g -Wall -o $@ \
	    $(XUL_CFLAGS) $(XUL_LDFLAGS) $(GNOME_LDFLAGS) $(CPPFLAGS) \
	    $(CXXFLAGS) $(GECKO_DEFINES)
	chmod +x $@

.PHONY: clean-all clean
clean:
	rm -f $(TARGET)
	rm -f $(XPI_TARGET)
	rm -f -r xpi

clean-all: clean
	rm -f *.xpi
