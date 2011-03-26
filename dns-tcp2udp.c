#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
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

int main(int argc, char *argv[]) {
	int sockets = max(0, argc - 2);     // listening socket count
	int server[sockets];                // listening sockets
	struct addrinfo hints;
	struct addrinfo *dest;
	struct addrinfo *res;
	int i;
	int ret;
	pid_t pid;
	uint16_t msgsz;

	assert(sizeof(msgsz) == SIZELEN);
	for (i = 0; i < MAXCONN; i++) {
		in[i] = -1;
		len[i] = 0;
		last[i] = 0;
		out[i] = -1;
	}
	used = 0;

	if (sockets <= 0) {
		printf("Usage: %s <dest ip> <listen ip> [listen ip]...\n", argv[0]);
		exit(EXIT_FAILURE);
	}

	// prepare destination address
	memset(&hints, 0, sizeof(struct addrinfo));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_DGRAM;
	hints.ai_flags = AI_NUMERICHOST;
	hints.ai_protocol = IPPROTO_UDP;

	ret = getaddrinfo(argv[1], "domain", &hints, &dest);
	if (ret != 0) {
		fprintf(stderr, "%s: %s\n", argv[1], gai_strerror(ret));
		exit(EXIT_FAILURE);
	}

	if (dest == NULL) {
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

	// drop root privileges
	ret = setregid(GID, GID);
	if (ret != 0) {
			fprintf(stderr, "%s: unable to change GID: %s\n", argv[0], strerror(errno));
			exit(EXIT_FAILURE);
	}
	
	ret = setreuid(UID, UID);
	if (ret != 0) {
			fprintf(stderr, "%s: unable to change UID: %s\n", argv[0], strerror(errno));
			exit(EXIT_FAILURE);
	}

	// background
	pid = fork();
	if (pid > 0) {
		printf("%s: started successfully with PID %d\n", argv[0], pid);
		exit(EXIT_SUCCESS);
	} else if (pid < 0) {
		printf("%s: unable to fork: %s\n", argv[0], strerror(errno));
		exit(EXIT_FAILURE);
	}

	// main loop
	while (1) {
		fd_set rfds, wfds, efds;
		struct timeval timeout;
		int maxfd = -1;
		time_t now;

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

		// accept new connections
		for (i = 0; i < sockets; i++) {
			if (FD_ISSET(server[i], &rfds)) {
				ret = accept4(server[i], NULL, NULL, SOCK_NONBLOCK);
				if (ret >= 0) {
					int j;

					// find spare slot (there must be one, we don't wait if full up)
					for (j = 0; j < MAXCONN; j++) {
						if (in[j] == -1) {
							in[j] = ret;
							out[j] = -1;
							last[j] = now;
							used++;
							break;
						}
					}
				}
			}
		}

		for (i = 0; i < MAXCONN; i++) {
			// read from incoming socket
			if (in[i] != -1 && FD_ISSET(in[i], &rfds)) {
				int space = BUFSZ - len[i];

				ret = recv(in[i], &buf[i], space, 0);
				if (ret <= 0) {
					// failed read: add socket to error set
					FD_SET(in[i], &efds);
				} else {
					len[i] += ret;

					if (len[i] >= SIZELEN) {
						memcpy(&msgsz, buf[i], SIZELEN);
						msgsz = ntohs(msgsz);

						// complete message?
						if (msgsz == 0) {
							// zero length message: add socket to error set
							FD_SET(in[i], &efds);
						} else if (len[i] >= SIZELEN + msgsz) {
							// open forwarding socket if it does not exist
							if (out[i] == -1) {
								ret = socket(dest->ai_family, dest->ai_socktype, dest->ai_protocol);
								if (ret < 0) {
									// failed open: add socket to error set
									FD_SET(in[i], &efds);
								} else {
									int flags;

									out[i] = ret;

									flags = fcntl(out[i], F_GETFL);
									flags |= O_NONBLOCK;
									ret = fcntl(out[i], F_SETFL, flags);
									if (ret != 0) {
										// failed fcntl: add socket to error set
										FD_SET(out[i], &efds);
									} else {
										ret = connect(out[i], dest->ai_addr, dest->ai_addrlen);
										if (ret != 0) {
											// failed connect: add socket to error set
											FD_SET(out[i], &efds);
										}
									}
								}
							}

							// check forwarding socket is ok
							if (out[i] != -1 && !FD_ISSET(out[i], &efds)) {
								// write data
								ret = send(out[i], &buf[i][SIZELEN], msgsz, 0);
								if (ret < 0) {
									// failed write: add socket to error set
									FD_SET(out[i], &efds);
								}
							}

							// move remaining buffer data
							len[i] -= SIZELEN;
							len[i] -= msgsz;
							if (len[i] > 0) {
								memmove(buf[i], &buf[i][SIZELEN + msgsz], len[i]);
							}
						}
					}
				}
			}

			// read from forwarding socket
			if (out[i] != -1 && FD_ISSET(out[i], &rfds)) {
				int resp_len;

				resp_len = recv(out[i], &resp[SIZELEN], MAXLEN, 0);
				if (resp_len <= 0) {
					// failed read: add socket to error set
					FD_SET(out[i], &efds);
				} else {
					// prepend length
					msgsz = htons(resp_len);
					memcpy(resp, &msgsz, SIZELEN);

					// send data
					ret = send(in[i], &resp, SIZELEN + resp_len, 0);
					if (ret < SIZELEN + resp_len) {
						// failed write: add socket to error set
						FD_SET(in[i], &efds);
					} else {
						// success: reset timeout
						last[i] = now;
					}
				}
			}

			// handle errors on either socket by closing both sockets
			if (in[i] != -1) {
				if (FD_ISSET(in[i], &efds) || (out[i] != -1 && FD_ISSET(out[i], &efds))) {
					close(in[i]);
					in[i] = -1;
					used--;

					if (out[i] != -1) {
						close(out[i]);
						out[i] = -1;
					}
				}
			}
		}

		// cleanup inactive sockets
		//
		// a socket is considered inactive if we haven't
		// forwarded a reply to it - so this will include
		// sockets that haven't sent a full request packet
		// too
		for (i = 0; i < MAXCONN; i++) {
			if (in[i] != -1) {
				if (now - last[i] >= CLEANUP_TIMEOUT) {
					// close both sockets
					close(in[i]);
					in[i] = -1;
					used--;

					if (out[i] != -1) {
						close(out[i]);
						out[i] = -1;
					}
				} else if (now < last[i]) {
					// oops, time went backwards
					last[i] = now;
				}
			}
		}
	}

	freeaddrinfo(dest);
	dest = NULL;

	exit(EXIT_FAILURE);
}
