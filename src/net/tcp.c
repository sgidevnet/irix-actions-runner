#include "net/tcp.h"

#include "compat/irix.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

static pthread_mutex_t resolve_lock = PTHREAD_MUTEX_INITIALIZER;

static int
resolve_ipv4(const char *host, struct in_addr *out)
{
	struct hostent *he;
	int rc = -1;

	pthread_mutex_lock(&resolve_lock);

	he = gethostbyname(host);
	if (he != NULL && he->h_addrtype == AF_INET &&
	    he->h_length == (int)sizeof(struct in_addr) &&
	    he->h_addr_list[0] != NULL) {
		memcpy(out, he->h_addr_list[0], sizeof(struct in_addr));
		rc = 0;
	}

	pthread_mutex_unlock(&resolve_lock);
	return rc;
}

int
sgug_tcp_connect(const char *host, int port, int timeout_ms)
{
	struct sockaddr_in sin;
	struct pollfd pfd;
	int fd, flags, err, pr;
	socklen_t errlen;

	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	sin.sin_port = htons((unsigned short)port);
	if (resolve_ipv4(host, &sin.sin_addr) != 0)
		return -1;

	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0)
		return -1;

	flags = fcntl(fd, F_GETFL, 0);
	if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
		goto fail;

	if (connect(fd, (struct sockaddr *)&sin, sizeof(sin)) != 0) {
		if (errno != EINPROGRESS)
			goto fail;

		pfd.fd = fd;
		pfd.events = POLLOUT;
		pfd.revents = 0;

		/* Retried because the caller may be running a periodic
		 * alarm; poll is never restarted by SA_RESTART. */
		do {
			pr = poll(&pfd, 1, timeout_ms);
		} while (pr < 0 && errno == EINTR);
		if (pr != 1)
			goto fail;

		errlen = sizeof(err);
		if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &errlen) != 0 || err != 0)
			goto fail;
	}

	if (fcntl(fd, F_SETFL, flags) < 0)
		goto fail;

	return fd;

fail:
	close(fd);
	return -1;
}

int
sgug_tcp_set_nodelay(int fd)
{
	int on = 1;

	return setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
}

int
sgug_tcp_set_timeouts(int fd, int recv_ms, int send_ms)
{
	struct timeval rtv, stv;

	rtv.tv_sec = recv_ms / 1000;
	rtv.tv_usec = (recv_ms % 1000) * 1000;
	stv.tv_sec = send_ms / 1000;
	stv.tv_usec = (send_ms % 1000) * 1000;

	if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rtv, sizeof(rtv)) != 0)
		return -1;
	return setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &stv, sizeof(stv));
}
