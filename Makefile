CC ?= cc
PKG_CONFIG ?= pkg-config
WAYLAND_SCANNER ?= wayland-scanner

PROTO_DIR = protocol
PROTO_XDG = ${PROTO_DIR}/xdg-shell.xml
PROTO_XDG_CLIENT_H = ${PROTO_DIR}/xdg-shell-client-protocol.h
PROTO_XDG_CLIENT_C = ${PROTO_DIR}/xdg-shell-protocol.c

.if defined(.MAKE)
PKG_CFLAGS != ${PKG_CONFIG} --cflags wayland-client pixman-1
PKG_LIBS != ${PKG_CONFIG} --libs wayland-client pixman-1
.else
PKG_CFLAGS := $(shell $(PKG_CONFIG) --cflags wayland-client pixman-1)
PKG_LIBS := $(shell $(PKG_CONFIG) --libs wayland-client pixman-1)
.endif

CFLAGS ?= -O2 -g
CFLAGS += -std=c99 -Wall -Wextra -Wpedantic
CFLAGS += -I.. -I./protocol
CFLAGS += ${PKG_CFLAGS}

LDFLAGS ?=
LDFLAGS += -Wl,-rpath,'$$ORIGIN/..'

LDLIBS += -L.. -lwld
LDLIBS += ${PKG_LIBS}

SOURCES = \
	wiv.c \
	image.c \
	${PROTO_XDG_CLIENT_C}

OBJECTS = ${SOURCES:.c=.o}

.PHONY: all clean
PREFIX ?= /usr/local
BINDIR ?= ${PREFIX}/bin

all: wiv

${PROTO_XDG_CLIENT_H} ${PROTO_XDG_CLIENT_C}: ${PROTO_XDG}
	${WAYLAND_SCANNER} client-header ${PROTO_XDG} ${PROTO_XDG_CLIENT_H}
	${WAYLAND_SCANNER} public-code ${PROTO_XDG} ${PROTO_XDG_CLIENT_C}

wiv.o: ${PROTO_XDG_CLIENT_H}

protocol/xdg-shell-protocol.o: protocol/xdg-shell-protocol.c
	${CC} ${CFLAGS} ${CPPFLAGS} -c -o $@ $<

wiv: $(OBJECTS)
	${CC} ${LDFLAGS} -o $@ ${OBJECTS} ${LDLIBS}

clean:
	rm -f $(OBJECTS) wiv ${PROTO_XDG_CLIENT_H} ${PROTO_XDG_CLIENT_C}

install: wiv
	install -D -m 755 wiv ${DESTDIR}${BINDIR}/wiv
