#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

// Config
#define MAXCONN 300
#define CLEANUP_INTERVAL 5        // seconds
#define CLEANUP_TIMEOUT 30        // seconds
#define UID 65534
#define GID 65534

// DNS
#define MAXLEN (1 << 16)          // 65536 bytes
#define SIZELEN 2                 // message length size (2 bytes)
#define BUFSZ (SIZELEN + MAXLEN)

static int in[MAXCONN];           // incoming accepted sockets
static char buf[MAXCONN][BUFSZ];  // incoming buffers
static int len[MAXCONN];          // incoming buffer used
static int used;                  // connections in use
static time_t last[MAXCONN];      // last response to incoming socket
static int out[MAXCONN];          // outgoing sockets (can only exist while an incoming socket does too)
static char resp[BUFSZ];          // response buffer

static inline int max(int a, int b) { return a > b ? a : b; }
static void setup(struct addrinfo **dest, int sockets, int server[], char *argv[]);
static void drop_root(char *name);
static void background(char *name);
static void loop(int sockets, int server[], struct addrinfo *dest);
static void accept_new(int sockets, int server[], fd_set *rfds, time_t now);
static void save_conn(int fd, time_t now);
static void incoming_read(int id, fd_set *efds, struct addrinfo *dest);
static void forward_message(int id, fd_set *efds, uint16_t msgsz, struct addrinfo *dest);
static bool forward_ready(int id, fd_set *efds, struct addrinfo *dest);
static void forwarding_read(int id, fd_set *efds, time_t now);
static void cleanup_inactive(time_t now);
static void close_conn(int id);

int main(int argc, char *argv[]) {
	int sockets = max(0, argc - 2);     // listening socket count
	int server[sockets];                // listening sockets
	struct addrinfo *dest;

	assert(sizeof(uint16_t) == SIZELEN);

	if (sockets <= 0) {
		printf("Usage: %s <dest ip> <listen ip> [listen ip]...\n", argv[0]);
		exit(EXIT_FAILURE);
	}

	setup(&dest, sockets, server, argv);
	drop_root(argv[0]);
	background(argv[0]);
	
	// main loop
	while (1) {
		loop(sockets, server, dest);
	}

	freeaddrinfo(dest);
	dest = NULL;

	exit(EXIT_FAILURE);
}

