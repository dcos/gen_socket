/*
 * Copyright (c) 2010-2012, Travelping GmbH <info@travelping.com
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *
 * Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution.
 *
 * Neither the name of the author nor the names of its contributors
 * may be used to endorse or promote products derived from this software
 * without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef _GNU_SOURCE
#  define _GNU_SOURCE
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <limits.h>
#include <fcntl.h>
#include <err.h>
#include <sched.h>

#include <unistd.h>
#include <sys/types.h>

#include <netinet/in.h>
#include <sys/un.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <net/if.h>

#include <sys/errno.h>

#include <sys/ioctl.h>

#include <linux/errqueue.h>

#include "erl_nif.h"
#include "erl_driver.h" // for erl_errno_id

#ifndef UNIX_PATH_MAX
    #define UNIX_PATH_MAX  sizeof(((struct sockaddr_un *)0)->sun_path)
#endif

static ERL_NIF_TERM atom_ok;
static ERL_NIF_TERM atom_eof;
static ERL_NIF_TERM atom_error;

// AF NAMES
static ERL_NIF_TERM atom_unix;
static ERL_NIF_TERM atom_inet4;

static ERL_NIF_TERM atom_sock_err;

static ErlNifResourceType *rsrc_sock;

// -------------------------------------------------------------------------------------------------
// -- MISC INTERNAL HELPER FUNCTIONS
#define enif_get_ssize enif_get_long

inline static ERL_NIF_TERM
error_tuple(ErlNifEnv *env, int errnum)
{
    return enif_make_tuple2(env, atom_error, enif_make_atom(env, erl_errno_id(errnum)));
}

#define fionread(sock, len)\
    do {\
    if (len < 0) {\
        int i;\
        if (ioctl((sock), FIONREAD, &i) < 0) {\
            return error_tuple(env, errno);\
        } else {\
            len = i;\
        }\
    }\
    if (len == 0) {\
        len = 1024;\
    }\
    if (len > SSIZE_MAX) {\
        len = SSIZE_MAX;\
    }\
    } while (0)

// -------------------------------------------------------------------------------------------------
// -- SOCKADDR TO ERLANG TERM
static ERL_NIF_TERM
sockaddr_unix_to_term(ErlNifEnv* env, const struct sockaddr_un* addr)
{
    ERL_NIF_TERM path_binary;
    size_t path_size = strlen(addr->sun_path);
    void* path_data = enif_make_new_binary(env, path_size, &path_binary);
    memcpy(path_data, addr->sun_path, path_size);

    // {unix, Path}
    return enif_make_tuple2(env, atom_unix, path_binary);
}

static ERL_NIF_TERM
sockaddr_inet4_to_term(ErlNifEnv* env, const struct sockaddr_in* addr)
{
    struct in_addr in_addr = addr->sin_addr;

    // {inet4, {A,B,C,D}, Port}
    return enif_make_tuple3(env,
                            atom_inet4,
                            enif_make_tuple4(env,
                                             enif_make_int(env, (in_addr.s_addr & 0x000000FF)),
                                             enif_make_int(env, (in_addr.s_addr & 0x0000FF00) >> 8),
                                             enif_make_int(env, (in_addr.s_addr & 0x00FF0000) >> 16),
                                             enif_make_int(env, (in_addr.s_addr & 0xFF000000) >> 24)),
                            enif_make_int(env, (int) ntohs(addr->sin_port)));
}

static ERL_NIF_TERM
sockaddr_unknown_to_term(ErlNifEnv* env, const struct sockaddr_storage* addr, socklen_t addrlen)
{
    ERL_NIF_TERM addr_binary;
    void* addr_data = enif_make_new_binary(env, addrlen, &addr_binary);
    memcpy(addr_data, addr, addrlen);

    // Address::binary()
    return addr_binary;
}

static ERL_NIF_TERM
sockaddr_to_term(ErlNifEnv* env, const struct sockaddr_storage* addr, const socklen_t addrlen)
{
    switch (((struct sockaddr *)addr)->sa_family) {
        case AF_UNIX:
            return sockaddr_unix_to_term(env, (struct sockaddr_un*) addr);
        case AF_INET:
            return sockaddr_inet4_to_term(env, (struct sockaddr_in*) addr);
        default:
            return sockaddr_unknown_to_term(env, addr, addrlen);
    }
}

// -------------------------------------------------------------------------------------------------
// -- ERLANG TERM TO SOCKADDR
static socklen_t
inet4_tuple_to_sockaddr(ErlNifEnv* env,
                        int arity,
                        const ERL_NIF_TERM* tuple,
                        struct sockaddr_in* addr,
                        socklen_t* addrlen)
{
    socklen_t required_addrlen = (socklen_t) sizeof(struct sockaddr_in);
    const ERL_NIF_TERM* ip_tuple;
    int ip_arity;
    unsigned int port = 0;
    unsigned int ip[4];

    if (arity != 3
        || !enif_get_tuple(env, tuple[1], &ip_arity, &ip_tuple)
        || ip_arity != 4
        || !enif_get_uint(env, ip_tuple[0], &ip[0]) || ip[0] > 255
        || !enif_get_uint(env, ip_tuple[1], &ip[1]) || ip[1] > 255
        || !enif_get_uint(env, ip_tuple[2], &ip[2]) || ip[2] > 255
        || !enif_get_uint(env, ip_tuple[3], &ip[3]) || ip[3] > 255
        || !enif_get_uint(env, tuple[2], &port) || port > UINT16_MAX) {
        return 0;
    }

    if (*addrlen >= required_addrlen) {
        addr->sin_family = AF_INET;
        addr->sin_port = htons((in_port_t) port);
        addr->sin_addr = (struct in_addr) {
            .s_addr = ip[0] | ip[1] << 8 | ip[2] << 16 | ip[3] << 24
        };
    }

    *addrlen = required_addrlen;
    return required_addrlen;
}

static socklen_t
unix_tuple_to_sockaddr(ErlNifEnv* env,
                       int arity,
                       const ERL_NIF_TERM* tuple,
                       struct sockaddr_un* addr,
                       socklen_t* addrlen)
{
    ErlNifBinary path;

    if (arity != 2
        || !enif_inspect_iolist_as_binary(env, tuple[1], &path)
        || path.size > UNIX_PATH_MAX) {
        return 0;
    }

    socklen_t required_addrlen = (socklen_t) offsetof(struct sockaddr_un, sun_path) + path.size + 1;

    if (*addrlen >= required_addrlen) {
        addr->sun_family = AF_UNIX;
        addr->sun_path[path.size] = 0;
        memcpy(addr->sun_path, path.data, path.size);
    }

    *addrlen = required_addrlen;
    return required_addrlen;
}

static socklen_t
term_to_sockaddr(ErlNifEnv* env, ERL_NIF_TERM term, struct sockaddr* addr, socklen_t* addrlen)
{
    ErlNifBinary addr_bin;
    const ERL_NIF_TERM* tuple;
    int arity;

    // binaries are used as-is
    if (enif_inspect_binary(env, term, &addr_bin)) {
        if (*addrlen >= addr_bin.size)
            memcpy(addr, addr_bin.data, addr_bin.size);
        *addrlen = addr_bin.size;
        return addr_bin.size;
    }

    if (enif_get_tuple(env, term, &arity, &tuple) && (arity > 1)) {
        if (enif_is_identical(tuple[0], atom_inet4)) {
            return inet4_tuple_to_sockaddr(env, arity, tuple, (struct sockaddr_in*) addr, addrlen);
        } else if (enif_is_identical(tuple[0], atom_unix)) {
            return unix_tuple_to_sockaddr(env, arity, tuple, (struct sockaddr_un*) addr, addrlen);
        } else {
            return 0;
        }
    } else {
        return 0;
    }
}

static void rsrc_sock_dtor(ErlNifEnv* env, void* obj) {
    int *sock, s;
    if(!obj) {
        return;
    }
    sock = obj;
    s = *sock;
    *sock = -1;
    if(s < 0) {
        return;
    }
    fprintf(stderr, "rsrc_sock_dtor %p %d\n", sock, (int)s);
    close(s);
}

// -------------------------------------------------------------------------------------------------
// -- NIF FUNCTIONS
static int load(ErlNifEnv* env, void** priv_data, ERL_NIF_TERM load_info)
{
    atom_ok      = enif_make_atom(env, "ok");
    atom_eof     = enif_make_atom(env, "eof");
    atom_error   = enif_make_atom(env, "error");
    atom_unix    = enif_make_atom(env, "unix");
    atom_inet4   = enif_make_atom(env, "inet4");
    atom_sock_err = enif_make_atom(env, "sock_err");
    rsrc_sock     = enif_open_resource_type(env, "gen_socket", "socket_resource", rsrc_sock_dtor, ERL_NIF_RT_CREATE, 0);
    if(!rsrc_sock) {
        enif_raise_exception(env, enif_make_atom(env, "error_enif_open_resource_type"));
        return -1;
    }
    return 0;
}
// param 0: socket address term
static ERL_NIF_TERM
nif_encode_sockaddr(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
    struct sockaddr* addr = NULL;
    socklen_t addrlen = 0;
    ERL_NIF_TERM addr_bin;

    // dry run to get size
    if (!term_to_sockaddr(env, argv[0], NULL, &addrlen))
        return enif_make_badarg(env);

    addr = (struct sockaddr*) enif_make_new_binary(env, addrlen, &addr_bin);

    if (!term_to_sockaddr(env, argv[0], addr, &addrlen))
        return enif_make_badarg(env);

    return addr_bin;
}

// param 0: socket address binary ad returned by nif_encode_sockaddr/1
static ERL_NIF_TERM
nif_decode_sockaddr(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
    ErlNifBinary addr_bin;

    if (!enif_inspect_binary(env, argv[0], &addr_bin))
        return enif_make_badarg(env);

    return sockaddr_to_term(env, (struct sockaddr_storage *)addr_bin.data, (socklen_t)addr_bin.size);
}

// param 0: socket
static ERL_NIF_TERM
nif_getsockname(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
    int *sock;
    struct sockaddr_storage addr;
    socklen_t addrlen = sizeof(addr);

    if (!enif_get_resource(env, argv[0], rsrc_sock, (void**)&sock))
        return enif_make_badarg(env);

    if (getsockname(*sock, (struct sockaddr*) &addr, &addrlen) != 0)
        return error_tuple(env, errno);

    return sockaddr_to_term(env, &addr, addrlen);
}

// param 0: socket
static ERL_NIF_TERM
nif_getpeername(ErlNifEnv* env, int argc, const ERL_NIF_TERM argv[])
{
    int *sock;
    struct sockaddr_storage addr;
    socklen_t addrlen = sizeof(addr);

    if (!enif_get_resource(env, argv[0], rsrc_sock, (void**)&sock))
        return enif_make_badarg(env);

    if (getpeername(*sock, (struct sockaddr*) &addr, &addrlen) != 0)
        return error_tuple(env, errno);

    return sockaddr_to_term(env, &addr, addrlen);
}

// param 0: socket descriptor
// param 1: socket address term
static ERL_NIF_TERM
nif_bind(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
    int *sock;
    struct sockaddr_storage addr;
    socklen_t addrlen = sizeof(addr);

    if (!enif_get_resource(env, argv[0], rsrc_sock, (void**)&sock)
    || !term_to_sockaddr(env, argv[1], (struct sockaddr*) &addr, &addrlen))
        return enif_make_badarg(env);

    if (addrlen > sizeof(addr))
        //TODO: should be EINVAL
        return error_tuple(env, E2BIG);

    if (bind(*sock, (struct sockaddr*) &addr, addrlen))
        return error_tuple(env, errno);

    return atom_ok;
}

// param 0: socket descriptor
// param 1: socket address term
static ERL_NIF_TERM
nif_connect(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
    int *sock;
    struct sockaddr_storage addr;
    socklen_t addrlen = sizeof(addr);

    if (!enif_get_resource(env, argv[0], rsrc_sock, (void**)&sock)
    || !term_to_sockaddr(env, argv[1], (struct sockaddr*) &addr, &addrlen))
        return enif_make_badarg(env);

    if (addrlen > sizeof(addr))
        //TODO: should be EINVAL
        return error_tuple(env, E2BIG);

    if (connect(*sock, (struct sockaddr*) &addr, addrlen)) {
        return error_tuple(env, errno);
    }
    return atom_ok;
}
/*  0: procotol, 1: type, 2: family */
static ERL_NIF_TERM
nif_socket(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
    int family = 0;
    int type = 0;
    int protocol = 0;
    int flags = 0;
    int *sock;
    ERL_NIF_TERM term;

    sock = enif_alloc_resource(rsrc_sock, sizeof(*sock)); 
    if(!sock) {
        return error_tuple(env, ENOMEM);
    }
    if (!enif_get_int(env, argv[0], &family)
    || !enif_get_int(env, argv[1], &type) 
    || !enif_get_int(env, argv[2], &protocol)) {
      return enif_make_badarg(env);
    }

    *sock = socket(family, type, protocol);
    if (*sock < 0) {
        return error_tuple(env, errno);
    }

    flags = fcntl(*sock, F_GETFL, 0);
    flags |= O_NONBLOCK;
    (void)fcntl(*sock, F_SETFL, flags);

    term = enif_make_resource(env, sock);
    enif_release_resource(sock);
    return enif_make_tuple(env, 2,
           atom_ok,
           term);
}

