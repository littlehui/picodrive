/*
 * PicoDrive
 * (C) notaz, 2013
 *
 * This work is licensed under the terms of MAME license.
 * See COPYING file in the top-level directory.
 */

#include <stdio.h>

#include "../libpicofe/input.h"
#include "../libpicofe/plat.h"
#include "../libpicofe/plat_sdl.h"
#include "../libpicofe/in_sdl.h"
#include "../libpicofe/gl.h"
#include "emu.h"
#include "menu_pico.h"
#include "input_pico.h"
#include "plat_sdl.h"
#include "version.h"
#include "scaler.h"

#include <pico/pico.h>

extern int vout_mode_hw2, vout_mode_hw_scanline, vout_mode_hw_grid, vout_mode_hw_scanline_vertical, vout_mode_auto_scanline;
extern struct plat_target plat_target;
extern int hardwarex2Flag;
static void *shadow_fb;

static struct in_pdata in_sdl_platform_data = {
	.defbinds = in_sdl_defbinds,
	.key_map = in_sdl_key_map,
	.joy_map = in_sdl_joy_map,
};

/* YUV stuff */
static int yuv_ry[32], yuv_gy[32], yuv_by[32];
static unsigned char yuv_u[32 * 2], yuv_v[32 * 2];
static unsigned char yuv_y[256];
static struct uyvy { uint32_t y:8; uint32_t vyu:24; } yuv_uyvy[65536];

void bgr_to_uyvy_init(void)
{
  int i, v;

  /* init yuv converter:
    y0 = (int)((0.299f * r0) + (0.587f * g0) + (0.114f * b0));
    y1 = (int)((0.299f * r1) + (0.587f * g1) + (0.114f * b1));
    u = (int)(8 * 0.565f * (b0 - y0)) + 128;
    v = (int)(8 * 0.713f * (r0 - y0)) + 128;
  */
  for (i = 0; i < 32; i++) {
    yuv_ry[i] = (int)(0.299f * i * 65536.0f + 0.5f);
    yuv_gy[i] = (int)(0.587f * i * 65536.0f + 0.5f);
    yuv_by[i] = (int)(0.114f * i * 65536.0f + 0.5f);
  }
  for (i = -32; i < 32; i++) {
    v = (int)(8 * 0.565f * i) + 128;
    if (v < 0)
      v = 0;
    if (v > 255)
      v = 255;
    yuv_u[i + 32] = v;
    v = (int)(8 * 0.713f * i) + 128;
    if (v < 0)
      v = 0;
    if (v > 255)
      v = 255;
    yuv_v[i + 32] = v;
  }
  // valid Y range seems to be 16..235
  for (i = 0; i < 256; i++) {
    yuv_y[i] = 16 + 219 * i / 32;
  }
  // everything combined into one large array for speed
  for (i = 0; i < 65536; i++) {
     int r = (i >> 11) & 0x1f, g = (i >> 6) & 0x1f, b = (i >> 0) & 0x1f;
     int y = (yuv_ry[r] + yuv_gy[g] + yuv_by[b]) >> 16;
     yuv_uyvy[i].y = yuv_y[y];
     yuv_uyvy[i].vyu = (yuv_v[r-y + 32] << 16) | (yuv_y[y] << 8) | yuv_u[b-y + 32];
  }
}

void rgb565_to_uyvy(void *d, const void *s, int pixels, int x2)
{
  uint32_t *dst = d;
  const uint16_t *src = s;

  if (x2)
  for (; pixels > 0; src += 4, dst += 4, pixels -= 4)
  {
    struct uyvy *uyvy0 = yuv_uyvy + src[0], *uyvy1 = yuv_uyvy + src[1];
    struct uyvy *uyvy2 = yuv_uyvy + src[2], *uyvy3 = yuv_uyvy + src[3];
    dst[0] = (uyvy0->y << 24) | uyvy0->vyu;
    dst[1] = (uyvy1->y << 24) | uyvy1->vyu;
    dst[2] = (uyvy2->y << 24) | uyvy2->vyu;
    dst[3] = (uyvy3->y << 24) | uyvy3->vyu;
  } else 
  for (; pixels > 0; src += 4, dst += 2, pixels -= 4)
  {
    struct uyvy *uyvy0 = yuv_uyvy + src[0], *uyvy1 = yuv_uyvy + src[1];
    struct uyvy *uyvy2 = yuv_uyvy + src[2], *uyvy3 = yuv_uyvy + src[3];
    dst[0] = (uyvy1->y << 24) | uyvy0->vyu;
    dst[1] = (uyvy3->y << 24) | uyvy2->vyu;
  }
}

