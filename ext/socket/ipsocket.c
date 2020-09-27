/************************************************

  ipsocket.c -

  created at: Thu Mar 31 12:21:29 JST 1994

  Copyright (C) 1993-2007 Yukihiro Matsumoto

************************************************/

#include "rubysocket.h"

struct inetsock_arg
{
    VALUE sock;
    struct {
	VALUE host, serv;
	struct rb_addrinfo *res;
    } remote, local;
    int type;
    int fd;
    VALUE resolv_timeout;
};

static VALUE
inetsock_cleanup(VALUE v)
{
    struct inetsock_arg *arg = (void *)v;
    if (arg->remote.res) {
	rb_freeaddrinfo(arg->remote.res);
	arg->remote.res = 0;
    }
    if (arg->local.res) {
	rb_freeaddrinfo(arg->local.res);
	arg->local.res = 0;
    }
    if (arg->fd >= 0) {
	close(arg->fd);
    }
    return Qnil;
}

static VALUE
init_inetsock_internal(VALUE v)
{
    struct inetsock_arg *arg = (void *)v;
    int error = 0;
    int type = arg->type;
    struct addrinfo *res, *lres;
    int fd, status = 0, local = 0;
    int family = AF_UNSPEC;
    const char *syscall = 0;

#ifdef HAVE_GETADDRINFO_A
    arg->remote.res = rsock_addrinfo_a(arg->remote.host, arg->remote.serv,
				       family, SOCK_STREAM,
				       (type == INET_SERVER) ? AI_PASSIVE : 0,
				       arg->resolv_timeout);
#else
    arg->remote.res = rsock_addrinfo(arg->remote.host, arg->remote.serv,
				     family, SOCK_STREAM,
				     (type == INET_SERVER) ? AI_PASSIVE : 0);
#endif


    /*
     * Maybe also accept a local address
     */

    if (type != INET_SERVER && (!NIL_P(arg->local.host) || !NIL_P(arg->local.serv))) {
	arg->local.res = rsock_addrinfo(arg->local.host, arg->local.serv,
					family, SOCK_STREAM, 0);
    }

    arg->fd = fd = -1;
    for (res = arg->remote.res->ai; res; res = res->ai_next) {
#if !defined(INET6) && defined(AF_INET6)
	if (res->ai_family == AF_INET6)
	    continue;
#endif
        lres = NULL;
        if (arg->local.res) {
            for (lres = arg->local.res->ai; lres; lres = lres->ai_next) {
                if (lres->ai_family == res->ai_family)
                    break;
            }
            if (!lres) {
                if (res->ai_next || status < 0)
                    continue;
                /* Use a different family local address if no choice, this
                 * will cause EAFNOSUPPORT. */
                lres = arg->local.res->ai;
            }
        }
	status = rsock_socket(res->ai_family,res->ai_socktype,res->ai_protocol);
	syscall = "socket(2)";
	fd = status;
	if (fd < 0) {
	    error = errno;
	    continue;
	}
	arg->fd = fd;
	if (type == INET_SERVER) {
#if !defined(_WIN32) && !defined(__CYGWIN__)
	    status = 1;
	    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR,
		       (char*)&status, (socklen_t)sizeof(status));
#endif
	    status = bind(fd, res->ai_addr, res->ai_addrlen);
	    syscall = "bind(2)";
	}
	else {
	    if (lres) {
#if !defined(_WIN32) && !defined(__CYGWIN__)
                status = 1;
                setsockopt(fd, SOL_SOCKET, SO_REUSEADDR,
                           (char*)&status, (socklen_t)sizeof(status));
#endif
		status = bind(fd, lres->ai_addr, lres->ai_addrlen);
		local = status;
		syscall = "bind(2)";
	    }

	    if (status >= 0) {
		status = rsock_connect(fd, res->ai_addr, res->ai_addrlen,
				       (type == INET_SOCKS));
		syscall = "connect(2)";
	    }
	}

	if (status < 0) {
	    error = errno;
	    close(fd);
	    arg->fd = fd = -1;
	    continue;
	} else
	    break;
    }
    if (status < 0) {
	VALUE host, port;

	if (local < 0) {
	    host = arg->local.host;
	    port = arg->local.serv;
	} else {
	    host = arg->remote.host;
	    port = arg->remote.serv;
	}

	rsock_syserr_fail_host_port(error, syscall, host, port);
    }

    arg->fd = -1;

    if (type == INET_SERVER) {
	status = listen(fd, SOMAXCONN);
	if (status < 0) {
	    error = errno;
	    close(fd);
	    rb_syserr_fail(error, "listen(2)");
	}
    }

    /* create new instance */
    return rsock_init_sock(arg->sock, fd);
}

