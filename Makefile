CFLAGS += $(shell pkg-config --cflags libsystemd-daemon)
LDFLAGS += $(shell pkg-config --cflags --libs libsystemd-daemon)

lightserver: lightserver.o

install: lightserver
	mkdir -p $(DESTDIR)/usr/sbin
	cp $< $(DESTDIR)/usr/sbin

clean:
	rm -f lightserver lightserver.o