static int clear_buf_cnt, clear_stat_cnt;

void plat_video_flip(void)
{
	if (plat_sdl_overlay != NULL) {
		SDL_Rect dstrect =
			{ 0, 0, plat_sdl_screen->w, plat_sdl_screen->h };

		SDL_LockYUVOverlay(plat_sdl_overlay);
		rgb565_to_uyvy(plat_sdl_overlay->pixels[0], shadow_fb,
				g_screen_ppitch * g_screen_height,
				plat_sdl_overlay->w > 2*plat_sdl_overlay->h);
		SDL_UnlockYUVOverlay(plat_sdl_overlay);
		SDL_DisplayYUVOverlay(plat_sdl_overlay, &dstrect);
	}
	else if (plat_sdl_gl_active) {
		gl_flip(shadow_fb, g_screen_ppitch, g_screen_height);
	}
	else {
		if (SDL_MUSTLOCK(plat_sdl_screen)) {
			SDL_UnlockSurface(plat_sdl_screen);
			SDL_Flip(plat_sdl_screen);
			SDL_LockSurface(plat_sdl_screen);
		} else {
            SDL_Flip(plat_sdl_screen);
        }

        g_screen_ptr = plat_sdl_screen->pixels;

        if (hardwarex2Flag) {
		    //scanline
            if (plat_target.vout_method == vout_mode_hw_scanline) {
                upscale_inter_x2_scanline((uint32_t *) plat_sdl_screen->pixels, (uint32_t *) plat_sdl_screen_ptr,
                                          plat_sdl_screen->w, plat_sdl_screen->h);
            } else if (plat_target.vout_method == vout_mode_hw2) {
                upscale_inter_x2((uint32_t*)plat_sdl_screen->pixels, (uint32_t*)plat_sdl_screen_ptr,plat_sdl_screen->w, plat_sdl_screen->h);
            } else if (plat_target.vout_method == vout_mode_hw_grid) {
                upscale_inter_x2_grid((uint32_t*)plat_sdl_screen->pixels, (uint32_t*)plat_sdl_screen_ptr,plat_sdl_screen->w, plat_sdl_screen->h);
            } else if (plat_target.vout_method == vout_mode_hw_scanline_vertical) {
                upscale_inter_x2_scanline_vertical((uint32_t*)plat_sdl_screen->pixels, (uint32_t*)plat_sdl_screen_ptr,plat_sdl_screen->w, plat_sdl_screen->h);
            } else if (plat_target.vout_method == vout_mode_auto_scanline) {
                if (g_screen_width == 640 && g_screen_height == 448) {
                    //vertical
                    upscale_inter_x2_scanline_vertical((uint32_t*)plat_sdl_screen->pixels, (uint32_t*)plat_sdl_screen_ptr,plat_sdl_screen->w, plat_sdl_screen->h);
                } else if (g_screen_width == 512 && g_screen_height == 480) {
                    upscale_inter_x2_scanline((uint32_t*)plat_sdl_screen->pixels, (uint32_t*)plat_sdl_screen_ptr,plat_sdl_screen->w, plat_sdl_screen->h);
                } else {
                    upscale_inter_x2_grid((uint32_t*)plat_sdl_screen->pixels, (uint32_t*)plat_sdl_screen_ptr,plat_sdl_screen->w, plat_sdl_screen->h);
                }
            }
            PicoDrawSetOutBuf(plat_sdl_screen_ptr, g_screen_ppitch);
        } else {
		    //sdl_window
            g_screen_ptr = plat_sdl_screen->pixels;
            plat_video_set_buffer(g_screen_ptr);
        }
		if (clear_buf_cnt) {
		    if (hardwarex2Flag) {
                memset(plat_sdl_screen_ptr, 0, plat_sdl_screen->w*plat_sdl_screen->h * 2);
            } else {
                memset(g_screen_ptr, 0, plat_sdl_screen->w*plat_sdl_screen->h * 2);
            }
            clear_buf_cnt--;
		}
	}

    if (clear_stat_cnt) {
        if (hardwarex2Flag) {
            unsigned short *d = (unsigned short *)plat_sdl_screen_ptr + g_screen_ppitch * g_screen_height;
            int l = g_screen_ppitch * 8;
            memset((int *)(d - l), 0, l * 2);
            clear_stat_cnt--;
        } else {
            unsigned short *d = (unsigned short *)g_screen_ptr + g_screen_ppitch * g_screen_height;
            int l = g_screen_ppitch * 8;
            memset((int *)(d - l), 0, l * 2);
            clear_stat_cnt--;
        }
    }
}

