// SPDX-License-Identifier: Apache-2.0
#include "https_get.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "bflb_mtimer.h"
#include "bl_crt_bundle.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "http/parser.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ssl.h"
#include "mm.h"

enum { CONNECTION_TIMEOUT_MS = 10000, HTTPS_READ_BYTES = 4 * 1024 };

static SemaphoreHandle_t bundle_lock;
static StaticSemaphore_t bundle_lock_storage;

typedef struct {
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config config;
    mbedtls_ctr_drbg_context rng;
    mbedtls_entropy_context entropy;
    mpx_https_response_t* response;
    uint64_t deadline;
    uint32_t receive_timeout_ms;
    int socket;
    bool bundle_attached;
    bool complete;
    bool header_value;
    bool headers_complete;
    char host[254];
    uint8_t read_buffer[HTTPS_READ_BYTES];
} transfer_t;

bool mpx_https_initialize(void) {
    bundle_lock = xSemaphoreCreateMutexStatic(&bundle_lock_storage);
    return bundle_lock != NULL;
}

static bool field_name_char(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-';
}

static bool valid_headers(const char* start, size_t length) {
    static const char* const reserved[] = {"Host", "Connection", "Content-Length", "Transfer-Encoding",
                                           "Accept-Encoding"};
    unsigned fields = 0;
    while (length) {
        const char* end = memchr(start, '\r', length);
        if (!end || (size_t)(end - start) + 1 >= length || end[1] != '\n' || ++fields > 8) return false;
        const char* colon = memchr(start, ':', end - start);
        if (!colon || colon == start) return false;
        const size_t name_length = colon - start;
        for (const char* p = start; p < colon; ++p)
            if (!field_name_char(*p)) return false;
        for (unsigned i = 0; i < sizeof(reserved) / sizeof(*reserved); ++i)
            if (name_length == strlen(reserved[i]) && !strncasecmp(start, reserved[i], name_length)) return false;
        for (const unsigned char* p = (const unsigned char*)colon + 1; p < (const unsigned char*)end; ++p)
            if ((*p < 32 && *p != '\t') || *p == 127) return false;
        length -= end + 2 - start;
        start = end + 2;
    }
    return true;
}

static bool parse_url(const char* url, struct http_parser_url* parsed) {
    http_parser_url_init(parsed);
    if (strncmp(url, "https://", 8) != 0) return false;
    for (const unsigned char* p = (const unsigned char*)url; *p; ++p)
        if (*p <= 32 || *p >= 127) return false;
    return !http_parser_parse_url(url, strlen(url), 0, parsed) && (parsed->field_set & (1U << UF_HOST)) &&
           !(parsed->field_set & (1U << UF_USERINFO)) &&
           parsed->field_data[UF_HOST].len < sizeof(((transfer_t*)0)->host) &&
           (!(parsed->field_set & (1U << UF_PORT)) || parsed->port);
}

bool mpx_https_valid_request(const mpx_https_request_t* request) {
    struct http_parser_url parsed;
    return parse_url(request->url, &parsed) && valid_headers(request->headers, request->headers_length);
}

static bool cancelled(const transfer_t* transfer) {
    const mpx_https_response_t* response = transfer->response;
    return response->overflow || response->cancelled(response->context);
}

static int await_socket(const transfer_t* transfer, short events) {
    struct pollfd ready = {.fd = transfer->socket, .events = events};
    while (!cancelled(transfer)) {
        const uint64_t now = bflb_mtimer_get_time_ms();
        if (now >= transfer->deadline) return -ETIMEDOUT;
        uint64_t remaining = transfer->deadline - now;
        const int result = lwip_poll(&ready, 1, remaining > 150 ? 150 : remaining);
        if (result > 0) {
            if (ready.revents & events) return 0;
            if (ready.revents & POLLHUP) return 0;  // Let TLS read EOF.
            return -EIO;
        }
        if (result < 0 && errno != EINTR) return -errno;
    }
    return -ECANCELED;
}

