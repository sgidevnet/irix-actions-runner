#ifndef SGUG_NET_TCP_H
#define SGUG_NET_TCP_H

/*
 * Connects to host:port over IPv4 and returns a socket, or -1.
 *
 * IPv4 only, deliberately. IRIX declares getaddrinfo `#pragma optional` (a weak
 * symbol GCC will not let us test for) and IPv6 support is incomplete before
 * 6.5.20. GitHub is reachable over IPv4 from everywhere.
 *
 * Resolution uses gethostbyname, which returns a static buffer, so this
 * serializes internally on a mutex.
 *
 * timeout_ms bounds the connect, not the resolve; IRIX offers no way to bound a
 * gethostbyname call short of a helper process.
 */
int sgug_tcp_connect(const char *host, int port, int timeout_ms);

/* Disables Nagle. Worth it for the long-poll loop, where requests are small and
 * latency to first byte dominates. */
int sgug_tcp_set_nodelay(int fd);

/* Bounds a single read or write. The long poll needs at least 100s, since the
 * service holds the connection open for about 50s before returning empty. */
int sgug_tcp_set_timeouts(int fd, int recv_ms, int send_ms);

#endif /* SGUG_NET_TCP_H */