void plat_video_wait_vsync(void)
{
}

void plat_video_clear_status(void)
{
	clear_stat_cnt = 3; // do it thrice in case of triple buffering
}

void plat_video_clear_buffers(void)
{
	if (plat_sdl_overlay != NULL || plat_sdl_gl_active)
		memset(shadow_fb, 0, plat_sdl_screen->w*plat_sdl_screen->h * 2);
	else {
		memset(g_screen_ptr, 0, plat_sdl_screen->w*plat_sdl_screen->h * 2);
		clear_buf_cnt = 3; // do it thrice in case of triple buffering
	}
}

void plat_video_menu_enter(int is_rom_loaded)
{
	if (SDL_MUSTLOCK(plat_sdl_screen))
		SDL_UnlockSurface(plat_sdl_screen);
    //fprintf(stderr, "plat_video_menu_enter\n");
    //fprintf(stderr, "g_menuscreen_w = %d\n", g_menuscreen_w);
    //fprintf(stderr, "g_menuscreen_h = %d\n", g_menuscreen_h);

    plat_sdl_change_video_mode(g_menuscreen_w, g_menuscreen_h, 0);
	g_screen_ptr = shadow_fb;
	plat_video_set_buffer(g_screen_ptr);
}

void plat_video_menu_begin(void)
{
	if (plat_sdl_overlay != NULL || plat_sdl_gl_active) {
		g_menuscreen_ptr = shadow_fb;
	}
	else {
		if (SDL_MUSTLOCK(plat_sdl_screen))
			SDL_LockSurface(plat_sdl_screen);
		g_menuscreen_ptr = plat_sdl_screen->pixels;
	}
}

void plat_video_menu_end(void)
{
	if (plat_sdl_overlay != NULL) {
		SDL_Rect dstrect =
			{ 0, 0, plat_sdl_screen->w, plat_sdl_screen->h };

		SDL_LockYUVOverlay(plat_sdl_overlay);
		rgb565_to_uyvy(plat_sdl_overlay->pixels[0], shadow_fb,
				g_menuscreen_pp * g_menuscreen_h, 0);
		SDL_UnlockYUVOverlay(plat_sdl_overlay);

		SDL_DisplayYUVOverlay(plat_sdl_overlay, &dstrect);
	}
	else if (plat_sdl_gl_active) {
		gl_flip(g_menuscreen_ptr, g_menuscreen_pp, g_menuscreen_h);
	}
	else {
		if (SDL_MUSTLOCK(plat_sdl_screen))
			SDL_UnlockSurface(plat_sdl_screen);
		SDL_Flip(plat_sdl_screen);
	}
	g_menuscreen_ptr = NULL;
}

void plat_video_menu_leave(void)
{
    if (plat_target.vout_method == 0) {
        hardwarex2Flag = 0;
    }
    fprintf(stderr, "plat_video_menu_leave  w = %d, h = %d\n", g_screen_width, g_screen_height);
    plat_sdl_change_video_mode(g_screen_width, g_screen_height, 0);
}

