#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

/* number of lines to generate on client side */
static const unsigned int nlines = 10;

struct client {
	char buf[1024]; // bufer
	size_t nread; // number of buffered bytes
	int	sock; // client socket
	int	cnum; // client serial number
};

static int	proceed_client(struct client *c);
static void	server_loop(int servsock);
static void	client_loop(int csock, unsigned int lines);
static void	usage(const char *msg);

/*
 * Processes data sent by client until end-of-stream or error.
 */
int proceed_client(struct client *c) {
	ssize_t n;
	size_t line;
	char *pos;

	while ((n = recv(c->sock, c->buf, sizeof(c->buf) - c->nread, 0)) > 0) { // read from socket to buffer as much as possible
		c->nread += n; // save number of bufered bytes
		if ((pos = memchr(c->buf, '\0', c->nread)) == NULL) // search for EOF (end of line) in buffered data
			break; // couldn't read new line fully (limit of buffer size reached) or received something strange
		line = pos - c->buf + 1; // length of the line including '\0' (NULL) at the end
		printf("from client %d: %s\n", c->cnum, c->buf); // print received line
		memmove(c->buf, pos + 1, sizeof(c->buf) - line); // erase printed line from buffer
		// void *memmove(void *dest, const void *src, size_t n) copies n bytes from memory area src to memory area dest
		c->nread -= line; // adjust buffer size after erasing
	}
	if (n == -1 && errno != EAGAIN) { // EAGAIN with O_NONBLOCK means there’s been no data received
		warn(__func__);
		n = 0;
	}
	return (int) n;
}

/*
 * Accepts clients and proceeds them in protocol-agnostic way.
 */
void server_loop(int servsock) {
	struct sockaddr_storage sas; // filled with client connection type information
	socklen_t len = sizeof(struct sockaddr_storage);
	int csock, lastcnum = 0, flags;
	char numbuf[20]; // temporary buffer
	struct client *clients = NULL;
	struct pollfd *pfds; // set of file descriptors to be monitored; fields: fd, events, revents
	size_t nclients = 0, i;

	pfds = malloc(sizeof(struct pollfd));
	pfds[0].fd = servsock; // fd - file descriptor
	pfds[0].events = POLLIN; // events - requested events, POLLIN - there is data to read (also POLLOUT, POLLHUP, ... exists)
	for (;;) {
		poll(pfds, 1+nclients, -1); // wait for some event on a file descriptor, -1 defined as INFTIM in some systems

		if (pfds[0].revents & POLLIN) { // revents - returned events
			// accept extracts the first connection request on the queue of pending connections for the listening socket,
			// ...creates a new connected socket, and returns a new file descriptor referring to that socket
			// accept4 equal to accept but using SOCK_NONBLOCK flag saves extra calls to fcntl, supported not everywhere
			if ((csock = accept(servsock, (struct sockaddr*)&sas, &len)) == -1) {
				if (errno == EINTR) // signal occured while performing accept4
					continue;
				err(1, "accept");
				break;
			}
			flags = fcntl(csock, F_GETFL);
			flags |= O_NONBLOCK;
			fcntl(csock, F_SETFL, flags);
			clients = realloc(clients, (nclients+1)*sizeof(struct client)); // add client instance
			pfds = realloc(pfds, (nclients+2)*sizeof(struct pollfd)); // add client event listener

			clients[nclients].sock = csock;
			clients[nclients].nread = 0;
			clients[nclients].cnum = ++lastcnum;
			pfds[nclients+1].fd = csock;
			pfds[nclients+1].events = POLLIN;
			nclients++;
			snprintf(numbuf, sizeof(numbuf), "client %d", lastcnum); // write to buffer
			printf("===> %s connected\n", numbuf); // write to stdout
		}

		for (i = 0; i < nclients; i++) {
			if (pfds[i+1].revents & POLLIN) {
				if (!proceed_client(&clients[i])) { // when proceed_client returned 0
					snprintf(numbuf, sizeof(numbuf), "client %d", clients[i].cnum);
					printf("===> %s disconnected\n", numbuf);
					close(clients[i].sock);
					memmove(&clients[i], &clients[i+1],
					        (nclients-(i+1))*sizeof(struct client)); // remove client
					memmove(&pfds[i+1], &pfds[i+2],
					        (nclients-(i+1))*sizeof(struct pollfd)); // remove client listener
					nclients--;
				}
			}
		}
	}
}

#define countof(x) (sizeof(x) / sizeof(x[0])) // returns number of elements in static array

// uses unix read and write functions - non portable solution
void client_loop(int csock, unsigned int lines) {
	static const char *nouns[] = {
		"danger",
		"security",
		"table",
		"picture",
		"rainbow"
	};
	static const char *verbs[] = {
		"eats",
		"sleeps",
		"invites",
		"sends",
		"sees"
	};
	static const char *adjectives[] = {
		"beautifully",
		"exclusively",
		"blue",
		"funny",
		"last"
	};

	int i;
	ssize_t n, total, length;
	const char *noun, *verb, *adjective;
	char buf[128]; // temporary buffer

	for (i = 0; i < lines; i++) {
		/*
		 * CAUTION in non-fun code you should care about uniform
		 * distribution; see, e.g., arc4random(3).
		 */
		noun = nouns[rand() % countof(nouns)];
		verb = verbs[rand() % countof(verbs)];
		adjective = adjectives[rand() % countof(adjectives)];
		if ((length = snprintf(buf, sizeof(buf), "%s %s %s", noun, verb, adjective)) < 0) // write to buffer
			err(1, "print");
        sleep(1); // delay 1 second
		// send while everything in not transmitted
		for (total = 0; (n = send(csock, buf + total, length - total, 0)) > 0; total += n); // send tolerates O_NONBLOCK
		if (n == -1)
			err(1, "send");
	}
}

