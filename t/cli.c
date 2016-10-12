/*
 * Copyright (c) 2016 DeNA Co., Ltd., Kazuho Oku
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */
#ifdef _WIN32
# ifndef _WIN32_WINNT
#  define _WIN32_WINNT 0x0600
# endif
#endif

#include <assert.h>
#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#ifndef _WIN32
# include <arpa/inet.h>
# include <netdb.h>
# include <netinet/in.h>
# include <sys/select.h>
# include <sys/socket.h>
typedef int sockopt_t;
#else
# include <ws2tcpip.h>
typedef char sockopt_t;
#endif
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/engine.h>
#include <openssl/pem.h>
#include "picotls.h"
#include "picotls/openssl.h"

#ifdef _WIN32
static void _win32_perror(const char* detail)
{
    LPSTR buf = NULL, p;

    DWORD e = WSAGetLastError();
    if (e == 0) {
        fprintf(stderr, "No errors: %s\n", detail);
        return;
    }

    FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER |
        FORMAT_MESSAGE_FROM_SYSTEM |
        FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL,
        e,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        (LPSTR)&buf,
        0,
        NULL);
    p = buf;
    while (*p) {
        if (*p == '\r' || *p == '\n') *p = ' ';
        p++;
    }
    fprintf(stderr, "%s: %s\n", buf, detail);
    LocalFree(buf);
}
#undef perror
#define perror(x) _win32_perror(x)
#endif

static int write_all(int fd, const uint8_t *data, size_t len)
{
    ssize_t wret;

    while (len != 0) {
        while ((wret = write(fd, data, len)) == -1 && errno == EINTR)
            ;
        if (wret <= 0)
            return -1;
        data += wret;
        len -= wret;
    }

    return 0;
}

static int run_handshake(int fd, ptls_t *tls, ptls_buffer_t *wbuf, uint8_t *pending_input, size_t *pending_input_len)
{
    size_t pending_input_bufsz = *pending_input_len;
    int ret;
    ssize_t rret = 0;

    *pending_input_len = 0;

    while ((ret = ptls_handshake(tls, wbuf, pending_input, pending_input_len)) == PTLS_ERROR_HANDSHAKE_IN_PROGRESS) {
        /* write to socket */
        if (write_all(fd, wbuf->base, wbuf->off) != 0)
            return -1;
        wbuf->off = 0;
        /* read from socket */
        while ((rret = read(fd, pending_input, pending_input_bufsz)) == -1 && errno == EINTR)
            ;
        if (rret <= 0)
            return -1;
        *pending_input_len = rret;
    }

    if (ret != 0) {
        fprintf(stderr, "ptls_handshake:%d\n", ret);
        return -1;
    }

    if (rret != *pending_input_len)
        memmove(pending_input, pending_input + *pending_input_len, rret - *pending_input_len);
    *pending_input_len = rret - *pending_input_len;
    return 0;
}

static int decrypt_and_print(ptls_t *tls, const uint8_t *input, size_t inlen)
{
    ptls_buffer_t decryptbuf;
    uint8_t decryptbuf_small[1024];
    int ret;

    ptls_buffer_init(&decryptbuf, decryptbuf_small, sizeof(decryptbuf_small));

    while (inlen != 0) {
        size_t consumed = inlen;
        if ((ret = ptls_receive(tls, &decryptbuf, input, &consumed)) != 0) {
            fprintf(stderr, "ptls_receive:%d\n", ret);
            return -1;
        }
        input += consumed;
        inlen -= consumed;
        if (decryptbuf.off != 0) {
            if (write_all(1, decryptbuf.base, decryptbuf.off) != 0)
                return -1;
            decryptbuf.off = 0;
        }
    }

    return 0;
}

static int handle_connection(int fd, ptls_context_t *ctx, const char *server_name)
{
    ptls_t *tls = ptls_new(ctx, server_name);
    uint8_t rbuf[1024], wbuf_small[1024];
    ptls_buffer_t wbuf;
    int ret;
    size_t roff;
    ssize_t rret;

    ptls_buffer_init(&wbuf, wbuf_small, sizeof(wbuf_small));

    roff = sizeof(rbuf);
    if (run_handshake(fd, tls, &wbuf, rbuf, &roff) != 0)
        goto Exit;

    if (write_all(fd, wbuf.base, wbuf.off) != 0)
        goto Exit;
    wbuf.off = 0;

    /* process pending post-handshake data (if any) */
    if (decrypt_and_print(tls, rbuf, roff) != 0)
        goto Exit;
    roff = 0;

    /* do the communication */
    while (1) {

        /* wait for either of STDIN or read-side of the socket to become available */
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(0, &readfds);
        FD_SET(fd, &readfds);
        if (select(fd + 1, &readfds, NULL, NULL, NULL) <= 0)
            continue;

        if (FD_ISSET(0, &readfds)) {
            /* read from stdin, encrypt and send */
            while ((rret = read(0, rbuf, sizeof(rbuf))) == 1 && errno == EINTR)
                ;
            if ((ret = ptls_send(tls, &wbuf, rbuf, rret)) != 0) {
                fprintf(stderr, "ptls_send:%d\n", ret);
                goto Exit;
            }
            if (write_all(fd, wbuf.base, wbuf.off) != 0)
                goto Exit;
            wbuf.off = 0;
        }

        if (FD_ISSET(fd, &readfds)) {
            /* read from socket, decrypt and print */
            while ((rret = read(fd, rbuf, sizeof(rbuf))) == 1 && errno == EINTR)
                ;
            if (rret <= 0)
                goto Exit;
            if (decrypt_and_print(tls, rbuf, rret) != 0)
                goto Exit;
        }
    }

Exit:
    ptls_buffer_dispose(&wbuf);
    ptls_free(tls);
    return 0;
}

