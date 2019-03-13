/*
 * Copyright (c) 2014-2019 DeNA Co., Ltd., Kazuho Oku, Fastly, Frederik
 *                         Deweerdt, Justin Zhu, Ichito Nagata, Grant Zhang,
 *                         Baodong Chen
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
#include <errno.h>
#include <getopt.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "picotls.h"
#include "picotls/openssl.h"
#include "quicly.h"
#include "h2o.h"

#ifndef MIN
#define MIN(a, b) (((a) > (b)) ? (b) : (a))
#endif

#define IO_TIMEOUT 5000

static h2o_httpclient_connection_pool_t *connpool;
static h2o_mem_pool_t pool;
static const char *url;
static char *method = "GET";
static int cnt_left = 1;
static int body_size = 0;
static int chunk_size = 10;
static h2o_iovec_t iov_filler;
static int delay_interval_ms = 0;
static int ssl_verify_none = 0;
static int cur_body_size;
static struct {
    ptls_context_t tls;
    quicly_context_t quic;
    h2o_http3_ctx_t h3;
} h3ctx = {{ptls_openssl_random_bytes,
            &ptls_get_time,
            ptls_openssl_key_exchanges,
            ptls_openssl_cipher_suites,
            {NULL},
            NULL,
            NULL,
            NULL,
            NULL,
            NULL,
            0,
            0,
            NULL,
            1}};

static h2o_httpclient_head_cb on_connect(h2o_httpclient_t *client, const char *errstr, h2o_iovec_t *method, h2o_url_t *url,
                                         const h2o_header_t **headers, size_t *num_headers, h2o_iovec_t *body,
                                         h2o_httpclient_proceed_req_cb *proceed_req_cb, h2o_httpclient_properties_t *props,
                                         h2o_url_t *origin);
static h2o_httpclient_body_cb on_head(h2o_httpclient_t *client, const char *errstr, int version, int status, h2o_iovec_t msg,
                                      h2o_header_t *headers, size_t num_headers, int header_requires_dup);

static void on_exit_deferred(h2o_timer_t *entry)
{
    h2o_timer_unlink(entry);
    exit(1);
}
static h2o_timer_t exit_deferred;

static void on_error(h2o_httpclient_ctx_t *ctx, const char *fmt, ...)
{
    char errbuf[2048];
    va_list args;
    va_start(args, fmt);
    int errlen = vsnprintf(errbuf, sizeof(errbuf), fmt, args);
    va_end(args);
    fprintf(stderr, "%.*s\n", errlen, errbuf);

    /* defer using zero timeout to send pending GOAWAY frame */
    memset(&exit_deferred, 0, sizeof(exit_deferred));
    exit_deferred.cb = on_exit_deferred;
    h2o_timer_link(ctx->loop, 0, &exit_deferred);
}

static void start_request(h2o_httpclient_ctx_t *ctx)
{
    h2o_url_t *url_parsed;

    /* clear memory pool */
    h2o_mem_clear_pool(&pool);

    /* parse URL */
    url_parsed = h2o_mem_alloc_pool(&pool, *url_parsed, 1);
    if (h2o_url_parse(url, SIZE_MAX, url_parsed) != 0) {
        on_error(ctx, "unrecognized type of URL: %s", url);
        return;
    }

    cur_body_size = body_size;

    /* initiate the request */
    if (ctx->http3 != NULL) {
        h2o_httpclient_connect_h3(NULL, &pool, url_parsed, ctx, url_parsed, on_connect);
    } else {
        if (connpool == NULL) {
            connpool = h2o_mem_alloc(sizeof(*connpool));
            h2o_socketpool_t *sockpool = h2o_mem_alloc(sizeof(*sockpool));
            h2o_socketpool_target_t *target = h2o_socketpool_create_target(url_parsed, NULL);
            h2o_socketpool_init_specific(sockpool, 10, &target, 1, NULL);
            h2o_socketpool_set_timeout(sockpool, IO_TIMEOUT);
            h2o_socketpool_register_loop(sockpool, ctx->loop);
            h2o_httpclient_connection_pool_init(connpool, sockpool);

            /* obtain root */
            char *root, *crt_fullpath;
            if ((root = getenv("H2O_ROOT")) == NULL)
                root = H2O_TO_STR(H2O_ROOT);
#define CA_PATH "/share/h2o/ca-bundle.crt"
            crt_fullpath = h2o_mem_alloc(strlen(root) + strlen(CA_PATH) + 1);
            sprintf(crt_fullpath, "%s%s", root, CA_PATH);
#undef CA_PATH

            SSL_CTX *ssl_ctx = SSL_CTX_new(TLSv1_client_method());
            SSL_CTX_load_verify_locations(ssl_ctx, crt_fullpath, NULL);
            if (ssl_verify_none) {
                SSL_CTX_set_verify(ssl_ctx, SSL_VERIFY_NONE, NULL);
            } else {
                SSL_CTX_set_verify(ssl_ctx, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, NULL);
            }
            h2o_socketpool_set_ssl_ctx(sockpool, ssl_ctx);
            SSL_CTX_free(ssl_ctx);
        }
        h2o_httpclient_connect(NULL, &pool, url_parsed, ctx, connpool, url_parsed, on_connect);
    }
}