#undef countof

void usage(const char *msg) {
	const char *name;
	if (msg != NULL)
		fprintf(stderr, "%s\n", msg);
	name = getprogname(); // returns the name of the program. If the name has not been set yet, it will return NULL
	fprintf(stderr, "usage: %s {server|client} unix  path\n", name);
	fprintf(stderr, "       %s {server|client} inet  port [address]\n", name);
	fprintf(stderr, "       %s {server|client} inet6 port [address]\n", name);
	exit(2);
}

int main(int argc, char **argv) {
	struct sockaddr_storage	ss; // ss_family field defines the type of socket used
	socklen_t slen; // addrlen arg for bind function
	int s, servermode, flags; // s holds socket

	memset(&ss, 0, sizeof(struct sockaddr_storage)); // all zeroes says bind function that we use all default values

	if (argc < 4 || argc > 5)
		usage(NULL);

	if (strcmp(argv[1], "server") == 0)
		servermode = 1;
	else if (strcmp(argv[1], "client") == 0)
		servermode = 0;
	else
		usage("invalid mode, should be either server or client");

	if (strcmp(argv[2], "unix") == 0) {
		struct sockaddr_un *sun = (struct sockaddr_un*)&ss; // prepare unix socket
        slen = sizeof(struct sockaddr_un); // prepare addrlen for bind function

		if (argc > 4)
			usage(NULL);
		sun->sun_family = AF_UNIX; // unix socket is a file somewhere in the system, supports only SOCK_STREAM
		if (strlcpy(sun->sun_path, argv[3], sizeof(sun->sun_path)) >= sizeof(sun->sun_path))
			usage("UNIX socket path is too long"); // strlcpy returns argv[3] (path) length
		if (servermode)
			unlink(argv[3]); // remove file at socket path if exists
	} else if (strcmp(argv[2], "inet") == 0) {
		struct sockaddr_in *sin = (struct sockaddr_in*)&ss; // prepare ipv4 socket
        slen = sizeof(struct sockaddr_in); // prepare addrlen for bind function
		int port, rv;

		ss.ss_family = AF_INET; // ipv4 socket is a file representing ipv4 addres and port combination
		port = atoi(argv[3]); // TCP (SOCK_STREAM) and UDP (SOCK_DGRAM) are different namespaces
		if (port <= 0 || port > 65535) // 65536 TCP and 65536 UPD ports are represented in system
			errx(1, "invalid port: %s", argv[3]);
		sin->sin_port = htons((uint16_t)port); // use network byte (BE) order instead of host byte order (LE)
		// htons, htonl, ntohs, ntohl required for ip address, port and sended data
		if (argc > 4) {
			rv = inet_pton(AF_INET, argv[4], &sin->sin_addr); // convert IPv4 and IPv6 addresses from text to binary form
			if (!rv) // 0 is returned if src does not contain a character string representing a valid network address
				errx(1, "invalid network address: %s", argv[4]);
		} else if (servermode) {
			/*
			 * Binding to "any" address happens via zeroed address,
			 * which was already set by memset(3).
			 */
		} else {
			/* connect to local address by default */
			inet_pton(ss.ss_family, "127.0.0.1",
			    &((struct sockaddr_in*)&ss)->sin_addr);
		}
	} else if (strcmp(argv[2], "inet6") == 0) {
		struct sockaddr_in6	*sin6 = (struct sockaddr_in6*)&ss;
        slen = sizeof(struct sockaddr_in6); // prepare addrlen for bind function
		int port, rv;

		ss.ss_family = AF_INET6;
		port = atoi(argv[3]); // ASCII to integer - converts char* to int value
		if (port <= 0 || port > 65535)
			errx(1, "invalid port: %s", argv[3]);
		sin6->sin6_port = htons((uint16_t)port);
		if (argc > 4) {
			rv = inet_pton(AF_INET6, argv[4], &sin6->sin6_addr);
			if (!rv)
				errx(1, "invalid network address: %s", argv[4]);
		} else if (servermode) {
			/*
			 * Binding to "any" address happens via zeroed address,
			 * which was already set by memset(3).
			 */
		} else {
			/* connect to local address by default */
			inet_pton(ss.ss_family, "::1",
			    &((struct sockaddr_in6*)&ss)->sin6_addr);
		}
	} else {
		usage("invalid protocol family");
	}

	// calling socket with SOCK_STREAM | SOCK_NONBLOCK flag saves extra calls to fcntl, supported not everywhere
	if ((s = socket(ss.ss_family, SOCK_STREAM, 0)) == -1) // 0 says that system should define protocol automatically
		err(1, "socket"); // SOCK_STREAM is only type for unix socket, for ipv4 & ipv6 it means TCP protocol
	flags = fcntl(s, F_GETFL); // copy socket file descriptor flags to variable
	flags |= O_NONBLOCK; // we want to use nonblocking IO (if not supported nothing happens)
	fcntl(s, F_SETFL, flags); // apply new flags, only O_APPEND, O_NONBLOCK, O_ASYNC, O_DIRECT could be used
	if (servermode) {
		if (bind(s, (const struct sockaddr*)&ss, slen) == -1) // creates socket file
			err(1, "bind");
		if (listen(s, 10) == -1) // mark socket as listening socket, set queue of pending connections length to 10
			err(1, "listen");
		server_loop(s);
	} else {
		// connect to socket (implicit bind call)
		// EINPROGRESS means that socket is nonblocking and the connection cannot be completed immediately
		if (connect(s, (const struct sockaddr*)&ss, slen) != 0 && errno != EINPROGRESS)
			err(1, "connect");
		client_loop(s, nlines);
	}
	close(s); // closes socket file descryptor so that it could be reused (file is not deleted!)

	return 0;
}