/*  0: netnsfile, 1: procotol, 2: type, 3: family */
static ERL_NIF_TERM
nif_socketat(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
    ErlNifBinary netnsfile;
    int family = 0;
    int type = 0;
    int protocol = 0;
    int flags = 0;
    int errsv;
    char filename[PATH_MAX];
    int nsfd = 0;
    sigset_t intmask, oldmask;
    int old_nsfd;
    int *sock;
    ERL_NIF_TERM term;

    sock = enif_alloc_resource(rsrc_sock, sizeof(*sock)); 
    if(!sock) {
        return error_tuple(env, ENOMEM);
    }
    if (!enif_inspect_binary(env, argv[0], &netnsfile)
    || netnsfile.size > PATH_MAX -1
    || !enif_get_int(env, argv[1], &family)
    || !enif_get_int(env, argv[2], &type) 
    || !enif_get_int(env, argv[3], &protocol)) {
        return enif_make_badarg(env);
    }

    memcpy(filename, netnsfile.data, netnsfile.size);
    filename[netnsfile.size] = '\0';
    if ((nsfd = open(filename, O_RDONLY)) < 0)
        return error_tuple(env, errno);

    if ((old_nsfd = open("/proc/self/ns/net", O_RDONLY)) < 0) {
          errsv = errno;
          close(nsfd);
        return error_tuple(env, errsv);
    }

    sigfillset(&intmask);
    sigprocmask(SIG_BLOCK, &intmask, &oldmask);

    setns(nsfd, CLONE_NEWNET);
    *sock = socket(family, type, protocol);
    errsv = errno;
    setns(old_nsfd, CLONE_NEWNET);

    sigprocmask(SIG_SETMASK, &oldmask, NULL);

    close(nsfd);
    close(old_nsfd);

    if (*sock < 0)
        return error_tuple(env, errsv);

    flags = fcntl(*sock, F_GETFL, 0);
    flags |= O_NONBLOCK;
    (void)fcntl(*sock, F_SETFL, flags);

    term = enif_make_resource(env, sock);
    enif_release_resource(sock);
    return enif_make_tuple(env, 2,
           atom_ok,
           term);
}