static int on_body(h2o_httpclient_t *client, const char *errstr)
{
    if (errstr != NULL && errstr != h2o_httpclient_error_is_eos) {
        on_error(client->ctx, errstr);
        return -1;
    }

    fwrite((*client->buf)->bytes, 1, (*client->buf)->size, stdout);
    fflush(stdout);
    h2o_buffer_consume(&(*client->buf), (*client->buf)->size);

    if (errstr == h2o_httpclient_error_is_eos) {
        if (--cnt_left != 0) {
            /* next attempt */
            h2o_mem_clear_pool(&pool);
            start_request(client->ctx);
        }
    }

    return 0;
}

static void print_status_line(int version, int status, h2o_iovec_t msg)
{
    fprintf(stderr, "HTTP/%d", (version >> 8));
    if ((version & 0xff) != 0) {
        fprintf(stderr, ".%d", version & 0xff);
    }
    fprintf(stderr, " %d", status);
    if (msg.len == 0) {
        fprintf(stderr, " %.*s\n", (int)msg.len, msg.base);
    } else {
        fprintf(stderr, "\n");
    }
}

h2o_httpclient_body_cb on_head(h2o_httpclient_t *client, const char *errstr, int version, int status, h2o_iovec_t msg,
                               h2o_header_t *headers, size_t num_headers, int header_requires_dup)
{
    size_t i;

    if (errstr != NULL && errstr != h2o_httpclient_error_is_eos) {
        on_error(client->ctx, errstr);
        return NULL;
    }

    print_status_line(version, status, msg);

    for (i = 0; i != num_headers; ++i) {
        const char *name = headers[i].orig_name;
        if (name == NULL)
            name = headers[i].name->base;
        fprintf(stderr, "%.*s: %.*s\n", (int)headers[i].name->len, name, (int)headers[i].value.len, headers[i].value.base);
    }
    fprintf(stderr, "\n");
    fflush(stderr);

    if (errstr == h2o_httpclient_error_is_eos) {
        on_error(client->ctx, "no body");
        return NULL;
    }

    return on_body;
}

int fill_body(h2o_iovec_t *reqbuf)
{
    if (cur_body_size > 0) {
        memcpy(reqbuf, &iov_filler, sizeof(*reqbuf));
        reqbuf->len = MIN(iov_filler.len, cur_body_size);
        cur_body_size -= reqbuf->len;
        return 0;
    } else {
        *reqbuf = h2o_iovec_init(NULL, 0);
        return 1;
    }
}

struct st_timeout_ctx {
    h2o_httpclient_t *client;
    h2o_timer_t _timeout;
};
static void timeout_cb(h2o_timer_t *entry)
{
    static h2o_iovec_t reqbuf;
    struct st_timeout_ctx *tctx = H2O_STRUCT_FROM_MEMBER(struct st_timeout_ctx, _timeout, entry);

    fill_body(&reqbuf);
    h2o_timer_unlink(&tctx->_timeout);
    tctx->client->write_req(tctx->client, reqbuf, cur_body_size <= 0);
    free(tctx);

    return;
}

