CC ?= cc
PKG_CONFIG ?= pkg-config
WAYLAND_SCANNER ?= wayland-scanner

PROTO_DIR = protocol
PROTO_XDG = ${PROTO_DIR}/xdg-shell.xml
PROTO_XDG_CLIENT_H = ${PROTO_DIR}/xdg-shell-client-protocol.h
PROTO_XDG_CLIENT_C = ${PROTO_DIR}/xdg-shell-protocol.c

PKG_CFLAGS != $(PKG_CONFIG) --cflags wayland-client pixman-1 2>/dev/null || :
PKG_LIBS != $(PKG_CONFIG) --libs wayland-client pixman-1 2>/dev/null || :

CFLAGS ?= -O2
CFLAGS += -std=c99 -Wall -Wextra -Wpedantic
CFLAGS += -I.. -I./protocol
CFLAGS += ${PKG_CFLAGS}
CPPFLAGS ?= -D_POSIX_C_SOURCE=200809L

LDFLAGS ?=
LDFLAGS += -Wl,-rpath,'$$ORIGIN/..'

LDLIBS += -L.. -lwld
LDLIBS += ${PKG_LIBS}
LDLIBS += -lm

SOURCES = \
	swiv.c \
	image.c \
	${PROTO_XDG_CLIENT_C}

OBJECTS = ${SOURCES:.c=.o}

.PHONY: all clean
.SUFFIXES:
.SUFFIXES: .c .o
PREFIX ?= /usr/local
BINDIR ?= ${PREFIX}/bin

all: swiv

${PROTO_XDG_CLIENT_H} ${PROTO_XDG_CLIENT_C}: ${PROTO_XDG}
	${WAYLAND_SCANNER} client-header ${PROTO_XDG} ${PROTO_XDG_CLIENT_H}
	${WAYLAND_SCANNER} public-code ${PROTO_XDG} ${PROTO_XDG_CLIENT_C}

swiv.o: ${PROTO_XDG_CLIENT_H}

.c.o:
	${CC} ${CFLAGS} ${CPPFLAGS} -c -o $@ $<

swiv: $(OBJECTS)
	${CC} ${LDFLAGS} -o $@ ${OBJECTS} ${LDLIBS}

clean:
	rm -f $(OBJECTS) swiv ${PROTO_XDG_CLIENT_H} ${PROTO_XDG_CLIENT_C}

install: swiv
	install -D -m 755 swiv ${DESTDIR}${BINDIR}/swiv

compile_flags:
	@rm -f compile_flags.txt
	@for f in ${CFLAGS} ${CPPFLAGS} ${LDFLAGS} ${LDLIBS} ; do echo $$f >> compile_flags.txt; done