// param 0: socket
// param 1: backlog
static ERL_NIF_TERM
nif_listen(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
    int *sock;
    int backlog = 5;

    if (!enif_get_resource(env, argv[0], rsrc_sock, (void**)&sock)
    || !enif_get_int(env, argv[1], &backlog))
        return enif_make_badarg(env);

    if (listen(*sock, backlog) < 0)
        return error_tuple(env, errno);

    return atom_ok;
}

// param 0: socket
static ERL_NIF_TERM
nif_accept(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
    int *sock, *newfd;
    struct sockaddr_storage addr;
    ERL_NIF_TERM term;

    socklen_t addrlen = sizeof(addr);

    newfd = enif_alloc_resource(rsrc_sock, sizeof(*newfd)); 
    if(!newfd)
        return error_tuple(env, ENOMEM);

    if (!enif_get_resource(env, argv[0], rsrc_sock, (void**)&sock))
        return enif_make_badarg(env);

    if ((*newfd = accept(*sock, (struct sockaddr*)&addr, &addrlen)) < 0)
        return error_tuple(env, errno);

    term = enif_make_resource(env, newfd);
    enif_release_resource(newfd);
    return enif_make_tuple3(env, atom_ok,
                            term,
                            sockaddr_to_term(env, &addr, addrlen));
}


