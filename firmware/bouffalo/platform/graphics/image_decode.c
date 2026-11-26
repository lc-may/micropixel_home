// SPDX-License-Identifier: Apache-2.0
#include "image_decode.h"

#include "abi/micropixel_abi.h"
#include "mm.h"
#include "platform/memory/psram_realloc.h"

#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_SIMD
#define STBI_NO_THREAD_LOCALS
#define STBI_NO_FAILURE_STRINGS
#define STBI_MAX_DIMENSIONS MICROPIXEL_ENCODED_TEXTURE_MAX_EDGE
#define STBI_MALLOC(n) kmalloc((n), MM_FLAG_HEAP_PSRAM_0 | MM_FLAG_ALIGN_8BYTE)
#define STBI_REALLOC(p, n) mpx_psram_realloc((p), (n))
#define STBI_FREE(p) kfree(p)
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

uint8_t* mpx_decode_image(const uint8_t* bytes, uint32_t length, int* width, int* height) {
    if (!bytes || !length || length > MICROPIXEL_ENCODED_TEXTURE_MAX_BYTES) return NULL;
    int channels;
    uint8_t* pixels = stbi_load_from_memory(bytes, (int)length, width, height, &channels, 4);
    if (!pixels) return NULL;
    for (int i = 0; i < *width * *height * 4; i += 4) {
        const uint8_t red = pixels[i];
        pixels[i] = pixels[i + 2];
        pixels[i + 2] = red;
    }
    return pixels;
}
