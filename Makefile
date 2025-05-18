ifeq ($(shell which clang 2>/dev/null),)
    CC = gcc
else
    CC = clang
endif

CFLAGS = -O3 -march=native -mtune=native -ffast-math -flto -Wall -Wextra
LDFLAGS = -flto
LIBS = $(shell pkg-config --libs libgphoto2 libgphoto2_port)
CPPFLAGS = $(shell pkg-config --cflags libgphoto2 libgphoto2_port)
BINDIR = bin

all: $(BINDIR)/webcamize

$(BINDIR):
	mkdir -p $(BINDIR)

$(BINDIR)/webcamize: webcamize.c | $(BINDIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $< $(LDFLAGS) $(LIBS)

clean:
	rm -rf $(BINDIR)

.PHONY: all clean