// param 0: socket
// param 1: number of bytes to receive, determined automatically if negative
static ERL_NIF_TERM
nif_recv(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
    int *sock = 0;
    ErlNifBinary buffer;
    ssize_t len = 0;

    if (!enif_get_resource(env, argv[0], rsrc_sock, (void**)&sock)) {
        fprintf(stderr, "bad argument 0\n");
        return enif_make_badarg(env);
    }
    if(!enif_get_ssize(env, argv[1], &len)) {
        fprintf(stderr, "bad argument 1\n");
        return enif_make_badarg(env);
    }
    fionread(*sock, len);

    if (!enif_alloc_binary(len, &buffer))
        return enif_make_badarg(env);

    while (42) { 
      if ((len = recv(*sock, buffer.data, (size_t) len, MSG_DONTWAIT)) >= 0)
        break;

    switch (errno) {
    case EINTR:
        continue;

    default:
        enif_release_binary(&buffer);
        return error_tuple(env, errno);
    }
    }

    if (len == 0) {
        enif_release_binary(&buffer);
        return atom_eof;
    }

    if (len < buffer.size)
        enif_realloc_binary(&buffer, len);

    return enif_make_tuple2(env, atom_ok, enif_make_binary(env, &buffer));
}

// param 0: socket
// param 1: flags for recvmsg
// param 2: number of bytes to receive, determined automatically if negative
static ERL_NIF_TERM
nif_recvmsg(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
    int *sock;
    int flag;
    ssize_t len = 0;
    ssize_t r = 0;

    char buffer[2048];
    struct iovec iov;                       /* Data array */
    struct msghdr msg;                      /* Message header */
    struct cmsghdr *cmsg;                   /* Control related data */
    struct sock_extended_err *sock_err;     /* Struct describing the error */
    struct sockaddr_in remote;              /* Our socket */

    ERL_NIF_TERM clist;
    ErlNifBinary data;

    if (!enif_get_resource(env, argv[0], rsrc_sock, (void**)&sock)
    || !enif_get_int(env, argv[1], &flag)
    || !enif_get_ssize(env, argv[2], &len))
        return enif_make_badarg(env);

    fionread(*sock, len);

    if (!enif_alloc_binary(len, &data))
        return error_tuple(env, ENOMEM);

    iov.iov_base = data.data;
    iov.iov_len = data.size;
    msg.msg_name = (void*)&remote;
    msg.msg_namelen = sizeof(remote);
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_flags = 0;
    msg.msg_control = buffer;
    msg.msg_controllen = sizeof(buffer);

    while (42) {
        if ((r = recvmsg(*sock, &msg, flag)) >= 0)
            break;

        switch (errno) {
        case EINTR:
            continue;

        default:
            enif_release_binary(&data);
            return error_tuple(env, errno);
        }
    }

    if (iov.iov_len < data.size)
        enif_realloc_binary(&data, iov.iov_len);


    clist = enif_make_list(env, 0);

    /* Control messages are always accessed via some macros
     * http://www.kernel.org/doc/man-pages/online/pages/man3/cmsg.3.html
     */
    for (cmsg = CMSG_FIRSTHDR(&msg); cmsg; cmsg = CMSG_NXTHDR(&msg, cmsg))  {
        ERL_NIF_TERM val;
        ERL_NIF_TERM decoded;

        /* Ip level */
        if ((cmsg->cmsg_level == SOL_IP &&
             cmsg->cmsg_type == IP_RECVERR) ||
            (cmsg->cmsg_level == SOL_IPV6 &&
             cmsg->cmsg_type == IPV6_RECVERR)) {

            sock_err = (struct sock_extended_err*)CMSG_DATA(cmsg);
            decoded = enif_make_tuple7(env,
                           atom_sock_err,
                           enif_make_int(env, sock_err->ee_errno),
                           enif_make_int(env, sock_err->ee_origin),
                           enif_make_int(env, sock_err->ee_type),
                           enif_make_int(env, sock_err->ee_code),
                           enif_make_int(env, sock_err->ee_info),
                           enif_make_int(env, sock_err->ee_data));
        } else {
            memcpy(enif_make_new_binary(env, cmsg->cmsg_len - sizeof(struct cmsghdr), &decoded),
               CMSG_DATA(cmsg), cmsg->cmsg_len - sizeof(struct cmsghdr));
        }

        val = enif_make_tuple3(env,
                       enif_make_int(env, cmsg->cmsg_level),
                       enif_make_int(env, cmsg->cmsg_type),
                       decoded);
        clist = enif_make_list_cell(env, val, clist);
    }

    return enif_make_tuple4(env,
                            atom_ok,
                            sockaddr_to_term(env, msg.msg_name, msg.msg_namelen),
                            clist, enif_make_binary(env, &data));
}

