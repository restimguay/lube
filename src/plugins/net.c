/*
 * net.c - the plugin net: sockets, TCP and UDP, for games and their servers.
 *
 *   import net.*                   (or import net: net.tcp_listen(...))
 *   tcp_listen(port, host?)        a server: s.accept() gives its new connections
 *   tcp_connect(host, port, wait?) a connection: c.send(data), c.receive()
 *   udp_open(port?, host?)         packets: u.send_to(host, port, data), u.packets()
 *   poll(sockets, wait?)           the ones with something to read
 *
 * Every socket is non-blocking: accept, receive and packets give what has
 * arrived (an empty list or Bytes when nothing has), so a game loop never
 * stalls. Each takes the seconds to wait for something first: a frame's
 * worth in a game loop, or INF in an isolate that has nothing else to do.
 * Connections send at once (TCP_NODELAY): games send small messages often.
 */
#ifdef _WIN32
#if !defined(_WIN32_WINNT) || _WIN32_WINNT < 0x0600
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0600  /* WSAPoll, inet_ntop: Windows Vista and later */
#endif
#elif !defined(__APPLE__) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L  /* getaddrinfo, poll under -std=c99 */
#endif
#include "../lube_plugin.h"

#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET Sock;
typedef WSAPOLLFD PollFd;
#define BAD_SOCK INVALID_SOCKET
#define close_sock closesocket
#define sys_poll(fds, n, ms) WSAPoll((fds), (ULONG)(n), (ms))
#define net_errno() WSAGetLastError()
#define E_AGAIN(e) ((e) == WSAEWOULDBLOCK)
#define E_PROGRESS(e) ((e) == WSAEWOULDBLOCK || (e) == WSAEINPROGRESS)
#define E_INTR(e) ((e) == WSAEINTR)
#define E_GONE(e) ((e) == WSAECONNRESET || (e) == WSAECONNABORTED || (e) == WSAENOTCONN || (e) == WSAESHUTDOWN || \
                   (e) == WSAENETRESET || (e) == WSAETIMEDOUT)
#define E_REFUSED(e) ((e) == WSAECONNRESET || (e) == WSAECONNREFUSED)  /* UDP: an earlier packet found no one */
#define E_MSGSIZE(e) ((e) == WSAEMSGSIZE)
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int Sock;
typedef struct pollfd PollFd;
#define BAD_SOCK (-1)
#define close_sock close
#define sys_poll(fds, n, ms) poll((fds), (nfds_t)(n), (ms))
#define net_errno() errno
#define E_AGAIN(e) ((e) == EAGAIN || (e) == EWOULDBLOCK)
#define E_PROGRESS(e) ((e) == EINPROGRESS)
#define E_INTR(e) ((e) == EINTR)
#define E_GONE(e) ((e) == ECONNRESET || (e) == EPIPE || (e) == ENOTCONN || (e) == ECONNABORTED || (e) == ETIMEDOUT)
#define E_REFUSED(e) ((e) == ECONNREFUSED || (e) == ECONNRESET)
#define E_MSGSIZE(e) ((e) == EMSGSIZE)
#endif
#ifdef MSG_NOSIGNAL
#define SEND_FLAGS MSG_NOSIGNAL  /* a closed connection is an error, not SIGPIPE (Linux; see no_sigpipe) */
#else
#define SEND_FLAGS 0
#endif
#ifdef _MSC_VER
#define THREAD_LOCAL __declspec(thread)
#else
#define THREAD_LOCAL __thread
#endif

#define FN(name) static LubeValue name(LubeVM *vm, int argc, const LubeValue *argv)
#define SOCK(s) ((Sock)(s)->fd)
#define MAX_READ (1 << 20)  /* what one receive() takes at most: the rest waits for the next */
#define SEND_PATIENCE 10.0  /* LCOV_EXCL_LINE: seconds send() waits for the other side to make room */

enum { SERVER, TCP, UDP };  /* what a socket is: tcp_listen, a connection, udp_open */
static const char *const KIND_NAMES[] = {"a server (from tcp_listen)", "a TCP connection", "a UDP socket"};

typedef struct {  /* UDP: where send_to sent last (games send to the same few hosts) */
    char *host;
    int port;
    struct sockaddr_storage addr;
    socklen_t len;
} SendTo;

