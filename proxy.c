/* port forwarder */
/* proxy inport outip outport */
/* Uses pipes to splice two sockets together. We (hope) this gives something
   approaching zero copy. */
#define _GNU_SOURCE 1
#include <sys/socket.h>
#include <sys/epoll.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <unistd.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdbool.h>
#include <errno.h>
#include <assert.h>

#define err(x) perror(x), exit(1)
#define NEW(x) ((x) = xmalloc(sizeof(*(x))))
#define MAX(a,b) ((a) > (b) ? (a) : (b))

void oom(void)
{
	fprintf(stderr, "Out of memory\n");
	exit(1);
}

void *xmalloc(size_t size)
{
	void *p = calloc(size, 1);
	if (!p)
		oom();
	return p;
}

void *xrealloc(void *old, size_t size)
{
	void *p = realloc(old, size);
	if (!p)
		oom();
	return p;
}

struct addrinfo *resolve(char *name, char *port, int flags)
{
	int ret;
	struct addrinfo *adr;

	ret = getaddrinfo(name, port, 
			  &((struct addrinfo){ .ai_flags = flags}),
			  &adr);
	if (ret) {
		fprintf(stderr, "proxy: Cannot resolve %s %s: %s\n",
			name, port, gai_strerror(ret));
		exit(1);
	}
	return adr;
}

void setnonblock(int fd, int *cache)
{
	int flags;
	if (!cache || *cache == -1) {
	 	flags = fcntl(fd, F_GETFL, 0);
		if (cache)
			*cache = flags;
	} else
		flags = *cache;
	fcntl(fd, F_SETFL, flags|O_NONBLOCK);
}

struct buffer {
	int pipe[2];
	int bytes;
};

struct conn {
	struct conn *other;
	int fd;
	struct buffer buf;
	// XXX timeout
};

#define MIN_EVENTS 32
struct epoll_event *events;
int num_events, max_events;

int epoll_add(int efd, int fd, int revents, void *conn)
{
	struct epoll_event ev = { .events = revents, .data.ptr = conn };
	if (++num_events >= max_events) {
		max_events = MAX(max_events * 2, MIN_EVENTS);
		events = xrealloc(events, 
				  sizeof(struct epoll_event) * max_events);
	}
	return epoll_ctl(efd, EPOLL_CTL_ADD, fd, &ev);
}

int epoll_del(int efd, int fd)
{
	num_events--;
	assert(num_events >= 0);
	return epoll_ctl(efd, EPOLL_CTL_DEL, fd, (void *)1L);
}

/* Create buffer between two connections */
struct buffer *newbuffer(struct buffer *buf)
{
	if (pipe2(buf->pipe, O_NONBLOCK) < 0) {
		perror("pipe");
		return NULL;
	}
	return buf;
}

void delbuffer(struct buffer *buf)
{
	close(buf->pipe[0]);
	close(buf->pipe[1]);
}

void delconn(int efd, struct conn *conn)
{
	delbuffer(&conn->buf);
	epoll_del(efd, conn->fd);
	close(conn->fd);
	free(conn);
}

struct conn *newconn(int efd, int fd)
{
	struct conn *conn;
	NEW(conn);
	conn->fd = fd;
	if (!newbuffer(&conn->buf)) {
		delconn(efd, conn);
		return NULL;
	}
	if (epoll_add(efd, fd, EPOLLIN|EPOLLOUT|EPOLLET, conn) < 0) {
		perror("epoll");
		delconn(efd, conn);
		return NULL;
	}
	return conn;
}

/* Process incoming connection. */
void new_request(int efd, int lfd, int *cache)
{
	int newsk = accept(lfd, NULL, NULL);
	if (newsk < 0) {
		perror("accept");
		return;
	}
	// xxx log
	setnonblock(newsk, cache);
	newconn(efd, newsk);
}