// param 0: socket
// param 1: number of bytes to receive, determined automatically if negative
static ERL_NIF_TERM
nif_recvfrom(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
    int *sock;
    struct sockaddr_storage addr;
    socklen_t addrlen = sizeof(addr);
    ssize_t len = 0;

    ErlNifBinary buffer;

    if (!enif_get_resource(env, argv[0], rsrc_sock, (void**)&sock)
    || !enif_get_ssize(env, argv[1], &len))
        return enif_make_badarg(env);

    fionread(*sock, len);

    if (!enif_alloc_binary(len, &buffer))
        return error_tuple(env, ENOMEM);

    while (42) {
        if ((len = recvfrom(*sock, buffer.data, len, MSG_DONTWAIT, (struct sockaddr*)&addr, &addrlen)) >= 0)
            break;

        switch (errno) {
            case EINTR:
                continue;
            default:
                enif_release_binary(&buffer);
                return error_tuple(env, errno);
        }
    }

    if (len == 0) {
    enif_release_binary(&buffer);
    return atom_eof;
    }

    if (len < buffer.size)
        enif_realloc_binary(&buffer, len);

    return enif_make_tuple3(env,
                            atom_ok,
                            sockaddr_to_term(env, &addr, addrlen),
                            enif_make_binary(env, &buffer));
}

