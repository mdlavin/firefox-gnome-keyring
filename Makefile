CXX       = g++
CPPFLAGS += -fno-rtti -fno-exceptions -shared -fPIC

DEPENDENCY_CFLAGS = `pkg-config --cflags libxul gnome-keyring-1`
GNOME_LDFLAGS     = `pkg-config --libs gnome-keyring-1`
XUL_LDFLAGS       = `pkg-config --libs libxul `
VERSION           = 0.3
FILES             = GnomeKeyring.cpp

TARGET = libgnomekeyring.so
XPI_TARGET = gnome-keyring_password_integration-$(VERSION).xpi
ARCH := $(shell uname -m)
# Update the ARCH variable so that the Mozilla architectures are used
ARCH := $(shell echo ${ARCH} | sed 's/i686/x86/')

build-xpi: build-library
	sed -i 's/<em:version>.*<\/em:version>/<em:version>$(VERSION)<\/em:version>/' xpi/install.rdf
	sed -i 's/<em:targetPlatform>.*<\/em:targetPlatform>/<em:targetPlatform>Linux_$(ARCH)-gcc3<\/em:targetPlatform>/' xpi/install.rdf
	mkdir -p xpi/platform/Linux_$(ARCH)-gcc3/components
	cp $(TARGET) xpi/platform/Linux_$(ARCH)-gcc3/components
	cd xpi && zip -r ../$(XPI_TARGET) *

build-library:
	$(CXX) $(FILES) -g -Wall -o $(TARGET) $(DEPENDENCY_CFLAGS) $(XUL_LDFLAGS) $(GNOME_LDFLAGS) $(CPPFLAGS) $(CXXFLAGS) $(GECKO_DEFINES)
	chmod +x $(TARGET)

build: build-library build-xpi

clean:
	rm $(TARGET)
	rm xpi/platform/Linux_$(ARCH)-gcc3/components/$(TARGET)
	rm gnome-keyring_password_integration-$(VERSION).xpi