typedef struct {
    intptr_t fd;             /* the socket, or -1 once closed */
    int kind;
    int ended;               /* a connection the other side has closed */
    int family;              /* its addresses: IPv4 or IPv6 */
    char address[80];        /* a connection's other side, or where a server or UDP socket is: "127.0.0.1:5000" */
    SendTo *to;
} Socket;

static const LubeAPI *lube;
static LubeType *SocketType;
static THREAD_LOCAL LubeValue raised;  /* what lube->error gave, for a function to return (see fail) */

/* Raises an error from inside a helper: its caller returns `raised`. */
static void fail(LubeVM *vm, const char *format, ...) {
    char text[512];
    va_list ap;
    va_start(ap, format);
    vsnprintf(text, sizeof text, format, ap);
    va_end(ap);
    raised = lube->error(vm, "%s", text);
}

/* ======================================================================
 * The system's side
 * ==================================================================== */

static void net_start(void) {
#ifdef _WIN32
    static THREAD_LOCAL int started;  /* (counted by Windows: once per thread is fine) */
    if (!started) {
        WSADATA w;
        WSAStartup(MAKEWORD(2, 2), &w);
        started = 1;
    }
#endif
}

/* What an error number means, in the same words everywhere. */
static const char *net_text(int e) {
#ifdef _WIN32
    static THREAD_LOCAL char other[32];
    switch (e) {
    case WSAECONNREFUSED: return "Connection refused";
    case WSAECONNRESET: return "Connection reset by peer";
    case WSAECONNABORTED: return "Software caused connection abort";
    case WSAETIMEDOUT: return "Operation timed out";
    case WSAEADDRINUSE: return "Address already in use";
    case WSAEADDRNOTAVAIL: return "Can't assign requested address";
    case WSAEHOSTUNREACH: return "No route to host";
    case WSAENETUNREACH: return "Network is unreachable";
    case WSAENETDOWN: return "Network is down";
    case WSAEACCES: return "Permission denied";
    case WSAEAFNOSUPPORT: return "Address family not supported by protocol family";
    case WSAEMFILE: return "Too many open files";
    case WSAENOBUFS: return "No buffer space available";
    default: snprintf(other, sizeof other, "error %d", e); return other;
    }
#else
    return strerror(e);
#endif
}

static int set_nonblocking(Sock s) {
#ifdef _WIN32
    u_long on = 1;
    return ioctlsocket(s, FIONBIO, &on) == 0;
#else
    int flags = fcntl(s, F_GETFL, 0);
    return flags >= 0 && fcntl(s, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

static void no_sigpipe(Sock s) {  /* (macOS and the BSDs have no MSG_NOSIGNAL) */
#ifdef SO_NOSIGPIPE
    int on = 1;
    setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, (const char *)&on, sizeof on);
#else
    (void)s;
#endif
}

static void no_delay(Sock s) {
    int on = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&on, sizeof on);
}

/* Milliseconds for poll: -1 for no limit (INF, or more than poll can count). */
static int millis(double seconds) {
    if (isinf(seconds) || seconds > 2e6) return -1;
    return (int)ceil(seconds * 1000);
}

/* Waits up to `wait` seconds for one of fds to be ready: how many are, 0 if
 * none, -1 on an error (net_errno). */
static int wait_any(PollFd *fds, int n, double wait) {
    double end = lube->now() + wait;
    for (;;) {
        int r = sys_poll(fds, n, millis(wait));
        if (r >= 0 || !E_INTR(net_errno())) return r;
        if (!isinf(wait)) {  /* LCOV_EXCL_START: a signal came: the rest of the time */
            wait = end - lube->now();
            if (wait < 0) wait = 0;
        }
    }  /* LCOV_EXCL_STOP */
}

static int wait_one(Sock s, short events, double wait) {
    PollFd p;
    p.fd = s;
    p.events = events;
    p.revents = 0;
    return wait_any(&p, 1, wait);
}

/* Waits for a connection being made: 1 when it has been (or has failed), 0 on no answer in time. */
static int wait_connected(Sock s, double wait) {
#ifdef _WIN32  /* (WSAPoll does not report a refused connection: select does) */
    fd_set w, e;
    FD_ZERO(&w);
    FD_ZERO(&e);
    FD_SET(s, &w);
    FD_SET(s, &e);
    struct timeval tv, *limit = NULL;
    if (millis(wait) >= 0) {
        tv.tv_sec = (long)wait;
        tv.tv_usec = (long)((wait - floor(wait)) * 1e6);
        limit = &tv;
    }
    return select(0, NULL, &w, &e, limit) > 0;
#else
    return wait_one(s, POLLOUT, wait) > 0;
#endif
}