// param 0: socket
// param 1: data
// param 2: flags
static ERL_NIF_TERM
nif_send(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
    int *sock;
    ErlNifBinary data;
    int flags;
    ssize_t len;

    if (!enif_get_resource(env, argv[0], rsrc_sock, (void**)&sock)
    || !enif_inspect_binary(env, argv[1], &data)
    || !enif_get_int(env, argv[2], &flags))
        return enif_make_badarg(env);

    flags |= MSG_NOSIGNAL;
    flags |= MSG_DONTWAIT;

    while (42) {
        if ((len = send(*sock, data.data, data.size, flags)) >= 0)
            break;

        switch (errno) {
            case EINTR:
                continue;
            default:
                return error_tuple(env, errno);
        }
    }

    return enif_make_tuple2(env, atom_ok, 
                enif_make_sub_binary(env, argv[1], len, data.size - len));
}

// param 0: socket
// param 1: data
// param 2: flags
// param 3: sockaddr term
static ERL_NIF_TERM
nif_sendto(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
    int *sock;
    ErlNifBinary data;
    int flags;
    ssize_t len;
    struct sockaddr_storage addr;
    socklen_t addrlen = sizeof(addr);

    if (!enif_get_resource(env, argv[0], rsrc_sock, (void**)&sock)
    || !enif_inspect_binary(env, argv[1], &data)
    || !enif_get_int(env, argv[2], &flags)
    || !term_to_sockaddr(env, argv[3], (struct sockaddr*) &addr, &addrlen))
        return enif_make_badarg(env);
    flags |= MSG_NOSIGNAL;
    flags |= MSG_DONTWAIT;

    while (42) {
        if ((len = sendto(*sock, data.data, data.size, flags, (struct sockaddr*) &addr, addrlen)) >= 0)
            break;

        switch (errno) {
            case EINTR:
                continue;
            default:
                return error_tuple(env, errno);
        }
    }

    return enif_make_tuple2(env, atom_ok, 
                enif_make_sub_binary(env, argv[1], len, data.size - len));
}

