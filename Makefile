CC = gcc
CFLAGS = -O2 -Wall
LIBS = -lncurses

fatmap: main.c
	$(CC) $(CFLAGS) -o fatmap main.c $(LIBS)

install:
	install -m 755 fatmap $(DESTDIR)/usr/bin/fatmap

clean:
	rm -f fatmap