/* "127.0.0.1" and 5000, or "::1" (as text for a person: without brackets). */
static void host_port(const struct sockaddr *sa, char *host, size_t size, int *port) {
    snprintf(host, size, "?");
    *port = 0;
    if (sa->sa_family == AF_INET) {
        const struct sockaddr_in *a = (const struct sockaddr_in *)(const void *)sa;
        inet_ntop(AF_INET, (void *)&a->sin_addr, host, (socklen_t)size);
        *port = ntohs(a->sin_port);
    } else if (sa->sa_family == AF_INET6) {
        const struct sockaddr_in6 *a = (const struct sockaddr_in6 *)(const void *)sa;
        inet_ntop(AF_INET6, (void *)&a->sin6_addr, host, (socklen_t)size);
        *port = ntohs(a->sin6_port);
    }
}

/* The addresses of host (a name or a number) and port; NULL after raising an error. */
static struct addrinfo *resolve(LubeVM *vm, const char *fn, const char *host, int port, int type, int family,
                                int passive) {
    struct addrinfo hints, *res = NULL;
    char service[16];
    memset(&hints, 0, sizeof hints);
    hints.ai_family = family;
    hints.ai_socktype = type;
    hints.ai_flags = AI_NUMERICSERV | (passive ? AI_PASSIVE : 0);
    snprintf(service, sizeof service, "%d", port);
    net_start();
    if (getaddrinfo(host, service, &hints, &res) != 0 || !res) {
        fail(vm, "%s(): cannot find the host '%s'%s", fn, host,
             family == AF_INET ? " (as an IPv4 address, for this socket)"
             : family == AF_INET6 ? " (as an IPv6 address, for this socket)" : "");
        return NULL;
    }
    return res;
}

static LubeValue new_socket(LubeVM *vm, Sock fd, int kind, const struct sockaddr *address) {
    void *data;
    LubeValue v = lube->new_foreign(vm, SocketType, &data);
    Socket *s = (Socket *)data;
    char host[64];
    int port;
    s->fd = (intptr_t)fd;
    s->kind = kind;
    s->family = address->sa_family;
    host_port(address, host, sizeof host, &port);
    snprintf(s->address, sizeof s->address, address->sa_family == AF_INET6 ? "[%s]:%d" : "%s:%d", host, port);
    return v;
}

static void close_socket(Socket *s) {
    if (s->fd != -1) close_sock(SOCK(s));
    s->fd = -1;
}

static void socket_release(void *data) {
    Socket *s = (Socket *)data;
    close_socket(s);
    if (s->to) {
        free(s->to->host);
        free(s->to);
    }
}

static int socket_describe(const void *data, char *buf, size_t size) {  /* Socket(tcp 127.0.0.1:5000) */
    const Socket *s = (const Socket *)data;
    static const char *const kinds[] = {"tcp server", "tcp", "udp"};
    return snprintf(buf, size, "Socket(%s %s%s)", kinds[s->kind], s->address,
                    s->fd == -1 ? ", closed" : s->ended ? ", ended" : "");
}

/* ======================================================================
 * Arguments
 * ==================================================================== */

static int port_of(LubeVM *vm, const char *fn, LubeValue v) {
    int numeric = lube->kind(v) == LUBE_NUMBER;
    double d = numeric ? lube->number(v) : -1;
    if (d == floor(d) && d >= 0 && d <= 65535) return (int)d;
    char buf[64];
    if (numeric) lube->format_number(d, buf, sizeof buf);
    fail(vm, "%s(): a port is a whole Number from 0 to 65535, not %s", fn, numeric ? buf : lube->a_type(v));
    return -1;
}

/* The seconds to wait (argument i if there is one, else 0); -1 after raising an error. */
static double wait_of(LubeVM *vm, const char *fn, int argc, const LubeValue *argv, int i) {
    if (i >= argc) return 0;
    int numeric = lube->kind(argv[i]) == LUBE_NUMBER;
    double w = numeric ? lube->number(argv[i]) : -1;
    if (w >= 0) return w;
    char buf[64];
    if (numeric) lube->format_number(w, buf, sizeof buf);
    fail(vm, "%s(): the seconds to wait must be a Number, 0 or more (or INF: as long as it takes), not %s", fn,
         numeric ? buf : lube->a_type(argv[i]));
    return -1;
}