// param 0: socket
// param 1: number of bytes to receive, determined automatically if negative
static ERL_NIF_TERM
nif_read(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
    int *sock;
    ErlNifBinary buffer;
    ssize_t len = 0;

    if (!enif_get_resource(env, argv[0], rsrc_sock, (void**)&sock)
    || !enif_get_ssize(env, argv[1], &len))
        return enif_make_badarg(env);

    fionread(*sock, len);

    if (!enif_alloc_binary(len, &buffer)) {
        return enif_make_badarg(env);
    }

    while (42) {
        if ((len = read(*sock, buffer.data, (size_t) len)) >= 0)
            break;

        switch (errno) {
            case EINTR:
                continue;
            default:
                enif_release_binary(&buffer);
                return error_tuple(env, errno);
        }
    }

    if (len == 0) {
        enif_release_binary(&buffer);
        return atom_eof;
    }

    if (len < buffer.size)
        enif_realloc_binary(&buffer, len);

    return enif_make_tuple2(env, atom_ok, enif_make_binary(env, &buffer));
}

// param 0: socket
// param 1: data
static ERL_NIF_TERM
nif_write(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
    int *sock;
    int len;
    ErlNifBinary data;

    if (!enif_get_resource(env, argv[0], rsrc_sock, (void**)&sock)
    || !enif_inspect_binary(env, argv[1], &data))
        return enif_make_badarg(env);

    while (42) {
        if ((len = write(*sock, data.data, data.size)) >= 0)
            break;

        switch (errno) {
            case EINTR:
                continue;
            default:
                return error_tuple(env, errno);
        }
    }

    return enif_make_tuple2(env, atom_ok, 
                enif_make_sub_binary(env, argv[1], len, data.size - len));
}

/* 0: (int)socket descriptor, 1: (int)device dependent request,
 * 2: (char *)argp, pointer to structure
 */
static ERL_NIF_TERM
nif_ioctl(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
    int *sock;
    int req = 0;
    ErlNifBinary arg;

    if (!enif_get_resource(env, argv[0], rsrc_sock, (void**)&sock)
    || !enif_get_int(env, argv[1], &req)
    || !enif_inspect_binary(env, argv[2], &arg))
        return enif_make_badarg(env);

    if (ioctl(*sock, req, arg.data) < 0)
        return error_tuple(env, errno);

    return enif_make_tuple2(env, atom_ok, enif_make_binary(env, &arg));
}

/* 0: socket resource, 1: int level,
 * 2: int optname, 3: void *optval
 */
static ERL_NIF_TERM
nif_setsockopt(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
    int *sock;
    int level = 0;
    int name = 0;
    ErlNifBinary val;

    if (!enif_get_resource(env, argv[0], rsrc_sock, (void**)&sock)
    || !enif_get_int(env, argv[1], &level)
    || !enif_get_int(env, argv[2], &name)
    || !enif_inspect_binary(env, argv[3], &val))
        return enif_make_badarg(env);

    if (setsockopt(*sock, level, name, (void *)val.data, val.size) < 0)
        return error_tuple(env, errno);

    return atom_ok;
}

/* 0: resource for the  socket descriptor, 1: int level,
 * 2: int optname
 * ret: void *optval
 */
