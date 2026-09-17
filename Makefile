CC      ?= gcc
PKG     := gtk+-3.0
CFLAGS  ?= -O2 -Wall -Wextra
CFLAGS  += $(shell pkg-config --cflags $(PKG))
LDLIBS  += $(shell pkg-config --libs $(PKG)) -pthread -lm

.PHONY: all clean install uninstall install-deps

all: headset-recorder

headset-recorder: main.c
	$(CC) $(CFLAGS) -o $@ main.c $(LDLIBS)

install: headset-recorder
	./install.sh

uninstall:
	./install.sh --uninstall

install-deps:
	sudo apt-get update
	sudo apt-get install -y build-essential make pkg-config libgtk-3-dev ffmpeg

clean:
	rm -f headset-recorder