static const char *host_of(LubeVM *vm, const char *fn, LubeValue v, int i) {
    const char *s = lube->str(v, NULL);
    if (!s) fail(vm, "%s() expects a str (a host) as argument %d, got %s", fn, i, lube->type_name(v));
    return s;
}

/* Bytes, or a str's bytes, to send. 0 after raising an error. */
static int data_of(LubeVM *vm, const char *fn, LubeValue v, const char **p, size_t *n) {
    if (lube->kind(v) == LUBE_BYTES) *p = (const char *)lube->bytes(v, n);
    else if (lube->kind(v) == LUBE_STR) *p = lube->str(v, n);
    else fail(vm, "%s() sends Bytes or a str, not %s", fn, lube->a_type(v));
    return lube->kind(v) == LUBE_BYTES || lube->kind(v) == LUBE_STR;
}

static Socket *open_socket(LubeVM *vm, const char *fn, LubeValue v) {
    Socket *s = (Socket *)lube->foreign(v, SocketType);
    if (s->fd == -1) {
        fail(vm, "%s(): the socket is closed", fn);
        return NULL;
    }
    return s;
}

/* ======================================================================
 * Making sockets
 * ==================================================================== */

/* A server or a UDP socket: port, host = "0.0.0.0" (every address the machine has). */
static LubeValue open_on(LubeVM *vm, const char *fn, int kind, int argc, const LubeValue *argv) {
    int port = argc > 0 ? port_of(vm, fn, argv[0]) : 0;
    if (port < 0) return raised;
    const char *host = argc > 1 ? host_of(vm, fn, argv[1], 2) : "0.0.0.0";
    if (!host) return raised;
    struct addrinfo *res = resolve(vm, fn, host, port, kind == UDP ? SOCK_DGRAM : SOCK_STREAM, AF_UNSPEC, 1);
    if (!res) return raised;
    Sock s = BAD_SOCK;
    int err = 0;
    for (struct addrinfo *ai = res; ai && s == BAD_SOCK; ai = ai->ai_next) {
        s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s == BAD_SOCK) {  /* LCOV_EXCL_START: the system has no sockets left */
            err = net_errno();
            continue;
        }  /* LCOV_EXCL_STOP */
#ifndef _WIN32  /* (a server started again at once can have its port back; on Windows this would share it) */
        int on = 1;
        if (kind == SERVER) setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char *)&on, sizeof on);
#endif
        if (bind(s, ai->ai_addr, (socklen_t)ai->ai_addrlen) == 0 && (kind != SERVER || listen(s, 128) == 0) &&
            set_nonblocking(s))
            break;
        err = net_errno();
        close_sock(s);
        s = BAD_SOCK;
    }
    freeaddrinfo(res);
    if (s == BAD_SOCK)
        return lube->error(vm, "%s(): cannot %s %s port %d: %s", fn, kind == SERVER ? "listen on" : "open", host, port,
                           net_text(err));
    no_sigpipe(s);
    struct sockaddr_storage at;
    socklen_t len = sizeof at;
    getsockname(s, (struct sockaddr *)&at, &len);  /* (port 0: the one the system chose) */
    return new_socket(vm, s, kind, (struct sockaddr *)&at);
}

/* tcp_listen(port, host = "0.0.0.0"): a server. Port 0 lets the system choose (s.port() says which). */
FN(f_tcp_listen) { return open_on(vm, "tcp_listen", SERVER, argc, argv); }

/* udp_open(port = 0, host = "0.0.0.0"): a socket for packets. */
FN(f_udp_open) { return open_on(vm, "udp_open", UDP, argc, argv); }

