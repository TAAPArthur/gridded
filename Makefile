
CFLAGS ?= -std=c99 -Wall -pedantic
CPPFLAGS += -D_POSIX_C_SOURCE=200112L
PREFIX ?= /usr
LDLIBS = -lxcb -lxcb-keysyms
BIN = gridded

gridded: $(BIN).o
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

install:
	install -Dt $(DESTDIR)$(PREFIX)/bin $(BIN)

uninstall:
	rm $(DESTDIR)$(PREFIX)/bin/$(BIN)

clean:
	rm *.o $(BIN)

.PHONY: all clean install uninstall
