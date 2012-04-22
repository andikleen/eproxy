CFLAGS := -g -Wall

eproxy: eproxy.o

clean:
	rm -f eproxy.o eproxy