/* tcp_connect(host, port, wait = 10): a connection, made within `wait` seconds. */
FN(f_tcp_connect) {
    const char *host = host_of(vm, "tcp_connect", argv[0], 1);
    if (!host) return raised;
    int port = port_of(vm, "tcp_connect", argv[1]);
    if (port < 0) return raised;
    double wait = argc > 2 ? wait_of(vm, "tcp_connect", argc, argv, 2) : 10;
    if (wait < 0) return raised;
    struct addrinfo *res = resolve(vm, "tcp_connect", host, port, SOCK_STREAM, AF_UNSPEC, 0);
    if (!res) return raised;
    Sock s = BAD_SOCK;
    int err = 0;
    /* each address it has: IPv4 ones first (servers listen there unless told otherwise, and on Windows each
       refused connection takes seconds), then the others */
    for (int pass = 0; pass < 2 && s == BAD_SOCK; pass++) {
        for (struct addrinfo *ai = res; ai && s == BAD_SOCK; ai = ai->ai_next) {
            if ((pass == 0) != (ai->ai_family == AF_INET)) continue;
            s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
            if (s == BAD_SOCK) {  /* LCOV_EXCL_START: the system has no sockets left */
                err = net_errno();
                continue;
            }  /* LCOV_EXCL_STOP */
            set_nonblocking(s);
            no_sigpipe(s);
            int e = connect(s, ai->ai_addr, (socklen_t)ai->ai_addrlen) == 0 ? 0 : net_errno();
            if (e && E_PROGRESS(e)) {
                if (wait_connected(s, wait)) {
                    int so = 0;
                    socklen_t len = sizeof so;
                    e = getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&so, &len) == 0 ? so : net_errno();
                } else {  /* LCOV_EXCL_START: no answer in time (a host that never answers: not in the tests) */
                    e = -1;
                }  /* LCOV_EXCL_STOP */
            }
            if (e == 0) break;
            if (e > 0) err = e;
            close_sock(s);
            s = BAD_SOCK;
        }
    }
    freeaddrinfo(res);
    if (s == BAD_SOCK) {
        if (!err) {  /* LCOV_EXCL_START: (likewise) */
            char secs[64];
            lube->format_number(wait, secs, sizeof secs);
            return lube->error(vm, "tcp_connect(): cannot connect to %s port %d: no answer in %s seconds", host, port, secs);
        }  /* LCOV_EXCL_STOP */
        return lube->error(vm, "tcp_connect(): cannot connect to %s port %d: %s", host, port, net_text(err));
    }
    no_delay(s);
    struct sockaddr_storage peer;
    socklen_t len = sizeof peer;
    getpeername(s, (struct sockaddr *)&peer, &len);
    return new_socket(vm, s, TCP, (struct sockaddr *)&peer);
}

/* ======================================================================
 * Using them
 * ==================================================================== */

/* server.accept(wait = 0): the new connections (a List: empty if there are none yet). */
FN(m_accept) {
    Socket *s = open_socket(vm, "accept", argv[0]);
    if (!s) return raised;
    if (s->kind != SERVER)
        return lube->error(vm, "accept(): only a server (from tcp_listen) has connections to accept; this is %s",
                           KIND_NAMES[s->kind]);
    double wait = wait_of(vm, "accept", argc, argv, 1);
    if (wait < 0) return raised;
    LubeValue l = lube->new_list(vm);
    for (int waited = 0;;) {
        struct sockaddr_storage peer;
        socklen_t len = sizeof peer;
        Sock c = accept(SOCK(s), (struct sockaddr *)&peer, &len);
        if (c != BAD_SOCK) {
            set_nonblocking(c);
            no_sigpipe(c);
            no_delay(c);
            lube->list_push(l, new_socket(vm, c, TCP, (struct sockaddr *)&peer));
            continue;
        }
        int e = net_errno();
        if (E_AGAIN(e)) {
            if (lube->list_len(l) || waited || wait == 0) break;
            waited = 1;
            int r = wait_one(SOCK(s), POLLIN, wait);
            if (r > 0) continue;
            if (r == 0) break;
            e = net_errno();  /* LCOV_EXCL_START: the system failing, a signal, or one that gave up before it was accepted */
        } else if (E_INTR(e) || E_GONE(e)) {
            continue;
        }
        lube->release(l);
        return lube->error(vm, "accept(): %s", net_text(e));  /* LCOV_EXCL_STOP */
    }
    return l;
}

/* A growing buffer of bytes (malloc'd). */
typedef struct {
    unsigned char *data;
    size_t len, cap;
} Buf;

static void buf_add(Buf *b, const void *p, size_t n) {
    if (b->len + n > b->cap) {
        size_t cap = b->cap ? b->cap : 256;
        while (cap < b->len + n) cap *= 2;
        unsigned char *d = (unsigned char *)realloc(b->data, cap);
        if (!d) abort();  /* LCOV_EXCL_LINE: out of memory */
        b->data = d;
        b->cap = cap;
    }
    if (n) memcpy(b->data + b->len, p, n);
    b->len += n;
}

/* c.receive(wait = 0): what has arrived (Bytes: empty if nothing has). After
 * the other side closes, it is empty and c.is_open() is false. On a UDP
 * socket: the next packet's bytes (see packets() for who sent it). */
