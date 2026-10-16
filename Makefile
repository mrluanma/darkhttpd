CC?=cc
CFLAGS?=-O
LIBS=`[ \`uname\` = "SunOS" ] && echo -lsocket -lnsl -lsendfile`

all: darkhttpd

darkhttpd: darkhttpd.c upnp.c upnp.h
	$(CC) $(CFLAGS) $(LDFLAGS) $(LIBS) darkhttpd.c upnp.c -o $@

test:
	$(MAKE) -C devel

clean:
	rm -f darkhttpd core darkhttpd.core darkhttpd-static darkhttpd-static.core

.PHONY: all clean
