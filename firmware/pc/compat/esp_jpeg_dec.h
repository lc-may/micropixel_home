#pragma once
#include <stdint.h>
enum { JPEG_ERR_OK = 0, JPEG_ERR_FAIL = -1, JPEG_PIXEL_FORMAT_RGB565_LE = 0, JPEG_PIXEL_FORMAT_RGB888 = 1 };
struct jpeg_dec_config_t {
    int output_type;
};
#define DEFAULT_JPEG_DEC_CONFIG() \
    jpeg_dec_config_t { JPEG_PIXEL_FORMAT_RGB888 }
struct PcJpeg;
using jpeg_dec_handle_t = PcJpeg*;
struct jpeg_dec_io_t {
    uint8_t* inbuf;
    int inbuf_len;
    uint8_t* outbuf;
};
struct jpeg_dec_header_info_t {
    uint32_t width, height;
};
int jpeg_dec_open(const jpeg_dec_config_t*, jpeg_dec_handle_t*);
int jpeg_dec_parse_header(jpeg_dec_handle_t, jpeg_dec_io_t*, jpeg_dec_header_info_t*);
int jpeg_dec_process(jpeg_dec_handle_t, jpeg_dec_io_t*);
int jpeg_dec_close(jpeg_dec_handle_t);