FN(m_receive) {
    Socket *s = open_socket(vm, "receive", argv[0]);
    if (!s) return raised;
    if (s->kind == SERVER)
        return lube->error(vm, "receive(): a server receives connections, with accept(); its connections receive data");
    double wait = wait_of(vm, "receive", argc, argv, 1);
    if (wait < 0) return raised;
    Buf b = {0};
    unsigned char chunk[65536];
    int waited = s->ended;  /* (nothing more will come: no waiting for it) */
    for (;;) {
        long n = (long)recv(SOCK(s), (char *)chunk, (int)sizeof chunk, 0);
        if (n > 0 || (n == 0 && s->kind == UDP)) {  /* (a UDP packet can be empty) */
            buf_add(&b, chunk, (size_t)n);
            if (s->kind == UDP || b.len >= MAX_READ) break;
            continue;
        }
        int e = n == 0 ? 0 : net_errno();
        if (s->kind == TCP && (n == 0 || E_GONE(e))) {  /* the other side closed it */
            s->ended = 1;
            break;
        }
        if (E_AGAIN(e)) {
            if (b.len || waited || wait == 0) break;
            waited = 1;
            int r = wait_one(SOCK(s), POLLIN, wait);
            if (r > 0) continue;
            if (r == 0) break;
            e = net_errno();  /* LCOV_EXCL_START: the system failing, a signal, or news of a lost packet */
        } else if (E_INTR(e) || (s->kind == UDP && E_REFUSED(e))) {
            continue;
        }
        free(b.data);
        return lube->error(vm, "receive(): %s", net_text(e));  /* LCOV_EXCL_STOP */
    }
    LubeValue out = lube->new_bytes(vm, b.data, b.len);
    free(b.data);
    return out;
}

/* A packet: {data: Bytes, host: "127.0.0.1", port: 5000}. */
static LubeValue packet(LubeVM *vm, const unsigned char *data, size_t n, const struct sockaddr *from) {
    char host[64];
    int port;
    host_port(from, host, sizeof host, &port);
    LubeValue p = lube->new_object(vm);
    lube->object_set(vm, p, "data", lube->new_bytes(vm, data, n));
    lube->object_set(vm, p, "host", lube->new_str(vm, host, strlen(host)));
    lube->object_set(vm, p, "port", lube->number_value(port));
    return p;
}

/* u.packets(wait = 0): every packet that has arrived, as {data, host, port}
 * (empty if none has): reply with u.send_to(p.host, p.port, ...). */
FN(m_packets) {
    Socket *s = open_socket(vm, "packets", argv[0]);
    if (!s) return raised;
    if (s->kind != UDP)
        return lube->error(vm, "packets(): only a UDP socket (from udp_open) gets packets; this is %s%s", KIND_NAMES[s->kind],
                           s->kind == TCP ? ", which has receive()" : ", which has accept()");
    double wait = wait_of(vm, "packets", argc, argv, 1);
    if (wait < 0) return raised;
    LubeValue l = lube->new_list(vm);
    unsigned char chunk[65536];
    for (int waited = 0; lube->list_len(l) < 4096;) {  /* (a flood waits for the next call) */
        struct sockaddr_storage from;
        socklen_t len = sizeof from;
        long n = (long)recvfrom(SOCK(s), (char *)chunk, (int)sizeof chunk, 0, (struct sockaddr *)&from, &len);
        if (n >= 0) {
            lube->list_push(l, packet(vm, chunk, (size_t)n, (struct sockaddr *)&from));
            continue;
        }
        int e = net_errno();
        if (E_AGAIN(e)) {
            if (lube->list_len(l) || waited || wait == 0) break;
            waited = 1;
            int r = wait_one(SOCK(s), POLLIN, wait);
            if (r > 0) continue;
            if (r == 0) break;
            e = net_errno();  /* LCOV_EXCL_START: (likewise) */
        } else if (E_INTR(e) || E_REFUSED(e)) {
            continue;
        }
        lube->release(l);
        return lube->error(vm, "packets(): %s", net_text(e));  /* LCOV_EXCL_STOP */
    }
    return l;
}

/* c.send(data): sends it all (Bytes, or a str's bytes); false if the other
 * side has closed the connection. */