static void proceed_request(h2o_httpclient_t *client, size_t written, int is_end_stream)
{
    if (cur_body_size > 0) {
        struct st_timeout_ctx *tctx;
        tctx = h2o_mem_alloc(sizeof(*tctx));
        memset(tctx, 0, sizeof(*tctx));
        tctx->client = client;
        tctx->_timeout.cb = timeout_cb;
        h2o_timer_link(client->ctx->loop, delay_interval_ms, &tctx->_timeout);
    }
}

h2o_httpclient_head_cb on_connect(h2o_httpclient_t *client, const char *errstr, h2o_iovec_t *_method, h2o_url_t *url,
                                  const h2o_header_t **headers, size_t *num_headers, h2o_iovec_t *body,
                                  h2o_httpclient_proceed_req_cb *proceed_req_cb, h2o_httpclient_properties_t *props,
                                  h2o_url_t *origin)
{
    if (errstr != NULL) {
        on_error(client->ctx, errstr);
        return NULL;
    }

    *_method = h2o_iovec_init(method, strlen(method));
    *url = *((h2o_url_t *)client->data);
    *headers = NULL;
    *num_headers = 0;
    *body = h2o_iovec_init(NULL, 0);
    *proceed_req_cb = NULL;

    if (cur_body_size > 0) {
        char *clbuf = h2o_mem_alloc_pool(&pool, char, sizeof(H2O_UINT32_LONGEST_STR) - 1);
        size_t clbuf_len = sprintf(clbuf, "%d", cur_body_size);
        h2o_headers_t headers_vec = (h2o_headers_t){NULL};
        h2o_add_header(&pool, &headers_vec, H2O_TOKEN_CONTENT_LENGTH, NULL, clbuf, clbuf_len);
        *headers = headers_vec.entries;
        *num_headers = 1;

        *proceed_req_cb = proceed_request;

        struct st_timeout_ctx *tctx;
        tctx = h2o_mem_alloc(sizeof(*tctx));
        memset(tctx, 0, sizeof(*tctx));
        tctx->client = client;
        tctx->_timeout.cb = timeout_cb;
        h2o_timer_link(client->ctx->loop, delay_interval_ms, &tctx->_timeout);
    }

    return on_head;
}

static void usage(const char *progname)
{
    fprintf(stderr,
            "Usage: %s [options] <url>\n"
            "Options:\n"
            "  -2 <ratio>   HTTP/2 ratio (between 0 and 100)\n"
            "  -3           HTTP/3-only mode\n"
            "  -E <path>    QUIC event log file (default: none)\n"
            "  -b <size>    size of request body (in bytes; default: 0)\n"
            "  -c <size>    size of body chunk (in bytes; default: 10)\n"
            "  -i <delay>   send interval between chunks (in msec; default: 0)\n"
            "  -k           skip peer verification\n"
            "  -m <method>  request method (default: GET)\n"
            "  -t <times>   number of requests to send the request (default: 1)\n"
            "\n",
            progname);
}

#if H2O_USE_LIBUV
#else
static h2o_socket_t *create_quic_socket(h2o_loop_t *loop)
{
    int fd;
    struct sockaddr_in sin;

    if ((fd = socket(PF_INET, SOCK_DGRAM, 0)) == -1) {
        perror("failed to create UDP socket");
        exit(EXIT_FAILURE);
    }
    memset(&sin, 0, sizeof(sin));
    if (bind(fd, (void *)&sin, sizeof(sin)) != 0) {
        perror("failed to bind bind UDP socket");
        exit(EXIT_FAILURE);
    }

    return h2o_evloop_socket_create(loop, fd, H2O_SOCKET_FLAG_DONT_READ);
}
#endif

