CC = gcc
CFLAGS = -Wall -g
LIBS = -lxcb-randr -lxcb -lm

xcubiclight: xcubiclight.o

%: %.o
	$(CC) $(LDFLAGS) -o $@ $^ $(LIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f *.o