FN(m_send) {
    (void)argc;
    Socket *s = open_socket(vm, "send", argv[0]);
    if (!s) return raised;
    if (s->kind != TCP)
        return lube->error(vm, "send(): %s", s->kind == UDP ? "a UDP socket sends packets, with send_to(host, port, data)"
                                                            : "a server sends nothing: its connections do");
    const char *p;
    size_t n;
    if (!data_of(vm, "send", argv[1], &p, &n)) return raised;
    if (s->ended) return lube->boolean(0);
    size_t done = 0;
    double start = lube->now();
    while (done < n) {
        size_t chunk = n - done > (size_t)1 << 30 ? (size_t)1 << 30 : n - done;
        long k = (long)send(SOCK(s), p + done, (int)chunk, SEND_FLAGS);
        if (k > 0) {
            done += (size_t)k;
            continue;
        }
        int e = net_errno();
        if (E_INTR(e)) continue;
        if (E_AGAIN(e)) {  /* LCOV_EXCL_START: the system's buffer is full: wait for the other side to read */
            double left = SEND_PATIENCE - (lube->now() - start);
            if (left > 0 && wait_one(SOCK(s), POLLOUT, left) > 0) continue;
            return lube->error(vm, "send(): the other side is not reading (gave up after %g seconds)", SEND_PATIENCE);
        }  /* LCOV_EXCL_STOP */
        if (E_GONE(e)) {
            s->ended = 1;
            return lube->boolean(0);
        }
        return lube->error(vm, "send(): %s", net_text(e));  /* LCOV_EXCL_LINE: the system failing */
    }
    return lube->boolean(1);
}

/* u.send_to(host, port, data): sends a packet; false if the system dropped it
 * (as it may: packets can be lost). */
FN(m_send_to) {
    (void)argc;
    Socket *s = open_socket(vm, "send_to", argv[0]);
    if (!s) return raised;
    if (s->kind != UDP)
        return lube->error(vm, "send_to(): only a UDP socket (from udp_open) sends packets; this is %s%s", KIND_NAMES[s->kind],
                           s->kind == TCP ? ", which has send(data)" : "");
    const char *host = host_of(vm, "send_to", argv[1], 1);
    if (!host) return raised;
    int port = port_of(vm, "send_to", argv[2]);
    if (port < 0) return raised;
    const char *p;
    size_t n;
    if (!data_of(vm, "send_to", argv[3], &p, &n)) return raised;
    SendTo *to = s->to;
    if (!to || to->port != port || strcmp(to->host, host)) {
        struct addrinfo *res = resolve(vm, "send_to", host, port, SOCK_DGRAM, s->family, 0);
        if (!res) return raised;
        if (!to) {
            to = (SendTo *)calloc(1, sizeof *to);
            if (!to) abort();  /* LCOV_EXCL_LINE: out of memory */
            s->to = to;
        }
        free(to->host);
        to->host = (char *)malloc(strlen(host) + 1);
        if (!to->host) abort();  /* LCOV_EXCL_LINE: out of memory */
        strcpy(to->host, host);
        to->port = port;
        memcpy(&to->addr, res->ai_addr, res->ai_addrlen);
        to->len = (socklen_t)res->ai_addrlen;
        freeaddrinfo(res);
    }
    for (;;) {
        long k = (long)sendto(SOCK(s), p, (int)n, SEND_FLAGS, (struct sockaddr *)&to->addr, to->len);
        if (k >= 0) return lube->boolean(1);
        int e = net_errno();
        if (E_INTR(e)) continue;
        if (E_AGAIN(e) || E_REFUSED(e)) return lube->boolean(0);
        if (E_MSGSIZE(e)) return lube->error(vm, "send_to(): %lu bytes are too many for one packet", (unsigned long)n);
        return lube->error(vm, "send_to(): cannot send to %s port %d: %s", host, port, net_text(e));  /* LCOV_EXCL_LINE */
    }
}

/* s.close(): closes it (again: nothing). It also closes when nothing holds it. */
FN(m_close) {
    (void)vm;
    (void)argc;
    close_socket((Socket *)lube->foreign(argv[0], SocketType));
    return lube->nothing();
}

/* s.is_open(): not closed, and (a connection) not closed by the other side. */
FN(m_is_open) {
    (void)vm;
    (void)argc;
    const Socket *s = (const Socket *)lube->foreign(argv[0], SocketType);
    return lube->boolean(s->fd != -1 && !s->ended);
}