int main(int argc, char **argv)
{
    h2o_multithread_queue_t *queue;
    h2o_multithread_receiver_t getaddr_receiver;
    h2o_httpclient_ctx_t ctx = {NULL, &getaddr_receiver, IO_TIMEOUT, IO_TIMEOUT, IO_TIMEOUT, NULL, IO_TIMEOUT, SIZE_MAX};
    int opt;

    SSL_load_error_strings();
    SSL_library_init();
    OpenSSL_add_all_algorithms();

    quicly_amend_ptls_context(&h3ctx.tls);
    h3ctx.quic = quicly_default_context;
    h3ctx.quic.transport_params.max_streams_uni = 10;
    h3ctx.quic.tls = &h3ctx.tls;
    {
        uint8_t random_key[PTLS_SHA256_DIGEST_SIZE];
        h3ctx.tls.random_bytes(random_key, sizeof(random_key));
        h3ctx.quic.cid_encryptor = quicly_new_default_cid_encryptor(&ptls_openssl_bfecb, &ptls_openssl_sha256,
                                                                    ptls_iovec_init(random_key, sizeof(random_key)));
        ptls_clear_memory(random_key, sizeof(random_key));
    }
    h3ctx.quic.stream_open = &h2o_httpclient_http3_on_stream_open;

#if H2O_USE_LIBUV
    ctx.loop = uv_loop_new();
#else
    ctx.loop = h2o_evloop_create();
#endif

    while ((opt = getopt(argc, argv, "t:m:b:c:i:k2:3E:")) != -1) {
        switch (opt) {
        case 't':
            cnt_left = atoi(optarg);
            break;
        case 'm':
            method = optarg;
            break;
        case 'b':
            body_size = atoi(optarg);
            if (body_size <= 0) {
                fprintf(stderr, "body size must be greater than 0\n");
                exit(EXIT_FAILURE);
            }
            break;
        case 'c':
            chunk_size = atoi(optarg);
            if (chunk_size <= 0) {
                fprintf(stderr, "chunk size must be greater than 0\n");
                exit(EXIT_FAILURE);
            }
            break;
        case 'i':
            delay_interval_ms = atoi(optarg);
            break;
        case 'k':
            ssl_verify_none = 1;
            break;
        case '2':
            if (sscanf(optarg, "%" SCNd8, &ctx.http2.ratio) != 1 || !(0 <= ctx.http2.ratio && ctx.http2.ratio <= 100)) {
                fprintf(stderr, "failed to parse HTTP/2 ratio (-2)\n");
                exit(EXIT_FAILURE);
            }
            break;
        case '3':
#if H2O_USE_LIBUV
            fprintf(stderr, "HTTP/3 is currently not supported by the libuv backend.\n");
            exit(EXIT_FAILURE);
#else
            h2o_http3_init_context(&h3ctx.h3, ctx.loop, create_quic_socket(ctx.loop), &h3ctx.quic, NULL,
                                   h2o_httpclient_http3_notify_connection_update);
            ctx.http3 = &h3ctx.h3;
#endif
            break;
        case 'E': {
            FILE *fp;
            if ((fp = fopen(optarg, "w")) == NULL) {
                fprintf(stderr, "failed to open file:%s:%s\n", optarg, strerror(errno));
                exit(EXIT_FAILURE);
            }
            setvbuf(fp, NULL, _IONBF, 0);
            h3ctx.quic.event_log.cb = quicly_new_default_event_logger(fp);
            h3ctx.quic.event_log.mask = UINT64_MAX;
        } break;
        default:
            usage(argv[0]);
            exit(EXIT_FAILURE);
            break;
        }
    }
    argc -= optind;
    argv += optind;

    if (argc < 1) {
        fprintf(stderr, "no URL\n");
        exit(EXIT_FAILURE);
    }
    url = argv[0];

    if (body_size != 0) {
        iov_filler.base = h2o_mem_alloc(chunk_size);
        memset(iov_filler.base, 'a', chunk_size);
        iov_filler.len = chunk_size;
    }
    h2o_mem_init_pool(&pool);

    /* setup context */
    queue = h2o_multithread_create_queue(ctx.loop);
    h2o_multithread_register_receiver(queue, ctx.getaddr_receiver, h2o_hostinfo_getaddr_receiver);

    /* setup the first request */
    start_request(&ctx);

    while (cnt_left != 0) {
#if H2O_USE_LIBUV
        uv_run(ctx.loop, UV_RUN_ONCE);
#else
        h2o_evloop_run(ctx.loop, INT32_MAX);
#endif
    }

    if (ctx.http3 != NULL) {
        h2o_http3_close_all_connections(ctx.http3);
        while (h2o_http3_num_connections(ctx.http3) != 0) {
#if H2O_USE_LIBUV
            uv_run(ctx.loop, UV_RUN_ONCE);
#else
            h2o_evloop_run(ctx.loop, INT32_MAX);
#endif
        }
    }

    return 0;
}
