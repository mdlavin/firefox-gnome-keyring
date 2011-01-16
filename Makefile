CPPFLAGS += -fno-rtti -fno-exceptions -shared -fPIC

DEPENDENCY_CFLAGS = `pkg-config --cflags libxul gnome-keyring-1`
GNOME_LDFLAGS     = `pkg-config --libs gnome-keyring-1`
XUL_LDFLAGS       = `pkg-config --libs libxul ` -L lib/i386
VERSION           = 0.4.2
FILES             = GnomeKeyring.cpp

TARGET = libgnomekeyring.so
XPI_TARGET = gnome-keyring_password_integration-$(VERSION).xpi

build-xpi: build-library-x86_64 build-library-x86
	mkdir -p xpi
	cp install.rdf xpi/install.rdf
	sed -i 's/<em:version>.*<\/em:version>/<em:version>$(VERSION)<\/em:version>/' xpi/install.rdf
	cd xpi && zip -r ../$(XPI_TARGET) *


build-library-x86_64: ARCH=x86_64
build-library-x86_64: $(FILES) Makefile
	mkdir -p xpi/platform/Linux_$(ARCH)-gcc3/components
	$(CXX) $(FILES) -g -Wall -o xpi/platform/Linux_$(ARCH)-gcc3/components/$(TARGET) $(DEPENDENCY_CFLAGS) $(XUL_LDFLAGS) $(GNOME_LDFLAGS) $(CPPFLAGS) $(CXXFLAGS) $(GECKO_DEFINES)
	chmod +x xpi/platform/Linux_$(ARCH)-gcc3/components/$(TARGET)


build-library-x86: ARCH=x86
build-library-x86: CPPFLAGS:=$(CPPFLAGS) -m32
build-library-x86: $(FILES) Makefile
	mkdir -p xpi/platform/Linux_$(ARCH)-gcc3/components
	$(CXX) $(FILES) -g -Wall -o xpi/platform/Linux_$(ARCH)-gcc3/components/$(TARGET) $(DEPENDENCY_CFLAGS) $(XUL_LDFLAGS) $(GNOME_LDFLAGS) $(CPPFLAGS) $(CXXFLAGS) $(GECKO_DEFINES)
	chmod +x xpi/platform/Linux_$(ARCH)-gcc3/components/$(TARGET)


build: build-xpi

clean:
	rm $(TARGET)
	rm -r xpi/
	rm gnome-keyring_password_integration-$(VERSION).xpi
