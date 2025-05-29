CFLAGS = -Wall -Wextra -march=native -mtune=native -O3 -ffast-math -funroll-loops
INCLUDES = $(shell pkg-config --cflags libgphoto2 libgphoto2_port libavformat libavcodec libavutil libswscale)
LIBS = $(shell pkg-config --libs libgphoto2 libgphoto2_port libavformat libavcodec libavutil libswscale)
UNAME_S := $(shell uname -s)
ifneq ($(findstring Linux,$(UNAME_S)),)
    INCLUDES += $(shell pkg-config --cflags libkmod)
    LIBS += $(shell pkg-config --libs libkmod)
    CFLAGS += -DUSE_LIBKMOD
endif

# Installation directories
PREFIX ?= /usr/local
BINDIR = bin
DESTDIR ?=
INSTALL_BINDIR = $(DESTDIR)$(PREFIX)/bin

all: $(BINDIR)/webcamize

$(BINDIR):
	mkdir -p $(BINDIR)

$(BINDIR)/webcamize: webcamize.c | $(BINDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $< $(LIBS)

install: $(BINDIR)/webcamize
	install -d $(INSTALL_BINDIR)
	install -m 755 $(BINDIR)/webcamize $(INSTALL_BINDIR)/webcamize

uninstall:
	rm -f $(INSTALL_BINDIR)/webcamize

clean:
	rm -rf $(BINDIR)

.PHONY: all clean install uninstall
