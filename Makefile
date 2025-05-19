CFLAGS = -Wall -Wextra -Ofast -march=native -mtune=native
INCLUDES = $(shell pkg-config --cflags libv4l2 libgphoto2 libgphoto2_port libavformat libavcodec libavutil libswscale)
LIBS = $(shell pkg-config --libs libv4l2 libgphoto2 libgphoto2_port libavformat libavcodec libavutil libswscale)
BINDIR = bin

all: $(BINDIR)/webcamize

$(BINDIR):
	mkdir -p $(BINDIR)

$(BINDIR)/webcamize: webcamize.c | $(BINDIR)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $< $(LIBS)

clean:
	rm -rf $(BINDIR)

.PHONY: all clean