static int connect_socket(transfer_t* transfer, const char* port) {
    const struct addrinfo hints = {.ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM};
    struct addrinfo* addresses = NULL;
    int lookup = lwip_getaddrinfo(transfer->host, port, &hints, &addresses);
    if (lookup != 0 && !cancelled(transfer) && bflb_mtimer_get_time_ms() + 200 < transfer->deadline) {
        vTaskDelay(pdMS_TO_TICKS(200));
        lookup = lwip_getaddrinfo(transfer->host, port, &hints, &addresses);
    }
    if (lookup != 0) {
        if (!cancelled(transfer)) printf("MicroPixel HTTPS DNS error %d for %s\n", lookup, transfer->host);
        return -EHOSTUNREACH;
    }
    int error = -ECONNREFUSED;
    for (const struct addrinfo* address = addresses; address && !cancelled(transfer); address = address->ai_next) {
        transfer->socket = lwip_socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (transfer->socket < 0) continue;
        int nonblocking = 1;
        if (lwip_ioctl(transfer->socket, FIONBIO, &nonblocking) != 0) {
            error = -errno;
        } else if (lwip_connect(transfer->socket, address->ai_addr, address->ai_addrlen) == 0) {
            error = 0;
            break;
        } else if (errno == EINPROGRESS || errno == EWOULDBLOCK) {
            error = await_socket(transfer, POLLOUT);
            if (!error) {
                int socket_error = 0;
                socklen_t length = sizeof(socket_error);
                if (lwip_getsockopt(transfer->socket, SOL_SOCKET, SO_ERROR, &socket_error, &length) != 0)
                    error = -errno;
                else
                    error = -socket_error;
            }
            if (!error) break;
        } else {
            error = -errno;
        }
        lwip_close(transfer->socket);
        transfer->socket = -1;
    }
    lwip_freeaddrinfo(addresses);
    return error;
}

static int send_bytes(void* context, const unsigned char* bytes, size_t length) {
    const int fd = ((transfer_t*)context)->socket;
    const int result = lwip_send(fd, bytes, length, 0);
    if (result >= 0) return result;
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ? MBEDTLS_ERR_SSL_WANT_WRITE
                                                                     : MBEDTLS_ERR_SSL_INTERNAL_ERROR;
}

static int receive_bytes(void* context, unsigned char* bytes, size_t length) {
    const int fd = ((transfer_t*)context)->socket;
    const int result = lwip_recv(fd, bytes, length, 0);
    if (result >= 0) return result;
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ? MBEDTLS_ERR_SSL_WANT_READ
                                                                     : MBEDTLS_ERR_SSL_INTERNAL_ERROR;
}

static int await_tls(const transfer_t* transfer, int result) {
    if (result == MBEDTLS_ERR_SSL_WANT_READ) return await_socket(transfer, POLLIN);
    if (result == MBEDTLS_ERR_SSL_WANT_WRITE) return await_socket(transfer, POLLOUT);
    return result;
}