void plat_video_loop_prepare(void)
{
    if (plat_sdl_overlay != NULL || plat_sdl_gl_active) {
        g_screen_ptr = shadow_fb;
        g_screen_width = 320;
        g_screen_height = 240;
        g_screen_ppitch = g_screen_width;
	}
	else {
		if (SDL_MUSTLOCK(plat_sdl_screen))
			SDL_LockSurface(plat_sdl_screen);

        g_screen_ptr = plat_sdl_screen->pixels;

#if defined(__RG350__) || defined(__GCW0__)
        //hardware no need set
#else
        g_screen_width = g_menuscreen_w;
        g_screen_height = g_menuscreen_h;
        g_screen_ppitch = g_menuscreen_pp;

#endif

    }
    plat_video_set_buffer(g_screen_ptr);
    plat_sdl_change_video_mode(g_screen_width, g_screen_height, 0);
}

void plat_video_loop_prepare2(void)
{
    //littlehui modify
    fprintf(stderr, "plat_video_loop_prepare2  w = %d, h = %d\n", g_screen_width, g_screen_height);
    if (!hardwarex2Flag) {
        g_menuscreen_w = g_screen_width;
        g_menuscreen_h = g_screen_height;
        g_menuscreen_pp = g_screen_ppitch;
    }
    if (plat_sdl_overlay != NULL || plat_sdl_gl_active) {
        g_screen_ptr = shadow_fb;
    }
    else {
        if (SDL_MUSTLOCK(plat_sdl_screen))
            SDL_LockSurface(plat_sdl_screen);
        g_screen_ptr = plat_sdl_screen->pixels;
    }
    plat_video_set_buffer(g_screen_ptr);
    plat_sdl_change_video_mode(g_screen_width, g_screen_height, 0);
}

void plat_early_init(void)
{
}

static void plat_sdl_quit(void)
{
	// for now..
	exit(1);
}

void plat_init(void)
{
	int shadow_size;
	int ret;

	ret = plat_sdl_init();
	if (ret != 0)
		exit(1);
	SDL_ShowCursor(0);
#if defined(__RG350__) || defined(__GCW0__) || defined(__OPENDINGUX__)
	// opendingux on JZ47x0 may falsely report a HW overlay, fix to window
	plat_target.vout_method = 0;
#endif

	plat_sdl_quit_cb = plat_sdl_quit;

	SDL_WM_SetCaption("PicoDrive " VERSION, NULL);

	g_menuscreen_w = plat_sdl_screen->w;
	g_menuscreen_h = plat_sdl_screen->h;
	g_menuscreen_pp = g_menuscreen_w;
	g_menuscreen_ptr = NULL;

	shadow_size = g_menuscreen_w * g_menuscreen_h * 2;
/*	if (shadow_size < 320 * 480 * 2)
		shadow_size = 320 * 480 * 2;*/
    if (shadow_size < 640 * 480 * 2)
        shadow_size = 640 * 480 * 2;
	shadow_fb = calloc(1, shadow_size);
	//shadow_fb = malloc(g_menuscreen_w * g_menuscreen_h);
	g_menubg_ptr = calloc(1, shadow_size);
    plat_sdl_screen_ptr = calloc(1, shadow_size);

    if (shadow_fb == NULL || g_menubg_ptr == NULL) {
		fprintf(stderr, "OOM\n");
		exit(1);
	}

    if (hardwarex2Flag) {
        g_screen_width = 640;
        g_screen_height = 480;
        g_screen_ppitch = 640;
        g_screen_ptr = shadow_fb;
    } else {
		g_screen_width = 320;
		g_screen_height = 240;
		g_screen_ppitch = 320;
		g_screen_ptr = shadow_fb;
	}
	in_sdl_platform_data.kmap_size = in_sdl_key_map_sz,
	in_sdl_platform_data.jmap_size = in_sdl_joy_map_sz,
	in_sdl_platform_data.key_names = *in_sdl_key_names,
	in_sdl_init(&in_sdl_platform_data, plat_sdl_event_handler);
	in_probe();

	bgr_to_uyvy_init();
}

void plat_finish(void)
{
	free(shadow_fb);
	shadow_fb = NULL;
	free(g_menubg_ptr);
	g_menubg_ptr = NULL;
	plat_sdl_finish();
    plat_sdl_screen_ptr = NULL;
}
