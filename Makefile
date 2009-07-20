CXX   = g++
CPPFLAGS +=     -fno-rtti              \
		-fno-exceptions        \
		-shared 

DEPENDENCY_CFLAGS = `pkg-config --cflags libxul libxul-unstable gnome-keyring-1`
GNOME_LDFLAGS = `pkg-config --libs gnome-keyring-1`
XUL_LDFLAGS = `pkg-config --libs libxul libxul-unstable`

FILES = GnomeKeyring.cpp 

TARGET = libgnomekeyring.so
XPI_TARGET = gnome-keyring_password_integration-0.3.xpi


build-xpi: build-library
	mkdir -p xpi/platform/Linux_x86-gcc3/components
	cp $(TARGET) xpi/platform/Linux_x86-gcc3/components
	cd xpi && zip -r ../$(XPI_TARGET) *

build-library: 
	$(CXX) $(FILES) -g -Wall -o $(TARGET) $(DEPENDENCY_CFLAGS) $(XUL_LDFLAGS) $(GNOME_LDFLAGS) $(CPPFLAGS) $(CXXFLAGS) $(GECKO_DEFINES)
	chmod +x $(TARGET)
#	strip $(TARGET)

build: build-library build-xpi
 
clean: 
	rm $(TARGET)
	rm xpi/platform/Linux_x86-gcc3/components/$(TARGET)
	rm gnome-keyring_password_integration-0.3.xpi