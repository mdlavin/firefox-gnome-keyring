CPPFLAGS += -fno-rtti -fno-exceptions -shared -fPIC -g

XUL_PKG_NAME := $(shell (pkg-config --atleast-version=2.0 libxul && echo libxul) || (pkg-config libxul2 && echo libxul2) || (echo libxul-is-missing))

DEPENDENCY_CFLAGS = `pkg-config --cflags libxul gnome-keyring-1` -DMOZ_NO_MOZALLOC
GNOME_LDFLAGS     = `pkg-config --libs gnome-keyring-1`
XUL_LDFLAGS       = `pkg-config --libs ${XUL_PKG_NAME} | sed 's/xpcomglue_s/xpcomglue_s_nomozalloc/' | sed 's/-lmozalloc//'`
PLATFORM          = `gcc --version --verbose 2>&1 | grep 'Target:' | cut '-d ' -f2`
VERSION           = `git describe --tags`
FILES             = GnomeKeyring.cpp

TARGET = libgnomekeyring.so
XPI_TARGET = gnome-keyring_password_integration-$(VERSION).xpi

build-xpi: build-library
	mkdir -p xpi
	sed -e 's/$${PLATFORM}/'$(PLATFORM)'/g' \
	    -e 's/$${VERSION}/'$(VERSION)'/g' \
	    install.rdf > xpi/install.rdf
	sed -e 's/$${PLATFORM}/'$(PLATFORM)'/g' \
	    chrome.manifest > xpi/chrome.manifest
	cd xpi && zip -r ../$(XPI_TARGET) *

build-library: $(FILES) Makefile
	mkdir -p xpi/platform/$(PLATFORM)/components
	$(CXX) $(FILES) -g -Wall -o xpi/platform/$(PLATFORM)/components/$(TARGET) \
	    $(DEPENDENCY_CFLAGS) $(XUL_LDFLAGS) $(GNOME_LDFLAGS) $(CPPFLAGS) \
	    $(CXXFLAGS) $(GECKO_DEFINES)
	chmod +x xpi/platform/$(PLATFORM)/components/$(TARGET)

build: build-xpi

all: build

clean:
	rm -f $(TARGET)
	rm -f -r xpi
	rm -f gnome-keyring_password_integration-$(VERSION).xpi
