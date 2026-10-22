# Standalone tests in this directory. Not part of the Electrobun build —
# that lives in ../../../build.ts. These build directly for quick Phase 2
# iteration from the Pi.

CXX      ?= g++
CXXFLAGS := -Wall -Wextra -O2 -g -std=c++17 -Wno-missing-field-initializers
PKGS     := libdrm cairo libinput libudev glib-2.0 gio-unix-2.0
CFLAGS   += $(shell pkg-config --cflags $(PKGS))
LIBS     := $(shell pkg-config --libs $(PKGS))

WPE_PKGS := wpe-1.0 wpebackend-fdo-1.0 wpe-webkit-2.0 wayland-server
WPE_CFLAGS := $(shell pkg-config --cflags $(WPE_PKGS))
WPE_LIBS   := $(shell pkg-config --libs $(WPE_PKGS))

all: drm_hello wpe_hello

drm_hello: drm_hello.cpp drm_display.cpp drm_display.h
	$(CXX) $(CXXFLAGS) $(CFLAGS) -I.. -I../../shared -o $@ drm_hello.cpp drm_display.cpp $(LIBS)

wpe_hello: wpe_hello.cpp drm_display.cpp drm_display.h input.cpp input.h
	$(CXX) $(CXXFLAGS) $(CFLAGS) $(WPE_CFLAGS) -I.. -I../../shared -o $@ wpe_hello.cpp drm_display.cpp input.cpp $(LIBS) $(WPE_LIBS)

clean:
	rm -f drm_hello

# Run on tty2. Use `make back` to return the display to tty1 afterwards.
run: drm_hello
	sudo openvt -c 2 -s -f -- $(CURDIR)/drm_hello

back:
	sudo chvt 1

.PHONY: all clean run back