static int run_server(struct sockaddr *sa, socklen_t salen, ptls_context_t *ctx)
{
    int listen_fd, conn_fd;
    sockopt_t on = 1;

    if ((listen_fd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
        perror("socket(2) failed");
        return 1;
    }
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) != 0) {
        perror("setsockopt(SO_REUSEADDR) failed");
        return 1;
    }
    if (bind(listen_fd, sa, salen) != 0) {
        perror("bind(2) failed");
        return 1;
    }
    if (listen(listen_fd, SOMAXCONN) != 0) {
        perror("listen(2) failed");
        return 1;
    }

    while (1) {
        if ((conn_fd = accept(listen_fd, NULL, 0)) != -1) {
            handle_connection(conn_fd, ctx, NULL);
            close(conn_fd);
        }
    }

    return 0;
}

static int run_client(struct sockaddr *sa, socklen_t salen, ptls_context_t *ctx)
{
    int fd;

    if ((fd = socket(AF_INET, SOCK_STREAM, 0)) == 1) {
        perror("socket(2) failed");
        return 1;
    }
    if (connect(fd, sa, salen) != 0) {
        perror("connect(2) failed");
        return 1;
    }

    return handle_connection(fd, ctx, "example.com");
}

static int resolve_address(struct sockaddr *sa, socklen_t *salen, const char *host, const char *port)
{
    struct addrinfo hints, *res;
    int err;

    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = AI_ADDRCONFIG | AI_NUMERICSERV | AI_PASSIVE;
    if ((err = getaddrinfo(host, port, &hints, &res)) != 0 || res == NULL) {
        fprintf(stderr, "failed to resolve address:%s:%s:%s\n", host, port,
                err != 0 ? gai_strerror(err) : "getaddrinfo returned NULL");
        return -1;
    }

    memcpy(sa, res->ai_addr, res->ai_addrlen);
    *salen = res->ai_addrlen;

    freeaddrinfo(res);
    return 0;
}

static void usage(const char *cmd)
{
}

int main(int argc, char **argv)
{
#ifdef _WIN32
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 0), &wsaData);
#endif

    ERR_load_crypto_strings();
    OpenSSL_add_all_algorithms();
#if !defined(OPENSSL_NO_ENGINE)
    /* Load all compiled-in ENGINEs */
    ENGINE_load_builtin_engines();
    ENGINE_register_all_ciphers();
    ENGINE_register_all_digests();
#endif

    ptls_openssl_context_t *ctx = ptls_openssl_context_new();
    const char *host, *port;
    STACK_OF(X509) *certs = NULL;
    EVP_PKEY *pkey = NULL;
    int ch;
    struct sockaddr_storage sa;
    socklen_t salen;

    while ((ch = getopt(argc, argv, "c:k:")) != -1) {
        switch (ch) {
        case 'c': {
            FILE *fp;
            X509 *cert;
            if ((fp = fopen(optarg, "rb")) == NULL) {
                fprintf(stderr, "failed to open file:%s:%s\n", optarg, strerror(errno));
                return 1;
            }
            certs = sk_X509_new(NULL);
            while ((cert = PEM_read_X509(fp, NULL, NULL, NULL)) != NULL)
                sk_X509_push(certs, cert);
            fclose(fp);
            if (sk_X509_num(certs) == 0) {
                fprintf(stderr, "failed to load certificate chain from file:%s\n", optarg);
                return 1;
            }
        } break;
        case 'k': {
            FILE *fp;
            if ((fp = fopen(optarg, "rb")) == NULL) {
                fprintf(stderr, "failed to open file:%s:%s\n", optarg, strerror(errno));
                return 1;
            }
            pkey = PEM_read_PrivateKey(fp, NULL, NULL, NULL);
            fclose(fp);
            if (pkey == NULL) {
                fprintf(stderr, "failed to load private key from file:%s\n", optarg);
                return 1;
            }
        } break;
        default:
            usage(argv[0]);
            exit(1);
        }
    }
    argc -= optind;
    argv += optind;
    if (certs != NULL || pkey != NULL) {
        if (certs == NULL || pkey == NULL) {
            fprintf(stderr, "-c and -k options must be used together\n");
            return 1;
        }
        ptls_openssl_context_register_server(ctx, "example.com", pkey, certs);
        sk_X509_free(certs);
        EVP_PKEY_free(pkey);
    }
    if (argc != 0) {
        host = (--argc, *argv++);
    } else {
        host = certs != NULL ? "0.0.0.0" : "127.0.0.1";
    }
    if (argc != 0) {
        port = (--argc, *argv++);
    } else {
        port = "8443";
    }

    if (resolve_address((struct sockaddr *)&sa, &salen, host, port) != 0)
        exit(1);

    return (certs != NULL ? run_server : run_client)((struct sockaddr *)&sa, salen, ptls_openssl_context_get_context(ctx));
}
