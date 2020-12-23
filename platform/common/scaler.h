/*
 * https://raw.github.com/dmitrysmagin/snes9x4d-rzx50/master/dingux-sdl/scaler.h
 */

#include <stdint.h>
void upscale_inter_x2_grid(uint32_t *dst, uint32_t *src, int width, int high);
void upscale_inter_x2_scanline(uint32_t *dst, uint32_t *src, int width, int high);
void upscale_inter_x2(uint32_t *dst, uint32_t *src, int width, int high);
void upscale_inter_x2_scanline_vertical(uint32_t *dst, uint32_t *src, int width, int high);
void upscale_inter_x2_scanline_with_zero(uint32_t *dst, uint32_t *src, int width, int high, int zeroCount);