/* s.port(): the port it has on this machine (the one the system chose, for port 0). */
FN(m_port) {
    (void)argc;
    Socket *s = open_socket(vm, "port", argv[0]);
    if (!s) return raised;
    struct sockaddr_storage at;
    socklen_t len = sizeof at;
    char host[64];
    int port = 0;
    if (getsockname(SOCK(s), (struct sockaddr *)&at, &len) == 0) host_port((struct sockaddr *)&at, host, sizeof host, &port);
    return lube->number_value(port);
}

/* c.peer(): the other side of a connection, "127.0.0.1:52431". */
FN(m_peer) {
    (void)argc;
    const Socket *s = (const Socket *)lube->foreign(argv[0], SocketType);
    if (s->kind != TCP)
        return lube->error(vm, "peer(): only a TCP connection has another side; this is %s", KIND_NAMES[s->kind]);
    return lube->new_str(vm, s->address, strlen(s->address));
}

/* poll(sockets, wait = 0): the sockets that have something to read (data, a
 * connection to accept, or the other side closing), waiting up to `wait`
 * seconds for one to. A game loop's wait for the next frame. */
FN(f_poll) {
    if (lube->kind(argv[0]) != LUBE_LIST)
        return lube->error(vm, "poll() expects a List of Sockets as argument 1, got %s", lube->type_name(argv[0]));
    size_t count = lube->list_len(argv[0]);
    double wait = wait_of(vm, "poll", argc, argv, 1);
    if (wait < 0) return raised;
    PollFd *fds = count ? (PollFd *)malloc(count * sizeof(PollFd)) : NULL;
    size_t *which = count ? (size_t *)malloc(count * sizeof(size_t)) : NULL;
    if (count && (!fds || !which)) abort();  /* LCOV_EXCL_LINE: out of memory */
    int n = 0;
    for (size_t i = 0; i < count; i++) {
        LubeValue item = lube->list_get(argv[0], i);
        Socket *s = (Socket *)lube->foreign(item, SocketType);
        if (!s) {
            free(fds);
            free(which);
            return lube->error(vm, "poll(): item %lu is %s, not a Socket", (unsigned long)i, lube->a_type(item));
        }
        if (s->fd == -1) continue;
        if (s->ended) wait = 0;  /* (one is ready already) */
        fds[n].fd = SOCK(s);
        fds[n].events = POLLIN;
        fds[n].revents = 0;
        which[n++] = i;
    }
    int r = 0;
    if (n > 0) {
        r = wait_any(fds, n, wait);
    } else if (millis(wait) < 0) {
        r = -2;
    } else if (wait > 0) {  /* nothing to wait for: just the time */
#ifdef _WIN32
        Sleep((DWORD)millis(wait));
#else
        poll(NULL, 0, millis(wait));
#endif
    }
    if (r < 0) {
        int e = net_errno();
        free(fds);
        free(which);
        return r == -2 ? lube->error(vm, "poll(): no open socket to wait for, and no limit on the wait")
                       : lube->error(vm, "poll(): %s", net_text(e));  /* LCOV_EXCL_LINE: the system failing */
    }
    LubeValue out = lube->new_list(vm);
    for (int k = 0; k < n; k++) {
        LubeValue item = lube->list_get(argv[0], which[k]);
        if (fds[k].revents || ((const Socket *)lube->foreign(item, SocketType))->ended) lube->list_push(out, lube->retain(item));
    }
    free(fds);
    free(which);
    return out;
}

LUBE_PLUGIN(net) {
    lube = api;
    api->function(plugin, "tcp_listen", f_tcp_listen, 1, 2);
    api->function(plugin, "tcp_connect", f_tcp_connect, 2, 3);
    api->function(plugin, "udp_open", f_udp_open, 0, 2);
    api->function(plugin, "poll", f_poll, 1, 2);
    SocketType = api->type(plugin, "Socket", sizeof(Socket), socket_release, socket_describe);
    api->method(SocketType, "accept", m_accept, 1, 2);
    api->method(SocketType, "receive", m_receive, 1, 2);
    api->method(SocketType, "packets", m_packets, 1, 2);
    api->method(SocketType, "send", m_send, 2, 2);
    api->method(SocketType, "send_to", m_send_to, 4, 4);
    api->method(SocketType, "close", m_close, 1, 1);
    api->method(SocketType, "is_open", m_is_open, 1, 1);
    api->method(SocketType, "port", m_port, 1, 1);
    api->method(SocketType, "peer", m_peer, 1, 1);
    return LUBE_PLUGIN_API;
}