static int
wait_connectable(int fd, struct timeval *timeout)
{
    int sockerr, revents;
    socklen_t sockerrlen;

    sockerrlen = (socklen_t)sizeof(sockerr);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, (void *)&sockerr, &sockerrlen) < 0)
        return -1;

    /* necessary for non-blocking sockets (at least ECONNREFUSED) */
    switch (sockerr) {
      case 0:
        break;
#ifdef EALREADY
      case EALREADY:
#endif
#ifdef EISCONN
      case EISCONN:
#endif
#ifdef ECONNREFUSED
      case ECONNREFUSED:
#endif
#ifdef EHOSTUNREACH
      case EHOSTUNREACH:
#endif
        errno = sockerr;
        return -1;
    }

    /*
     * Stevens book says, successful finish turn on RB_WAITFD_OUT and
     * failure finish turn on both RB_WAITFD_IN and RB_WAITFD_OUT.
     * So it's enough to wait only RB_WAITFD_OUT and check the pending error
     * by getsockopt().
     *
     * Note: rb_wait_for_single_fd already retries on EINTR/ERESTART
     */
    revents = rb_wait_for_single_fd(fd, RB_WAITFD_IN|RB_WAITFD_OUT, timeout);

    if (revents < 0)
        return -1;

    sockerrlen = (socklen_t)sizeof(sockerr);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, (void *)&sockerr, &sockerrlen) < 0)
        return -1;

    switch (sockerr) {
      case 0:
      /*
       * be defensive in case some platforms set SO_ERROR on the original,
       * interrupted connect()
       */

	/* when the connection timed out, no errno is set and revents is 0. */
	if (timeout && revents == 0) {
	    errno = ETIMEDOUT;
	    return -1;
	}
      case EINTR:
#ifdef ERESTART
      case ERESTART:
#endif
      case EAGAIN:
#ifdef EINPROGRESS
      case EINPROGRESS:
#endif
#ifdef EALREADY
      case EALREADY:
#endif
#ifdef EISCONN
      case EISCONN:
#endif
	return 0; /* success */
      default:
        /* likely (but not limited to): ECONNREFUSED, ETIMEDOUT, EHOSTUNREACH */
        errno = sockerr;
        return -1;
    }

    return 0;
}