/* Open outgoing connection */
struct conn *openconn(int efd, struct addrinfo *host, int *cache, struct conn *other)
{
	int outfd = socket(host->ai_family, SOCK_STREAM, 0);
	if (outfd < 0)
		return NULL;
	setnonblock(outfd, cache);
	int n = connect(outfd, host->ai_addr, host->ai_addrlen);
	if (n < 0 && errno != EINPROGRESS) {
		perror("connect");
		close(outfd);
		return NULL;
	}
	struct conn *conn = newconn(efd, outfd);
	if (conn) {
		conn->other = other;
		other->other = conn;
	}
	return conn;
}

#define BUFSZ 16384 /* XXX */

/* Move from socket to pipe */
void move_data_in(int srcfd, struct buffer *buf)
{
	/* XXX what happens when the pipe fills. do we get
 	   get another epoll event? need ioctl? */
	for (;;) {	
		int n = splice(srcfd, NULL, buf->pipe[1], NULL, 
			   	BUFSZ, SPLICE_F_NONBLOCK|SPLICE_F_MOVE);
		if (n > 0)
			buf->bytes += n;
		if (n < BUFSZ)
			break;
	}
}

/* From pipe to socket */
void move_data_out(struct buffer *buf, int dstfd)
{ 
	while (buf->bytes > 0) {
		int bytes = buf->bytes;
		if (bytes > BUFSZ)
			bytes = BUFSZ;
		int n = splice(buf->pipe[0], NULL, dstfd, NULL,
		 	 	 bytes, SPLICE_F_NONBLOCK|SPLICE_F_MOVE);
		if (n <= 0)
			break;
		buf->bytes -= n;
	}
}

void closeconn(int efd, struct conn *conn)
{
	if (conn->other)
		delconn(efd, conn->other);
	delconn(efd, conn);		
}

int main(int ac, char **av)
{
	if (ac != 4 && ac != 5) {
		fprintf(stderr, "Usage: proxy inport outhost outport [listenaddr]\n");
		exit(1);
	}

	struct addrinfo *outhost = resolve(av[2], av[3], 0);
	char *lname = av[4] ? av[4] : "0.0.0.0";
	struct addrinfo *laddr = resolve(lname, av[1], AI_PASSIVE);
	
	int lfd = socket(laddr->ai_family, SOCK_STREAM, 0);
	if (lfd < 0) err("socket");
	int opt = 1;
	if (setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(int)) < 0) 
		err("SO_REUSEADDR");
	if (bind(lfd, laddr->ai_addr, laddr->ai_addrlen) < 0) err("bind");
	if (listen(lfd, 20) < 0) err("listen");
	setnonblock(lfd, NULL);
	freeaddrinfo(laddr);

	int efd = epoll_create(10);
	if (efd < 0) err("epoll_create");

	if (epoll_add(efd, lfd, EPOLLIN, NULL) < 0) 
		err("epoll add listen fd");

	int cache_in = -1, cache_out = -1;	
	for (;;) {
		// XXX timeout
		int nfds = epoll_wait(efd, events, num_events, -1);
		if (nfds < 0) {
			perror("epoll");
			continue;
		}
		int i;
		for (i = 0; i < nfds; i++) { 
			struct epoll_event *ev = &events[i];
			struct conn *conn = ev->data.ptr;

			/* listen socket */
			if (conn == NULL) {
				if (ev->events & EPOLLIN)
					new_request(efd, lfd, &cache_in);
				continue;
			} 

			if (ev->events & (EPOLLERR|EPOLLHUP)) {
				closeconn(efd, conn);
				continue;
			}

			if (ev->events & EPOLLIN) {
				if (!conn->other)
					openconn(efd, outhost, &cache_out, conn);
				move_data_in(conn->fd, &conn->buf);
			}	
				
			if ((ev->events & EPOLLOUT) && conn->other)
				move_data_out(&conn->other->buf, conn->fd);
		}	
	}
	return 0;
}

