CC = gcc
CFLAGS = -Wall -g
LIBS = -lxcb-randr -lxcb -lm

prefix = /usr
bindir = $(prefix)/bin
man1dir = $(prefix)/share/man1

xcubiclight: xcubiclight.o

%: %.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

install:
	install -m 0755 -Dt $(DESTDIR)$(bindir)/ xcubiclight
	install -m 0644 -Dt $(DESTDIR)$(man1dir)/ xcubiclight.1

clean:
	rm -f *.o