static VALUE
init_inetsock_internal_happy(VALUE v)
{
    struct inetsock_arg *arg = (void *)v;
    int error = 0;
    int type = arg->type;
    struct addrinfo *res;
    int fd, status = 0;
    int family = AF_UNSPEC;
    const char *syscall = 0;
    /* int preferred_family = 0; */
    struct timeval timeout;
    int i, maxfd = 0, oflags;
    rb_fdset_t writefds;
    VALUE fds_ary = rb_ary_tmp_new(1);

#ifdef HAVE_GETADDRINFO_A
    arg->remote.res = rsock_addrinfo_a(arg->remote.host, arg->remote.serv,
				       family, SOCK_STREAM, 0,
				       arg->resolv_timeout);
#else
    arg->remote.res = rsock_addrinfo(arg->remote.host, arg->remote.serv,
				     family, SOCK_STREAM, 0);
#endif

    arg->fd = fd = -1;

    /* set timeout for wait_connectable() */
    timeout.tv_sec = 0;
    timeout.tv_usec = 250000; /* 250ms is a recommended value in RFC8305 */

    rb_fd_init(&writefds);

    for (res = arg->remote.res->ai; res; res = res->ai_next) {
        printf("%d\n", res->ai_family == AF_INET6);
#if !defined(INET6) && defined(AF_INET6)
	if (res->ai_family == AF_INET6)
	    continue;
#endif
        res->ai_socktype |= SOCK_NONBLOCK;
	status = rsock_socket(res->ai_family,res->ai_socktype,res->ai_protocol);
	syscall = "socket(2)";
	fd = status;
	if (fd < 0) {
	    error = errno;
	    continue;
	}
	arg->fd = fd;
        if (status >= 0) {
            /* status = rsock_connect(fd, res->ai_addr, res->ai_addrlen, */
            /*                        (type == INET_SOCKS)); */
            status = connect(fd, res->ai_addr, res->ai_addrlen);
            syscall = "connect(2)";
        }

	if (status < 0 && errno != EINPROGRESS) {
	    error = errno;
	    close(fd);
	    arg->fd = fd = -1;
	    continue;
	} else {
            /* preferred_family = res->ai_family; */
            status = wait_connectable(fd, &timeout);
            if (status == -1) {
                if (errno == ETIMEDOUT) {
                    rb_fd_set(fd, &writefds);
                    rb_ary_push(fds_ary, INT2FIX(fd));
                    if (fd > maxfd) { maxfd = fd; }
                }
                continue;
            }
            rb_fd_set(fd, &writefds);
            rb_ary_push(fds_ary, INT2FIX(fd));
            if (fd > maxfd) { maxfd = fd; }
            break;
        }
    }

    printf("select\n");
    status = rb_fd_select(maxfd+1, NULL, &writefds, NULL, NULL);
    /* status = rb_thread_fd_select(maxfd+1, &writefds, &writefds, NULL, NULL); */

    for (i=0; i<RARRAY_LEN(fds_ary); i++) {
	int _fd = FIX2INT(RARRAY_AREF(fds_ary, i));
        if (rb_fd_isset(_fd, &writefds)) { fd = _fd; }
    }

    /* TODO: close fds */

    if (status < 0) {
	VALUE host, port;

        host = arg->remote.host;
        port = arg->remote.serv;

	rsock_syserr_fail_host_port(error, syscall, host, port);
    }

    arg->fd = -1;

    /* unset nonblock flag */
    oflags = fcntl(fd, F_GETFL);
    if (oflags == -1) { rb_sys_fail(0); }
    oflags &= ~O_NONBLOCK;
    if (fcntl(fd, F_SETFL, oflags) == -1) {
	rb_sys_fail(0);
    }

    /* create new instance */
    return rsock_init_sock(arg->sock, fd);
}

VALUE
rsock_init_inetsock(VALUE sock, VALUE remote_host, VALUE remote_serv,
	            VALUE local_host, VALUE local_serv, int type,
		    VALUE resolv_timeout)
{
    struct inetsock_arg arg;
    arg.sock = sock;
    arg.remote.host = remote_host;
    arg.remote.serv = remote_serv;
    arg.remote.res = 0;
    arg.local.host = local_host;
    arg.local.serv = local_serv;
    arg.local.res = 0;
    arg.type = type;
    arg.fd = -1;
    arg.resolv_timeout = resolv_timeout;

    if (type == INET_CLIENT && NIL_P(local_host) && NIL_P(local_serv)) {
        return rb_ensure(init_inetsock_internal_happy, (VALUE)&arg,
                         inetsock_cleanup, (VALUE)&arg);
    }

    return rb_ensure(init_inetsock_internal, (VALUE)&arg,
		     inetsock_cleanup, (VALUE)&arg);
}

static ID id_numeric, id_hostname;

int
rsock_revlookup_flag(VALUE revlookup, int *norevlookup)
{
#define return_norevlookup(x) {*norevlookup = (x); return 1;}
    ID id;

    switch (revlookup) {
      case Qtrue:  return_norevlookup(0);
      case Qfalse: return_norevlookup(1);
      case Qnil: break;
      default:
	Check_Type(revlookup, T_SYMBOL);
	id = SYM2ID(revlookup);
	if (id == id_numeric) return_norevlookup(1);
	if (id == id_hostname) return_norevlookup(0);
	rb_raise(rb_eArgError, "invalid reverse_lookup flag: :%s", rb_id2name(id));
    }
    return 0;
#undef return_norevlookup
}

