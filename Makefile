CFLAGS := -g -Wall

proxy: proxy.o

clean:
	rm -f proxy.o proxy
