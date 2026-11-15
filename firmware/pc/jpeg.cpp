// SPDX-License-Identifier: Apache-2.0
// libjpeg requires size_t and FILE to be declared before its public header.
// clang-format off
#include <cstddef>
#include <cstdio>
#include <jpeglib.h>
// clang-format on

#include <csetjmp>
#include <cstdio>
#include <new>
#include <vector>

#include "esp_jpeg_dec.h"
struct PcJpeg {
    jpeg_decompress_struct decoder{};
    jpeg_error_mgr errors{};
    std::jmp_buf recovery{};
    bool rgb565{};
    std::vector<uint8_t> row;
};
static void OnJpegError(j_common_ptr decoder) {
    auto* context = static_cast<PcJpeg*>(decoder->client_data);
    std::longjmp(context->recovery, 1);
}
int jpeg_dec_open(const jpeg_dec_config_t* config, jpeg_dec_handle_t* out) {
    if (!config || !out) return JPEG_ERR_FAIL;
    auto* context = new (std::nothrow) PcJpeg;
    if (!context) return JPEG_ERR_FAIL;
    context->decoder.err = jpeg_std_error(&context->errors);
    context->errors.error_exit = OnJpegError;
    context->decoder.client_data = context;
    context->rgb565 = config->output_type == JPEG_PIXEL_FORMAT_RGB565_LE;
    if (setjmp(context->recovery)) {
        delete context;
        return JPEG_ERR_FAIL;
    }
    jpeg_create_decompress(&context->decoder);
    *out = context;
    return JPEG_ERR_OK;
}
int jpeg_dec_parse_header(jpeg_dec_handle_t c, jpeg_dec_io_t* io, jpeg_dec_header_info_t* header) {
    if (!c || !io || !header || !io->inbuf || io->inbuf_len <= 0) return JPEG_ERR_FAIL;
    if (setjmp(c->recovery)) return JPEG_ERR_FAIL;
    jpeg_mem_src(&c->decoder, io->inbuf, io->inbuf_len);
    if (jpeg_read_header(&c->decoder, TRUE) != JPEG_HEADER_OK) return JPEG_ERR_FAIL;
    header->width = c->decoder.image_width;
    header->height = c->decoder.image_height;
    return JPEG_ERR_OK;
}
int jpeg_dec_process(jpeg_dec_handle_t c, jpeg_dec_io_t* io) {
    if (!c || !io || !io->outbuf) return JPEG_ERR_FAIL;
    if (setjmp(c->recovery)) return JPEG_ERR_FAIL;
    c->decoder.out_color_space = JCS_RGB;
    jpeg_start_decompress(&c->decoder);
    const size_t width = c->decoder.output_width;
    c->row.resize(width * 3);
    while (c->decoder.output_scanline < c->decoder.output_height) {
        const auto y = c->decoder.output_scanline;
        JSAMPROW row = c->rgb565 ? c->row.data() : io->outbuf + y * width * 3;
        if (jpeg_read_scanlines(&c->decoder, &row, 1) != 1) return JPEG_ERR_FAIL;
        if (c->rgb565) {
            auto* pixels = reinterpret_cast<uint16_t*>(io->outbuf + y * width * 2);
            for (size_t x = 0; x < width; ++x)
                pixels[x] = ((row[3 * x] >> 3) << 11) | ((row[3 * x + 1] >> 2) << 5) | (row[3 * x + 2] >> 3);
        }
    }
    jpeg_finish_decompress(&c->decoder);
    return JPEG_ERR_OK;
}
int jpeg_dec_close(jpeg_dec_handle_t c) {
    if (!c) return JPEG_ERR_FAIL;
    jpeg_destroy_decompress(&c->decoder);
    delete c;
    return JPEG_ERR_OK;
}