/*
 * call-seq:
 *   ipsocket.inspect   -> string
 *
 * Return a string describing this IPSocket object.
 */
static VALUE
ip_inspect(VALUE sock)
{
    VALUE str = rb_call_super(0, 0);
    rb_io_t *fptr = RFILE(sock)->fptr;
    union_sockaddr addr;
    socklen_t len = (socklen_t)sizeof addr;
    ID id;
    if (fptr && fptr->fd >= 0 &&
	getsockname(fptr->fd, &addr.addr, &len) >= 0 &&
	(id = rsock_intern_family(addr.addr.sa_family)) != 0) {
	VALUE family = rb_id2str(id);
	char hbuf[1024], pbuf[1024];
	long slen = RSTRING_LEN(str);
	const char last = (slen > 1 && RSTRING_PTR(str)[slen - 1] == '>') ?
	    (--slen, '>') : 0;
	str = rb_str_subseq(str, 0, slen);
	rb_str_cat_cstr(str, ", ");
	rb_str_append(str, family);
	if (!rb_getnameinfo(&addr.addr, len, hbuf, sizeof(hbuf),
			    pbuf, sizeof(pbuf), NI_NUMERICHOST | NI_NUMERICSERV)) {
	    rb_str_cat_cstr(str, ", ");
	    rb_str_cat_cstr(str, hbuf);
	    rb_str_cat_cstr(str, ", ");
	    rb_str_cat_cstr(str, pbuf);
	}
	if (last) rb_str_cat(str, &last, 1);
    }
    return str;
}

/*
 * call-seq:
 *   ipsocket.addr([reverse_lookup]) => [address_family, port, hostname, numeric_address]
 *
 * Returns the local address as an array which contains
 * address_family, port, hostname and numeric_address.
 *
 * If +reverse_lookup+ is +true+ or +:hostname+,
 * hostname is obtained from numeric_address using reverse lookup.
 * Or if it is +false+, or +:numeric+,
 * hostname is same as numeric_address.
 * Or if it is +nil+ or omitted, obeys to +ipsocket.do_not_reverse_lookup+.
 * See +Socket.getaddrinfo+ also.
 *
 *   TCPSocket.open("www.ruby-lang.org", 80) {|sock|
 *     p sock.addr #=> ["AF_INET", 49429, "hal", "192.168.0.128"]
 *     p sock.addr(true)  #=> ["AF_INET", 49429, "hal", "192.168.0.128"]
 *     p sock.addr(false) #=> ["AF_INET", 49429, "192.168.0.128", "192.168.0.128"]
 *     p sock.addr(:hostname)  #=> ["AF_INET", 49429, "hal", "192.168.0.128"]
 *     p sock.addr(:numeric)   #=> ["AF_INET", 49429, "192.168.0.128", "192.168.0.128"]
 *   }
 *
 */
static VALUE
ip_addr(int argc, VALUE *argv, VALUE sock)
{
    rb_io_t *fptr;
    union_sockaddr addr;
    socklen_t len = (socklen_t)sizeof addr;
    int norevlookup;

    GetOpenFile(sock, fptr);

    if (argc < 1 || !rsock_revlookup_flag(argv[0], &norevlookup))
	norevlookup = fptr->mode & FMODE_NOREVLOOKUP;
    if (getsockname(fptr->fd, &addr.addr, &len) < 0)
	rb_sys_fail("getsockname(2)");
    return rsock_ipaddr(&addr.addr, len, norevlookup);
}

/*
 * call-seq:
 *   ipsocket.peeraddr([reverse_lookup]) => [address_family, port, hostname, numeric_address]
 *
 * Returns the remote address as an array which contains
 * address_family, port, hostname and numeric_address.
 * It is defined for connection oriented socket such as TCPSocket.
 *
 * If +reverse_lookup+ is +true+ or +:hostname+,
 * hostname is obtained from numeric_address using reverse lookup.
 * Or if it is +false+, or +:numeric+,
 * hostname is same as numeric_address.
 * Or if it is +nil+ or omitted, obeys to +ipsocket.do_not_reverse_lookup+.
 * See +Socket.getaddrinfo+ also.
 *
 *   TCPSocket.open("www.ruby-lang.org", 80) {|sock|
 *     p sock.peeraddr #=> ["AF_INET", 80, "carbon.ruby-lang.org", "221.186.184.68"]
 *     p sock.peeraddr(true)  #=> ["AF_INET", 80, "carbon.ruby-lang.org", "221.186.184.68"]
 *     p sock.peeraddr(false) #=> ["AF_INET", 80, "221.186.184.68", "221.186.184.68"]
 *     p sock.peeraddr(:hostname) #=> ["AF_INET", 80, "carbon.ruby-lang.org", "221.186.184.68"]
 *     p sock.peeraddr(:numeric)  #=> ["AF_INET", 80, "221.186.184.68", "221.186.184.68"]
 *   }
 *
 */