static ERL_NIF_TERM
nif_getsockopt(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
    int *sock;
    int level = 0;
    int name = 0;
    socklen_t optlen = 64;
    ErlNifBinary opt, val;

    if (!enif_get_resource(env, argv[0], rsrc_sock, (void**)&sock)
    || !enif_get_int(env, argv[1], &level)
    || !enif_get_int(env, argv[2], &name)
    || !enif_inspect_binary(env, argv[3], &opt)
    || !enif_get_uint(env, argv[4], &optlen))
        return enif_make_badarg(env);

    if (!enif_alloc_binary(optlen, &val))
        return atom_error;

    memcpy(val.data, opt.data, optlen > opt.size ? opt.size : optlen);

    if (getsockopt(*sock, level, name, (void *)val.data, &optlen) < 0) {
        enif_release_binary(&val);
        return error_tuple(env, errno);
    }

    enif_realloc_binary(&val, optlen);
    return enif_make_binary(env, &val);
}

/* 0: socket resource
 * ret: posix_error()
 */
static ERL_NIF_TERM
nif_getsock_error(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
    int *sock;
    int opt = 0;
    socklen_t optlen = sizeof(opt);

    if (!enif_get_resource(env, argv[0], rsrc_sock, (void**)&sock))
        return enif_make_badarg(env);

    if (getsockopt(*sock, SOL_SOCKET, SO_ERROR, &opt, &optlen) < 0)
        return error_tuple(env, errno);

    if (opt != 0)
        return enif_make_atom(env, erl_errno_id(opt));
    else
        return atom_ok;
}

/* 0: socket resource
 * ret: posix_error()
 */
static ERL_NIF_TERM
nif_close(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
    int *sock, s;
    if (!enif_get_resource(env, argv[0], rsrc_sock, (void**)&sock))
        return enif_make_badarg(env);
    s = *sock;
    *sock = -1;
    if (close(s) < 0)
        return error_tuple(env, errno);

    return atom_ok;
}

/* 0: resource for the socket descriptor
 * 1: int how
 * ret: posix_error()
 */
static ERL_NIF_TERM
nif_shutdown(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
    int *sock;
    int how;

    if (!enif_get_resource(env, argv[0], rsrc_sock, (void**)&sock)
    || !enif_get_int(env, argv[1], &how))
        return enif_make_badarg(env);

    if (shutdown(*sock, how) < 0)
        return error_tuple(env, errno);

    return atom_ok;
}

/* 0: socket resource
 * ret: {ok, fd} | badarg
 */
static ERL_NIF_TERM
nif_getfd(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[])
{
    int *sock;
    if (!enif_get_resource(env, argv[0], rsrc_sock, (void**)&sock))
        return enif_make_badarg(env);
    return enif_make_tuple2(env, atom_ok,
                            enif_make_int(env, *sock));
}


static ErlNifFunc nif_funcs[] = {
    {"nif_encode_sockaddr", 1, nif_encode_sockaddr},
    {"nif_decode_sockaddr", 1, nif_decode_sockaddr},
    {"nif_getsockname",     1, nif_getsockname},
    {"nif_getpeername",     1, nif_getpeername},
    {"nif_setsockopt",      4, nif_setsockopt},
    {"nif_getsockopt",      5, nif_getsockopt},
    {"nif_getsock_error",   1, nif_getsock_error},
    {"nif_accept",          1, nif_accept},
    {"nif_connect",         2, nif_connect},
    {"nif_bind",            2, nif_bind},
    {"nif_recv",            2, nif_recv},
    {"nif_recvmsg",         3, nif_recvmsg},
    {"nif_recvfrom",        2, nif_recvfrom},
    {"nif_send",            3, nif_send},
    {"nif_sendto",          4, nif_sendto},
    {"nif_read",            2, nif_read},
    {"nif_write",           2, nif_write},
    {"nif_ioctl",           3, nif_ioctl},
    {"nif_socket",          3, nif_socket},
    {"nif_socketat",        4, nif_socketat},
    {"nif_listen",          2, nif_listen},
    {"nif_shutdown",        2, nif_shutdown},
    {"nif_close",           1, nif_close},
    {"nif_getfd",           1, nif_getfd},
};

ERL_NIF_INIT(gen_socket, nif_funcs, load, NULL, NULL, NULL)
