/* stb_image_write - v1.16 - public domain - http://nothings.org/stb
   writes out PNG/BMP/TGA/JPEG/HDR images to C stdio - Sean Barrett 2010-2015
   
   簡略版: PNG書き込み機能のみ実装
*/

#ifndef STBI_WRITE_H
#define STBI_WRITE_H

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

int stbi_write_png(char const *filename, int w, int h, int comp, const void *data, int stride_in_bytes);

#ifdef __cplusplus
}
#endif

#endif // STBI_WRITE_H

#ifdef STB_IMAGE_WRITE_IMPLEMENTATION

#include <zlib.h>

static void write_u32_be(FILE *f, uint32_t val) {
    uint8_t bytes[4] = {
        (uint8_t)(val >> 24),
        (uint8_t)(val >> 16),
        (uint8_t)(val >> 8),
        (uint8_t)val
    };
    fwrite(bytes, 1, 4, f);
}

static uint32_t crc32_table[256];
static int crc32_table_initialized = 0;

static void init_crc32_table() {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) {
            if (c & 1)
                c = 0xedb88320U ^ (c >> 1);
            else
                c = c >> 1;
        }
        crc32_table[i] = c;
    }
    crc32_table_initialized = 1;
}

static uint32_t calc_crc32(const uint8_t *data, size_t len) {
    if (!crc32_table_initialized) init_crc32_table();
    
    uint32_t crc = 0xffffffffU;
    for (size_t i = 0; i < len; i++) {
        crc = crc32_table[(crc ^ data[i]) & 0xff] ^ (crc >> 8);
    }
    return crc ^ 0xffffffffU;
}

static void write_chunk(FILE *f, const char *type, const uint8_t *data, uint32_t len) {
    write_u32_be(f, len);
    fwrite(type, 1, 4, f);
    if (data && len > 0) {
        fwrite(data, 1, len, f);
    }
    
    uint8_t crc_data[4 + len];
    memcpy(crc_data, type, 4);
    if (data && len > 0) {
        memcpy(crc_data + 4, data, len);
    }
    uint32_t crc = calc_crc32(crc_data, 4 + len);
    write_u32_be(f, crc);
}

int stbi_write_png(char const *filename, int w, int h, int comp, const void *data, int stride_in_bytes) {
    FILE *f = fopen(filename, "wb");
    if (!f) return 0;
    
    // PNG signature
    uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    fwrite(sig, 1, 8, f);
    
    // IHDR chunk
    uint8_t ihdr[13];
    ihdr[0] = (w >> 24) & 0xff;
    ihdr[1] = (w >> 16) & 0xff;
    ihdr[2] = (w >> 8) & 0xff;
    ihdr[3] = w & 0xff;
    ihdr[4] = (h >> 24) & 0xff;
    ihdr[5] = (h >> 16) & 0xff;
    ihdr[6] = (h >> 8) & 0xff;
    ihdr[7] = h & 0xff;
    ihdr[8] = 8;  // bit depth
    ihdr[9] = (comp == 4) ? 6 : (comp == 3) ? 2 : 0;  // color type (RGB or RGBA)
    ihdr[10] = 0; // compression
    ihdr[11] = 0; // filter
    ihdr[12] = 0; // interlace
    write_chunk(f, "IHDR", ihdr, 13);
    
    // Prepare image data with filter bytes
    size_t row_size = w * comp;
    size_t filtered_size = h * (1 + row_size);
    uint8_t *filtered = (uint8_t*)malloc(filtered_size);
    
    const uint8_t *src = (const uint8_t*)data;
    for (int y = 0; y < h; y++) {
        filtered[y * (1 + row_size)] = 0; // filter type: None
        memcpy(&filtered[y * (1 + row_size) + 1], &src[y * stride_in_bytes], row_size);
    }
    
    // Compress with zlib
    uLongf compressed_size = compressBound(filtered_size);
    uint8_t *compressed = (uint8_t*)malloc(compressed_size);
    
    if (compress(compressed, &compressed_size, filtered, filtered_size) != Z_OK) {
        free(filtered);
        free(compressed);
        fclose(f);
        return 0;
    }
    
    // IDAT chunk
    write_chunk(f, "IDAT", compressed, compressed_size);
    
    // IEND chunk
    write_chunk(f, "IEND", NULL, 0);
    
    free(filtered);
    free(compressed);
    fclose(f);
    return 1;
}

#endif // STB_IMAGE_WRITE_IMPLEMENTATION
