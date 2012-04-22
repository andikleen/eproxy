CFLAGS := -g -Wall -O2

eproxy: eproxy.o list.h

clean:
	rm -f eproxy.o eproxy
