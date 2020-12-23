
#include "scaler.h"
#include <stdio.h>
#define AVERAGE(z, x) ((((z) & 0xF7DEF7DE) >> 1) + (((x) & 0xF7DEF7DE) >> 1))
#define AVERAGEHI(AB) ((((AB) & 0xF7DE0000) >> 1) + (((AB) & 0xF7DE) << 15))
#define AVERAGELO(CD) ((((CD) & 0xF7DE) >> 1) + (((CD) & 0xF7DE0000) >> 17))

//void (*upscale_p)(uint32_t *dst, uint32_t *src, int width) = upscale_256x224_to_320x240;

// Support math
#define Half(A) (((A) >> 1) & 0x7BEF)
#define Quarter(A) (((A) >> 2) & 0x39E7)
// Error correction expressions to piece back the lower bits together
#define RestHalf(A) ((A) & 0x0821)
#define RestQuarter(A) ((A) & 0x1863)

// Error correction expressions for quarters of pixels
#define Corr1_3(A, B)     Quarter(RestQuarter(A) + (RestHalf(B) << 1) + RestQuarter(B))
#define Corr3_1(A, B)     Quarter((RestHalf(A) << 1) + RestQuarter(A) + RestQuarter(B))

// Error correction expressions for halves
#define Corr1_1(A, B)     ((A) & (B) & 0x0821)

// Quarters
#define Weight1_3(A, B)   (Quarter(A) + Half(B) + Quarter(B) + Corr1_3(A, B))
#define Weight3_1(A, B)   (Half(A) + Quarter(A) + Quarter(B) + Corr3_1(A, B))

// Halves
#define Weight1_1(A, B)   (Half(A) + Half(B) + Corr1_1(A, B))

#define cR(A) (((A) & 0xf800) >> 8)
#define cG(A) (((A) & 0x7e0) >> 3)
#define cB(A) (((A) & 0x1f) << 3)

#define Weight2_1(A, B)  ((((cR(A) + cR(A) + cR(B)) / 3) & 0xf8) << 8 | (((cG(A) + cG(A) + cG(B)) / 3) & 0xfc) << 3 | (((cB(A) + cB(A) + cB(B)) / 3) & 0xf8) >> 3)

int static PICO_FULL_HEIGHT = 480;
uint16_t hexcolor_to_rgb565(const uint32_t color)
{
    uint8_t colorr = ((color >> 16) & 0xFF);
    uint8_t colorg = ((color >> 8) & 0xFF);
    uint8_t colorb = ((color) & 0xFF);

    uint16_t r = ((colorr >> 3) & 0x1f) << 11;
    uint16_t g = ((colorg >> 2) & 0x3f) << 5;
    uint16_t b = (colorb >> 3) & 0x1f;

    return (uint16_t) (r | g | b);
}


void upscale_inter_x2_scanline(uint32_t *dst, uint32_t *src, int width, int high) {
    uint16_t* Src16 = (uint16_t*) src;
    uint16_t* Dst16 = (uint16_t*) dst + (PICO_FULL_HEIGHT - high)/2;
    int interWidth = width >> 1;
    int interHigh = high >> 1;
    uint32_t BlockX, BlockY;
    uint16_t* BlockSrc;
    uint16_t* BlockDst;

    uint16_t gcolor = hexcolor_to_rgb565(0x000000);
    for (BlockY = 0; BlockY < interHigh; BlockY++)
    {
        //littlehui * 2
        BlockSrc = Src16 + BlockY * interWidth * 1;
        BlockDst = Dst16 + BlockY * width * 1 * 2;
        for (BlockX = 0; BlockX < interWidth; BlockX++)
        {
            /* Horizontally:
             * Before(1):
             * (a)
             * After(4):
             * (a)(a)
             * (c)(c)
             */
            //one

            uint16_t  a = *(BlockSrc);
            //fprintf(stderr, "row one is %d\n", a);

            uint16_t  scanline_color = Weight2_1( a, gcolor);
            // -- Row 1 --
            *(BlockDst) = a;
            *(BlockDst + 1) = a;
            // -- next row 2 --
            *(BlockDst +  width )  = scanline_color;
            *(BlockDst +  width + 1)  = scanline_color;
            BlockSrc += 1;
            BlockDst += 2;
        }
    }
}

void upscale_inter_x2_grid(uint32_t *dst, uint32_t *src, int width, int high) {
    uint16_t* Src16 = (uint16_t*) src;
    uint16_t* Dst16 = (uint16_t*) dst + (PICO_FULL_HEIGHT - high)/2;
    int interWidth = width >> 1;
    int interHigh = high >> 1;
    uint32_t BlockX, BlockY;
    uint16_t* BlockSrc;
    uint16_t* BlockDst;
    uint16_t gcolor = hexcolor_to_rgb565(0x000000);
    for (BlockY = 0; BlockY < interHigh; BlockY++)
    {
        //littlehui * 2
        BlockSrc = Src16 + BlockY * interWidth * 1;
        BlockDst = Dst16 + BlockY * width * 1 * 2;
        for (BlockX = 0; BlockX < interWidth; BlockX++)
        {
            /* Horizontally:
             * Before(1):
             * (a)
             * After(4):
             * (a)(ca)
             * (ca)(ca)
             */
            //one
            uint16_t  color = *(BlockSrc);
            uint16_t  scanline_color = Weight2_1( color, gcolor);

            //uint16_t scanline_color = (color + (color & 0x7474)) >> 1;
            //scanline_color = (color + scanline_color + ((color ^ scanline_color) & 0x0421)) >> 1;

            //uint32_t next_offset = (BlockX + 1) >= 256 ? 0 : (BlockX + 1);
            //uint32_t scanline_color = (uint32_t)bgr555_to_native_16(*(src + next_offset));

            // -- Row 1 --
            *(BlockDst) = color;
            *(BlockDst + 1) = scanline_color;
            // -- next row 2 --
            *(BlockDst +  width )  = scanline_color;
            *(BlockDst +  width + 1)  = scanline_color;
            BlockSrc += 1;
            BlockDst += 2;
        }
    }
}


