CFLAGS = -Wall -Wextra -Ofast -march=native -mtune=native
INCLUDES = $(shell pkg-config --cflags libgphoto2 libgphoto2_port libavformat libavcodec libavutil libswscale)
LIBS = $(shell pkg-config --libs libgphoto2 libgphoto2_port libavformat libavcodec libavutil libswscale)

UNAME_S := $(shell uname -s)
ifneq ($(findstring Linux,$(UNAME_S)),)
    INCLUDES += $(shell pkg-config --cflags libkmod)
    LIBS += $(shell pkg-config --libs libkmod)
    CFLAGS += -DUSE_LIBKMOD
endif

BINDIR = bin
all: $(BINDIR)/webcamize
$(BINDIR):
	mkdir -p $(BINDIR)
$(BINDIR)/webcamize: webcamize.c | $(BINDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $< $(LIBS)
clean:
	rm -rf $(BINDIR)
.PHONY: all clean