static int connect_tls(transfer_t* transfer) {
    int result = mbedtls_ctr_drbg_seed(&transfer->rng, mbedtls_entropy_func, &transfer->entropy, NULL, 0);
    if (result) return result;
    result = mbedtls_ssl_config_defaults(&transfer->config, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                         MBEDTLS_SSL_PRESET_DEFAULT);
    if (result) return result;
    xSemaphoreTake(bundle_lock, portMAX_DELAY);
    result = bl_crt_bundle_attach(&transfer->config);
    xSemaphoreGive(bundle_lock);
    if (result) return result;
    transfer->bundle_attached = true;
    mbedtls_ssl_conf_authmode(&transfer->config, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_rng(&transfer->config, mbedtls_ctr_drbg_random, &transfer->rng);
    result = mbedtls_ssl_setup(&transfer->ssl, &transfer->config);
    if (result) return result;
    result = mbedtls_ssl_set_hostname(&transfer->ssl, transfer->host);
    if (result) return result;
    mbedtls_ssl_set_bio(&transfer->ssl, transfer, send_bytes, receive_bytes, NULL);
    while ((result = mbedtls_ssl_handshake(&transfer->ssl)) != 0) {
        const int ready = await_tls(transfer, result);
        if (ready) return ready;
    }
    return mbedtls_ssl_get_verify_result(&transfer->ssl) ? -EACCES : 0;
}

static int write_tls(transfer_t* transfer, const char* bytes, size_t length) {
    while (length && !cancelled(transfer)) {
        if (bflb_mtimer_get_time_ms() >= transfer->deadline) return -ETIMEDOUT;
        const int result = mbedtls_ssl_write(&transfer->ssl, (const uint8_t*)bytes, length);
        if (result > 0) {
            bytes += result;
            length -= result;
        } else {
            if (!result) return -EPIPE;
            const int ready = await_tls(transfer, result);
            if (ready) return ready;
        }
    }
    return length ? -ECANCELED : 0;
}

static int body(struct http_parser* parser, const char* bytes, size_t length) {
    transfer_t* transfer = parser->data;
    mpx_https_response_t* response = transfer->response;
    if (length > response->capacity - response->length) {
        response->overflow = true;
        return 1;
    }
    if (response->on_body) {
        if (!response->on_body(response->context, (const uint8_t*)bytes, length)) return 1;
    } else {
        memcpy(response->body + response->length, bytes, length);
    }
    response->length += length;
    return 0;
}

static int append_header(transfer_t* transfer, const char* bytes, size_t length) {
    mpx_https_response_t* response = transfer->response;
    if (!response->headers) return 0;
    if (length > response->headers_capacity - response->headers_length) {
        response->overflow = true;
        return 1;
    }
    if (length) memcpy(response->headers + response->headers_length, bytes, length);
    response->headers_length += length;
    return 0;
}

static int header_name(struct http_parser* parser, const char* bytes, size_t length) {
    transfer_t* transfer = parser->data;
    if (transfer->headers_complete) return 0;
    if (transfer->header_value && append_header(transfer, "\r\n", 2)) return 1;
    transfer->header_value = false;
    return append_header(transfer, bytes, length);
}

static int header_value(struct http_parser* parser, const char* bytes, size_t length) {
    transfer_t* transfer = parser->data;
    if (transfer->headers_complete) return 0;
    if (!transfer->header_value && append_header(transfer, ": ", 2)) return 1;
    transfer->header_value = true;
    return append_header(transfer, bytes, length);
}

static int end_headers(struct http_parser* parser) {
    transfer_t* transfer = parser->data;
    transfer->headers_complete = true;
    // Return values 1 and 2 have special meanings in http-parser.
    if (transfer->header_value && append_header(transfer, "\r\n", 2)) return -1;
    mpx_https_response_t* response = transfer->response;
    if (parser->status_code >= 200 && response->on_headers &&
        !response->on_headers(response->context, parser->status_code, parser->content_length))
        return -1;
    return 0;
}

static int complete(struct http_parser* parser) {
    transfer_t* transfer = parser->data;
    if (parser->status_code >= 100 && parser->status_code < 200 && parser->status_code != 101) {
        transfer->response->headers_length = 0;
        transfer->header_value = false;
        transfer->headers_complete = false;
        return 0;
    }
    transfer->response->status = parser->status_code;
    transfer->complete = true;
    http_parser_pause(parser, 1);
    return 0;
}

static int receive_http(transfer_t* transfer) {
    struct http_parser parser;
    http_parser_init(&parser, HTTP_RESPONSE);
    parser.data = transfer;
    static const struct http_parser_settings settings = {.on_header_field = header_name,
                                                         .on_header_value = header_value,
                                                         .on_headers_complete = end_headers,
                                                         .on_body = body,
                                                         .on_message_complete = complete};
    transfer->deadline = bflb_mtimer_get_time_ms() + transfer->receive_timeout_ms;
    while (!transfer->complete && !cancelled(transfer)) {
        if (bflb_mtimer_get_time_ms() >= transfer->deadline) return -ETIMEDOUT;
        int count = mbedtls_ssl_read(&transfer->ssl, transfer->read_buffer, sizeof(transfer->read_buffer));
        if (count < 0 && count != MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
            const int ready = await_tls(transfer, count);
            if (ready) return ready;
            continue;
        }
        if (count == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) count = 0;
        const size_t parsed = http_parser_execute(&parser, &settings, (const char*)transfer->read_buffer, count);
        if (transfer->complete && HTTP_PARSER_ERRNO(&parser) == HPE_PAUSED) return 0;
        if (parsed != (size_t)count || HTTP_PARSER_ERRNO(&parser) != HPE_OK) return -EBADMSG;
        if (!count) break;
        // A streaming sink may wait for PCM capacity or pause. Socket idle
        // time starts when the consumer is ready to receive again.
        transfer->deadline = bflb_mtimer_get_time_ms() + transfer->receive_timeout_ms;
    }
    return transfer->complete ? 0 : cancelled(transfer) ? -ECANCELED : -ECONNRESET;
}

static int send_request(transfer_t* transfer, const mpx_https_request_t* request, const char* authority,
                        const char* target, size_t target_length) {
    const char* method = request->post ? "POST " : "GET ";
    const char* path = *target == '/' ? "" : "/";
    static const char fixed_headers[] = "\r\nConnection: close\r\nAccept-Encoding: identity\r\n";
    char content_length[32];
    const size_t length = request->post ? snprintf(content_length, sizeof(content_length), "Content-Length: %zu\r\n",
                                                   request->body_length)
                                        : 0;
    int result;
    if ((result = write_tls(transfer, method, strlen(method))) || (result = write_tls(transfer, path, strlen(path))) ||
        (result = write_tls(transfer, target, target_length)) ||
        (result = write_tls(transfer, " HTTP/1.1\r\nHost: ", sizeof(" HTTP/1.1\r\nHost: ") - 1)) ||
        (result = write_tls(transfer, authority, target - authority)) ||
        (result = write_tls(transfer, fixed_headers, sizeof(fixed_headers) - 1)) ||
        (result = write_tls(transfer, request->headers, request->headers_length)) ||
        (result = write_tls(transfer, content_length, length)) || (result = write_tls(transfer, "\r\n", 2)))
        return result;
    return write_tls(transfer, (const char*)request->body, request->body_length);
}

int mpx_https_exchange(const mpx_https_request_t* request, mpx_https_response_t* response) {
    const char* url = request->url;
    struct http_parser_url parsed;
    if (!parse_url(url, &parsed)) return -EINVAL;
    const size_t host_length = parsed.field_data[UF_HOST].len;
    const char* authority = url + 8;
    const size_t authority_length = strcspn(authority, "/?#");
    const char* target = authority + authority_length;
    const size_t target_length = strcspn(target, "#");
    char port[6];
    snprintf(port, sizeof(port), "%u", (parsed.field_set & (1U << UF_PORT)) ? parsed.port : 443);

    transfer_t* transfer = kmalloc(sizeof(*transfer), MM_FLAG_HEAP_PSRAM_0 | MM_FLAG_ALIGN_8BYTE);
    if (!transfer) return -ENOMEM;
    memset(transfer, 0, sizeof(*transfer));
    memcpy(transfer->host, url + parsed.field_data[UF_HOST].off, host_length);
    transfer->response = response;
    transfer->deadline = bflb_mtimer_get_time_ms() + CONNECTION_TIMEOUT_MS;
    transfer->receive_timeout_ms = request->receive_timeout_ms;
    transfer->socket = -1;
    mbedtls_ssl_init(&transfer->ssl);
    mbedtls_ssl_config_init(&transfer->config);
    mbedtls_ctr_drbg_init(&transfer->rng);
    mbedtls_entropy_init(&transfer->entropy);

    const char* stage = "TCP";
    int result = connect_socket(transfer, port);
    if (!result) {
        stage = "TLS";
        result = connect_tls(transfer);
    }
    if (!result) {
        stage = "request";
        result = send_request(transfer, request, authority, target, target_length);
    }
    if (!result) {
        stage = "response";
        result = receive_http(transfer);
    }
    if (result < 0 && !cancelled(transfer)) printf("MicroPixel HTTPS %s error %d\n", stage, result);

    mbedtls_ssl_free(&transfer->ssl);
    if (transfer->bundle_attached) {
        xSemaphoreTake(bundle_lock, portMAX_DELAY);
        bl_crt_bundle_detach(&transfer->config);
        xSemaphoreGive(bundle_lock);
    }
    mbedtls_ssl_config_free(&transfer->config);
    mbedtls_ctr_drbg_free(&transfer->rng);
    mbedtls_entropy_free(&transfer->entropy);
    if (transfer->socket >= 0) lwip_close(transfer->socket);
    kfree(transfer);
    return result;
}
