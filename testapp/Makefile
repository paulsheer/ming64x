CC      = gcc
PKG     = x11
CFLAGS  = -O2 -Wall -Wextra $(shell pkg-config --cflags $(PKG))
LDLIBS  = $(shell pkg-config --static --libs $(PKG))

all: xtest

xtest: xtest.c
	$(CC) $(CFLAGS) -static -o $@ $< $(LDLIBS)

clean:
	rm -f xtest

.PHONY: all clean