static VALUE
ip_peeraddr(int argc, VALUE *argv, VALUE sock)
{
    rb_io_t *fptr;
    union_sockaddr addr;
    socklen_t len = (socklen_t)sizeof addr;
    int norevlookup;

    GetOpenFile(sock, fptr);

    if (argc < 1 || !rsock_revlookup_flag(argv[0], &norevlookup))
	norevlookup = fptr->mode & FMODE_NOREVLOOKUP;
    if (getpeername(fptr->fd, &addr.addr, &len) < 0)
	rb_sys_fail("getpeername(2)");
    return rsock_ipaddr(&addr.addr, len, norevlookup);
}

/*
 * call-seq:
 *   ipsocket.recvfrom(maxlen)        => [mesg, ipaddr]
 *   ipsocket.recvfrom(maxlen, flags) => [mesg, ipaddr]
 *
 * Receives a message and return the message as a string and
 * an address which the message come from.
 *
 * _maxlen_ is the maximum number of bytes to receive.
 *
 * _flags_ should be a bitwise OR of Socket::MSG_* constants.
 *
 * ipaddr is same as IPSocket#{peeraddr,addr}.
 *
 *   u1 = UDPSocket.new
 *   u1.bind("127.0.0.1", 4913)
 *   u2 = UDPSocket.new
 *   u2.send "uuuu", 0, "127.0.0.1", 4913
 *   p u1.recvfrom(10) #=> ["uuuu", ["AF_INET", 33230, "localhost", "127.0.0.1"]]
 *
 */
static VALUE
ip_recvfrom(int argc, VALUE *argv, VALUE sock)
{
    return rsock_s_recvfrom(sock, argc, argv, RECV_IP);
}

/*
 * call-seq:
 *   IPSocket.getaddress(host)        => ipaddress
 *
 * Lookups the IP address of _host_.
 *
 *   require 'socket'
 *
 *   IPSocket.getaddress("localhost")     #=> "127.0.0.1"
 *   IPSocket.getaddress("ip6-localhost") #=> "::1"
 *
 */
static VALUE
ip_s_getaddress(VALUE obj, VALUE host)
{
    union_sockaddr addr;
    struct rb_addrinfo *res = rsock_addrinfo(host, Qnil, AF_UNSPEC, SOCK_STREAM, 0);
    socklen_t len = res->ai->ai_addrlen;

    /* just take the first one */
    memcpy(&addr, res->ai->ai_addr, len);
    rb_freeaddrinfo(res);

    return rsock_make_ipaddr(&addr.addr, len);
}

void
rsock_init_ipsocket(void)
{
    /*
     * Document-class: IPSocket < BasicSocket
     *
     * IPSocket is the super class of TCPSocket and UDPSocket.
     */
    rb_cIPSocket = rb_define_class("IPSocket", rb_cBasicSocket);
    rb_define_method(rb_cIPSocket, "inspect", ip_inspect, 0);
    rb_define_method(rb_cIPSocket, "addr", ip_addr, -1);
    rb_define_method(rb_cIPSocket, "peeraddr", ip_peeraddr, -1);
    rb_define_method(rb_cIPSocket, "recvfrom", ip_recvfrom, -1);
    rb_define_singleton_method(rb_cIPSocket, "getaddress", ip_s_getaddress, 1);
    rb_undef_method(rb_cIPSocket, "getpeereid");

    id_numeric = rb_intern_const("numeric");
    id_hostname = rb_intern_const("hostname");
}