static void setup(struct addrinfo **dest, int sockets, int server[], char *argv[]) {
	struct addrinfo hints;
	struct addrinfo *res;
	int ret, i;

	// initialise arrays
	for (i = 0; i < MAXCONN; i++) {
		in[i] = -1;
		len[i] = 0;
		last[i] = 0;
		out[i] = -1;
	}
	used = 0;

	// prepare destination address
	memset(&hints, 0, sizeof(struct addrinfo));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_DGRAM;
	hints.ai_flags = AI_NUMERICHOST;
	hints.ai_protocol = IPPROTO_UDP;

	ret = getaddrinfo(argv[1], "domain", &hints, dest);
	if (ret != 0) {
		fprintf(stderr, "%s: %s\n", argv[1], gai_strerror(ret));
		exit(EXIT_FAILURE);
	}

	if (*dest == NULL) {
		fprintf(stderr, "Unable to parse destination host \"%s\"\n", argv[1]);
		exit(EXIT_FAILURE);
	}

	// prepare listening sockets
	hints.ai_protocol = IPPROTO_TCP;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags |= AI_PASSIVE;

	for (i = 0; i < sockets; i++) {
		int flags;
		int one = 1;

		ret = getaddrinfo(argv[i + 2], "domain", &hints, &res);
		if (ret != 0) {
			fprintf(stderr, "%s: %s\n", argv[i + 2], gai_strerror(ret));
			exit(EXIT_FAILURE);
		}

		if (res == NULL) {
			fprintf(stderr, "Unable to parse listen host \"%s\"\n", argv[i + 2]);
			exit(EXIT_FAILURE);
		}

		server[i] = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
		if (server[i] < 0) {
			fprintf(stderr, "%s: unable to open socket: %s\n", argv[i + 2], strerror(errno));
			exit(EXIT_FAILURE);
		}

		flags = fcntl(server[i], F_GETFL);
		if (flags == -1) {
			fprintf(stderr, "%s: unable to get flags: %s\n", argv[i + 2], strerror(errno));
			exit(EXIT_FAILURE);
		}
		flags |= O_NONBLOCK;

		ret = fcntl(server[i], F_SETFL, flags);
		if (ret != 0) {
			fprintf(stderr, "%s: unable to set O_NONBLOCK: %s\n", argv[i + 2], strerror(errno));
			exit(EXIT_FAILURE);
		}

		ret = setsockopt(server[i], SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
		if (ret != 0) {
			fprintf(stderr, "%s: unable to set SO_REUSEADDR: %s\n", argv[i + 2], strerror(errno));
			exit(EXIT_FAILURE);
		}

		ret = bind(server[i], res->ai_addr, res->ai_addrlen);
		if (ret != 0) {
			fprintf(stderr, "%s: unable to bind socket: %s\n", argv[i + 2], strerror(errno));
			exit(EXIT_FAILURE);
		}

		ret = listen(server[i], 10);
		if (ret != 0) {
			fprintf(stderr, "%s: unable to listen on socket: %s\n", argv[i + 2], strerror(errno));
			exit(EXIT_FAILURE);
		}

		freeaddrinfo(res);
		res = NULL;
	}
}

static void drop_root(char *name) {
	int ret;

	ret = setregid(GID, GID);
	if (ret != 0) {
			fprintf(stderr, "%s: unable to change GID: %s\n", name, strerror(errno));
			exit(EXIT_FAILURE);
	}
	
	ret = setreuid(UID, UID);
	if (ret != 0) {
			fprintf(stderr, "%s: unable to change UID: %s\n", name, strerror(errno));
			exit(EXIT_FAILURE);
	}
}

static void background(char *name) {
	pid_t pid = fork();
	if (pid > 0) {
		printf("%s: started successfully with PID %d\n", name, pid);
		exit(EXIT_SUCCESS);
	} else if (pid < 0) {
		printf("%s: unable to fork: %s\n", name, strerror(errno));
		exit(EXIT_FAILURE);
	}
}

static void loop(int sockets, int server[], struct addrinfo *dest) {
	fd_set rfds, wfds, efds;
	struct timeval timeout;
	int maxfd = -1;
	time_t now;
	int ret, i;

	// determine sockets to wait for
	FD_ZERO(&rfds);
	FD_ZERO(&wfds);
	FD_ZERO(&efds);
	timeout.tv_sec = CLEANUP_INTERVAL;
	timeout.tv_usec = 0;

	// listen for new connections if there are spare slots
	if (used < MAXCONN) {
		for (i = 0; i < sockets; i++) {
			FD_SET(server[i], &rfds);
			maxfd = max(maxfd, server[i]);
		}
	}

	// wait for activity on connections
	for (i = 0; i < MAXCONN; i++) {
		if (in[i] != -1) {
			FD_SET(in[i], &rfds);
			FD_SET(in[i], &efds);
			maxfd = max(maxfd, in[i]);
		}

		if (out[i] != -1) {
			FD_SET(out[i], &rfds);
			FD_SET(out[i], &efds);
			maxfd = max(maxfd, out[i]);
		}
	}

	// wait for activity (forever if no active client sockets)
	ret = select(maxfd + 1, &rfds, &wfds, &efds, used > 0 ? &timeout : NULL);
	if (ret < 0) {
		fprintf(stderr, "select: %s\n", strerror(errno));
		exit(EXIT_FAILURE);
	}

	now = time(NULL);

	accept_new(sockets, server, &rfds, now);

	for (i = 0; i < MAXCONN; i++) {
		// read from incoming socket
		if (in[i] != -1 && FD_ISSET(in[i], &rfds)) {
			incoming_read(i, &efds, dest);
		}

		// read from forwarding socket
		if (out[i] != -1 && FD_ISSET(out[i], &rfds)) {
			forwarding_read(i, &efds, now);
		}

		// handle errors on either socket by closing both sockets
		if (in[i] != -1) {
			if (FD_ISSET(in[i], &efds) || (out[i] != -1 && FD_ISSET(out[i], &efds))) {
				close_conn(i);
			}
		}
	}

	cleanup_inactive(now);
}

static void accept_new(int sockets, int server[], fd_set *rfds, time_t now) {
	int ret, i;

	for (i = 0; i < sockets; i++) {
		if (FD_ISSET(server[i], rfds)) {
			ret = accept4(server[i], NULL, NULL, SOCK_NONBLOCK);
			if (ret >= 0) {
				int tmp = BUFSZ;

				// set buffer sizes to one DNS packet
				setsockopt(ret, SOL_SOCKET, SO_RCVBUF, (void*)&tmp, sizeof(tmp));
				setsockopt(ret, SOL_SOCKET, SO_SNDBUF, (void*)&tmp, sizeof(tmp));

				save_conn(ret, now);
			}
		}
	}
}

static void save_conn(int fd, time_t now) {
	int i;

	// find spare slot (there must be one, we don't accept if full up)
	for (i = 0; i < MAXCONN; i++) {
		if (in[i] == -1) {
			in[i] = fd;
			out[i] = -1;
			last[i] = now;
			used++;
			break;
		}
	}
}

static void incoming_read(int id, fd_set *efds, struct addrinfo *dest) {
	int space = BUFSZ - len[id];
	int ret;
	uint16_t msgsz;

	ret = recv(in[id], &buf[id], space, 0);
	if (ret <= 0) {
		// failed read: add socket to error set
		FD_SET(in[id], efds);
		return;
	}

	len[id] += ret;

	// size header in buffer?
	if (len[id] >= SIZELEN) {
		memcpy(&msgsz, buf[id], SIZELEN);
		msgsz = ntohs(msgsz);

		if (msgsz == 0) {
			// zero length message: add socket to error set
			FD_SET(in[id], efds);
		} else if (len[id] >= SIZELEN + msgsz) {
			// complete message
			forward_message(id, efds, msgsz, dest);
		}
	}
}

static void forward_message(int id, fd_set *efds, uint16_t msgsz, struct addrinfo *dest) {
	int ret;

	// check forwarding socket is ok
	if (forward_ready(id, efds, dest)) {
		// write data
		ret = send(out[id], &buf[id][SIZELEN], msgsz, 0);
		if (ret < 0) {
			// failed write: add socket to error set
			FD_SET(out[id], efds);
			// continue so that this buffer data can't be retried
		}
	}

	// move remaining buffer data
	len[id] -= SIZELEN;
	len[id] -= msgsz;
	if (len[id] > 0)
		memmove(buf[id], &buf[id][SIZELEN + msgsz], len[id]);
}

static bool forward_ready(int id, fd_set *efds, struct addrinfo *dest) {
	int flags, ret;

	// open forwarding socket if it does not exist
	if (out[id] != -1)
		return true;

	ret = socket(dest->ai_family, dest->ai_socktype, dest->ai_protocol);
	if (ret < 0)
		goto failed;

	out[id] = ret;

	flags = fcntl(out[id], F_GETFL);
	if (flags == -1)
		goto failed;
	flags |= O_NONBLOCK;

	ret = fcntl(out[id], F_SETFL, flags);
	if (ret != 0)
		goto failed;

	ret = connect(out[id], dest->ai_addr, dest->ai_addrlen);
	if (ret != 0)
		goto failed;

	return true;

failed:
	// add socket to error set
	FD_SET(out[id], efds);
	return false;
}

static void forwarding_read(int id, fd_set *efds, time_t now) {
	uint16_t msgsz;
	int ret, resp_len;

	resp_len = recv(out[id], &resp[SIZELEN], MAXLEN, 0);
	if (resp_len <= 0) {
		// failed read: add socket to error set
		FD_SET(out[id], efds);
		return;
	}

	// prepend length
	msgsz = htons(resp_len);
	memcpy(resp, &msgsz, SIZELEN);

	// send data
	ret = send(in[id], &resp, SIZELEN + resp_len, 0);
	if (ret < SIZELEN + resp_len) {
		// failed write: add socket to error set
		FD_SET(in[id], efds);
		return;
	}

	// reset timeout
	last[id] = now;
}

static void cleanup_inactive(time_t now) {
	int i;

	// a socket is considered inactive if we haven't
	// forwarded a reply to it - so this will include
	// sockets that haven't sent a full request packet
	// too
	for (i = 0; i < MAXCONN; i++) {
		if (in[i] != -1) {
			if (now - last[i] >= CLEANUP_TIMEOUT) {
				close_conn(i);
			} else if (now < last[i]) {
				// oops, time went backwards
				last[i] = now;
			}
		}
	}
}

static void close_conn(int id) {
	// close both sockets
	close(in[id]);
	in[id] = -1;
	used--;

	if (out[id] != -1) {
		close(out[id]);
		out[id] = -1;
	}
}