void upscale_inter_x2(uint32_t *dst, uint32_t *src, int width, int high) {
    uint16_t* Src16 = (uint16_t*) src;
    uint16_t* Dst16 = (uint16_t*) dst;
    int interWidth = width >> 1;
    int interHigh = high >> 1;
    uint32_t BlockX, BlockY;
    uint16_t* BlockSrc;
    uint16_t* BlockDst;

    for (BlockY = 0; BlockY < interHigh; BlockY++)
    {
        //littlehui * 2
        BlockSrc = Src16 + BlockY * interWidth * 1;
        BlockDst = Dst16 + BlockY * width * 1 * 2;
        for (BlockX = 0; BlockX < interWidth; BlockX++)
        {
            /* Horizontally:
             * Before(1):
             * (a)
             * After(4):
             * (a)(a)
             * (a)(a)
             */
            //one

            uint16_t  color = *(BlockSrc);
            //fprintf(stderr, "row one is %d\n", a);

            // -- Row 1 --
            *(BlockDst) = color;
            *(BlockDst + 1) = color;
            // -- next row 2 --
            *(BlockDst +  width )  = color;
            *(BlockDst +  width + 1)  = color;
            BlockSrc += 1;
            BlockDst += 2;
        }
    }
}


void upscale_inter_x2_scanline_vertical(uint32_t *dst, uint32_t *src, int width, int high) {
    uint16_t* Src16 = (uint16_t*) src;
    uint16_t* Dst16 = (uint16_t*) dst + (PICO_FULL_HEIGHT - high)/2;
    int interWidth = width >> 1;
    int interHigh = high >> 1;
    uint32_t BlockX, BlockY;
    uint16_t* BlockSrc;
    uint16_t* BlockDst;

    uint16_t gcolor = hexcolor_to_rgb565(0x000000);
    for (BlockY = 0; BlockY < interHigh; BlockY++)
    {
        //littlehui * 2
        BlockSrc = Src16 + BlockY * interWidth * 1;
        BlockDst = Dst16 + BlockY * width * 1 * 2;
        for (BlockX = 0; BlockX < interWidth; BlockX++)
        {
            /* Horizontally:
             * Before(1):
             * (a)
             * After(4):
             * (a)
             * (c)
             */
            //one

            uint16_t  color = *(BlockSrc);
            uint16_t  scanline_color = Weight2_1( color, gcolor);

            // -- Row 1 --
            *(BlockDst) = color;
            *(BlockDst + 1) = scanline_color;
            // -- next row 2 --
            *(BlockDst +  width )  = color;
            *(BlockDst +  width + 1)  = scanline_color;
            BlockSrc += 1;
            BlockDst += 2;
        }
    }
}

void upscale_inter_x2_scanline_with_zero(uint32_t *dst, uint32_t *src, int width, int high, int zeroCount) {
    uint16_t* Src16 = (uint16_t*) src;
    uint16_t* Dst16 = (uint16_t*) dst + zeroCount;
    int interWidth = width >> 1;
    int interHigh = high >> 1;
    int zeroFill = 0;
    uint32_t BlockX, BlockY;
    uint16_t* BlockSrc;
    uint16_t* BlockDst;
/*    for (zeroFill = 0; zeroFill < zeroCount; zeroFill++) {
        //*(BlockDst + zeroFill) &= 0;
    }*/
    uint16_t gcolor = hexcolor_to_rgb565(0x000000);
    for (BlockY = 0; BlockY < interHigh; BlockY++)
    {
        //littlehui * 2
        BlockSrc = Src16 + BlockY * interWidth * 1;
        BlockDst = Dst16 + BlockY * width * 1 * 2;
        for (BlockX = 0; BlockX < interWidth; BlockX++)
        {
            /* Horizontally:
             * Before(1):
             * (a)
             * After(4):
             * (a)(a)
             * (c)(c)
             */
            //one

            uint16_t  a = *(BlockSrc);
            //fprintf(stderr, "row one is %d\n", a);

            uint16_t  scanline_color = Weight2_1( a, gcolor);
            // -- Row 1 --
            *(BlockDst) = a;
            *(BlockDst + 1) = a;
            // -- next row 2 --
            *(BlockDst +  width )  = scanline_color;
            *(BlockDst +  width + 1)  = scanline_color;
            BlockSrc += 1;
            BlockDst += 2;
        }
    }
}
