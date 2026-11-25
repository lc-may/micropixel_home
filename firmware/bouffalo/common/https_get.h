// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char* url;
    const char* headers;
    size_t headers_length;
    const uint8_t* body;
    size_t body_length;
    uint32_t receive_timeout_ms;
    bool post;
} mpx_https_request_t;

typedef struct {
    void* context;
    bool (*cancelled)(void*);
    uint8_t* body;
    uint32_t capacity;
    uint32_t length;
    char* headers;
    uint32_t headers_capacity;
    uint32_t headers_length;
    uint16_t status;
    bool overflow;
    // Optional streaming sink. Body bytes are valid only during the callback.
    // The same capacity/length fields limit and count the total body bytes.
    // on_headers receives UINT64_MAX when no Content-Length was supplied.
    bool (*on_headers)(void*, uint16_t, uint64_t);
    bool (*on_body)(void*, const uint8_t*, size_t);
} mpx_https_response_t;

bool mpx_https_initialize(void);
bool mpx_https_valid_request(const mpx_https_request_t* request);
int mpx_https_exchange(const mpx_https_request_t* request, mpx_https_response_t* response);

#ifdef __cplusplus
}
#endif
