/*
 * mode4/SMS renderer
 * (C) notaz, 2009-2010
 *
 * This work is licensed under the terms of MAME license.
 * See COPYING file in the top-level directory.
 */
/*
 * TODO:
 * - TMS9918 modes?
 * - gg mode?
 * - column scroll (reg 0 bit7)
 * - 224/240 line modes
 * - doubled sprites
 */
#include "pico_int.h"

static void (*FinalizeLineM4)(int line);
static int skip_next_line;
static int screen_offset, line_offset;

static void TileBGM4(int sx, int pal)
{
  u32 *pd = (u32 *)(Pico.est.HighCol + sx);
  pd[0] = pd[1] = pal ? 0x10101010 : 0;
}

#define PLANAR_PIXELL(x,p) \
  t = pack & (0x80808080 >> p); \
  t = ((t >> (7-p)) | (t >> (14-p)) | (t >> (21-p)) | (t >> (28-p))) & 0x0f; \
  pd[x] = pal|t;

static void TileNormM4Low(int sx, unsigned int pack, int pal)
{
  unsigned char *pd = Pico.est.HighCol + sx;
  unsigned int t;

  PLANAR_PIXELL(0, 0)
  PLANAR_PIXELL(1, 1)
  PLANAR_PIXELL(2, 2)
  PLANAR_PIXELL(3, 3)
  PLANAR_PIXELL(4, 4)
  PLANAR_PIXELL(5, 5)
  PLANAR_PIXELL(6, 6)
  PLANAR_PIXELL(7, 7)
}

static void TileFlipM4Low(int sx, unsigned int pack, int pal)
{
  unsigned char *pd = Pico.est.HighCol + sx;
  unsigned int t;

  PLANAR_PIXELL(0, 7)
  PLANAR_PIXELL(1, 6)
  PLANAR_PIXELL(2, 5)
  PLANAR_PIXELL(3, 4)
  PLANAR_PIXELL(4, 3)
  PLANAR_PIXELL(5, 2)
  PLANAR_PIXELL(6, 1)
  PLANAR_PIXELL(7, 0)
}

#define PLANAR_PIXEL(x,p) \
  t = pack & (0x80808080 >> p); \
  if (t) { \
    t = ((t >> (7-p)) | (t >> (14-p)) | (t >> (21-p)) | (t >> (28-p))) & 0x0f; \
    pd[x] = pal|t; \
  }

static void TileNormM4(int sx, unsigned int pack, int pal)
{
  unsigned char *pd = Pico.est.HighCol + sx;
  unsigned int t;

  PLANAR_PIXEL(0, 0)
  PLANAR_PIXEL(1, 1)
  PLANAR_PIXEL(2, 2)
  PLANAR_PIXEL(3, 3)
  PLANAR_PIXEL(4, 4)
  PLANAR_PIXEL(5, 5)
  PLANAR_PIXEL(6, 6)
  PLANAR_PIXEL(7, 7)
}

static void TileFlipM4(int sx, unsigned int pack, int pal)
{
  unsigned char *pd = Pico.est.HighCol + sx;
  unsigned int t;

  PLANAR_PIXEL(0, 7)
  PLANAR_PIXEL(1, 6)
  PLANAR_PIXEL(2, 5)
  PLANAR_PIXEL(3, 4)
  PLANAR_PIXEL(4, 3)
  PLANAR_PIXEL(5, 2)
  PLANAR_PIXEL(6, 1)
  PLANAR_PIXEL(7, 0)
}

static void draw_sprites(int scanline)
{
  struct PicoVideo *pv = &Pico.video;
  unsigned int sprites_addr[8];
  unsigned int sprites_x[8];
  unsigned int pack;
  unsigned char *sat;
  int xoff = 8; // relative to HighCol, which is (screen - 8)
  int sprite_base, addr_mask;
  int i, s, h;

  if (pv->reg[0] & 8)
    xoff = 0;
  xoff += line_offset;

  sat = (unsigned char *)PicoMem.vram + ((pv->reg[5] & 0x7e) << 7);
  if (pv->reg[1] & 2) {
    addr_mask = 0xfe; h = 16;
  } else {
    addr_mask = 0xff; h = 8;
  }
  sprite_base = (pv->reg[6] & 4) << (13-2-1);

  for (i = s = 0; i < 64; i++)
  {
    int y;
    y = sat[i] + 1;
    if (y == 0xd1)
      break;
    if (y + h <= scanline || scanline < y)
      continue; // not on this line
    if (s >= 8) {
      pv->status |= SR_SOVR;
      break;
    }

    sprites_x[s] = xoff + sat[0x80 + i*2];
    sprites_addr[s] = sprite_base + ((sat[0x80 + i*2 + 1] & addr_mask) << (5-1)) +
      ((scanline - y) << (2-1));
    s++;
  }

  // really half-assed but better than nothing
  if (s > 1)
    pv->status |= SR_C;

  // now draw all sprites backwards
  for (--s; s >= 0; s--) {
    pack = *(u32 *)(PicoMem.vram + sprites_addr[s]);
    TileNormM4(sprites_x[s], pack, 0x10);
  }
}


// tilex_ty_prio merged to reduce register pressure
static void draw_strip_low(const unsigned short *nametab, int dx, int cells, int tilex_ty_prio)
{
  int oldcode = -1;
  int addr = 0, pal = 0;

  // Draw tiles across screen:
  for (; cells > 0; dx += 8, tilex_ty_prio++, cells--)
  {
    unsigned int pack;
    unsigned code;

    code = nametab[tilex_ty_prio & 0x1f];

    if (code != oldcode) {
      oldcode = code;
      // Get tile address/2:
      addr = (code & 0x1ff) << 4;
      addr += tilex_ty_prio >> 16;
      if (code & 0x0400)
        addr ^= 0xe; // Y-flip

      pal = (code>>7) & 0x10;
    }

    pack = *(u32 *)(PicoMem.vram + addr); /* Get 4 bitplanes / 8 pixels */
    if (pack == 0)          TileBGM4(dx, pal);
    else if (code & 0x0200) TileFlipM4Low(dx, pack, pal);
    else                    TileNormM4Low(dx, pack, pal);
  }
}
// tilex_ty_prio merged to reduce register pressure
static void draw_strip_high(const unsigned short *nametab, int dx, int cells, int tilex_ty_prio)
{
  int oldcode = -1, blank = -1; // The tile we know is blank
  int addr = 0, pal = 0;

  // Draw tiles across screen:
  for (; cells > 0; dx += 8, tilex_ty_prio++, cells--)
  {
    unsigned int pack;
    unsigned code;

    code = nametab[tilex_ty_prio & 0x1f];
    if (code == blank)
      continue;
    if ((code ^ tilex_ty_prio) & 0x1000) // priority differs?
      continue;

    if (code != oldcode) {
      oldcode = code;
      // Get tile address/2:
      addr = (code & 0x1ff) << 4;
      addr += tilex_ty_prio >> 16;
      if (code & 0x0400)
        addr ^= 0xe; // Y-flip

      pal = (code>>7) & 0x10;
    }

    pack = *(u32 *)(PicoMem.vram + addr); /* Get 4 bitplanes / 8 pixels */
    if (pack == 0) {
      blank = code;
      continue;
    }
    if (code & 0x0200) TileFlipM4(dx, pack, pal);
    else               TileNormM4(dx, pack, pal);
  }
}

static void DrawDisplayM4(int scanline)
{
  struct PicoVideo *pv = &Pico.video;
  unsigned short *nametab;
  int line, tilex, dx, ty, cells;
  int cellskip = 0; // XXX
  int maxcells = 32;

  // Find the line in the name table
  line = pv->reg[9] + scanline; // vscroll + scanline
  if (line >= 224)
    line -= 224;

  // Find name table:
  nametab = PicoMem.vram;
  nametab += (pv->reg[2] & 0x0e) << (10-1);
  nametab += (line>>3) << (6-1);

  dx = pv->reg[8]; // hscroll
  if (scanline < 16 && (pv->reg[0] & 0x40))
    dx = 0; // hscroll disabled for top 2 rows

  tilex = ((-dx >> 3) + cellskip) & 0x1f;
  ty = (line & 7) << 1; // Y-Offset into tile
  cells = maxcells - cellskip;

  dx = ((dx - 1) & 7) + 1;
  if (dx != 8)
    cells++; // have hscroll, need to draw 1 cell more
  dx += cellskip << 3;
  dx += line_offset;

  // low priority tiles
  if (!(pv->debug_p & PVD_KILL_B))
    draw_strip_low(nametab, dx, cells, tilex | 0x0000 | (ty << 16));

  // sprites
  if (!(pv->debug_p & PVD_KILL_S_LO))
    draw_sprites(scanline);

  // high priority tiles (use virtual layer switch just for fun)
  if (!(pv->debug_p & PVD_KILL_A))
    draw_strip_high(nametab, dx, cells, tilex | 0x1000 | (ty << 16));

  if (pv->reg[0] & 0x20) {
    // first column masked, caculate offset to start of line
    dx = (dx&~0x1f) / 4;
    ((u32 *)Pico.est.HighCol)[dx+2] = ((u32 *)Pico.est.HighCol)[dx+3] = 0xe0e0e0e0;
  }
}

void PicoFrameStartMode4(void)
{
  int lines = 192;
  skip_next_line = 0;
  screen_offset = 24;
  Pico.est.rendstatus = PDRAW_32_COLS;

  if ((Pico.video.reg[0] & 6) == 6 && (Pico.video.reg[1] & 0x18)) {
    if (Pico.video.reg[1] & 0x08) {
      screen_offset = 0;
      lines = 240;
    }
    else {
      screen_offset = 8;
      lines = 224;
    }
  }

  if (Pico.est.rendstatus != rendstatus_old || lines != rendlines) {
    emu_video_mode_change(screen_offset, lines, 1);
    rendstatus_old = Pico.est.rendstatus;
    rendlines = lines;
  }

  Pico.est.HighCol = HighColBase + screen_offset * HighColIncrement;
  Pico.est.DrawLineDest = (char *)DrawLineDestBase + screen_offset * DrawLineDestIncrement;
}

void PicoLineMode4(int line)
{
  if (skip_next_line > 0) {
    skip_next_line--;
    return;
  }

  if (PicoScanBegin != NULL)
    skip_next_line = PicoScanBegin(line + screen_offset);

  // Draw screen:
  BackFill(Pico.video.reg[7] & 0x0f, 0, &Pico.est);
  if (Pico.video.reg[1] & 0x40)
    DrawDisplayM4(line);

  if (FinalizeLineM4 != NULL)
    FinalizeLineM4(line);

  if (PicoScanEnd != NULL)
    skip_next_line = PicoScanEnd(line + screen_offset);

  Pico.est.HighCol += HighColIncrement;
  Pico.est.DrawLineDest = (char *)Pico.est.DrawLineDest + DrawLineDestIncrement;
}

void PicoDoHighPal555M4(void)
{
  unsigned int *spal=(void *)PicoMem.cram;
  unsigned int *dpal=(void *)Pico.est.HighPal;
  unsigned int t;
  int i;

  Pico.m.dirtyPal = 0;

  /* cram is always stored as shorts, even though real hardware probably uses bytes */
  for (i = 0x20/2; i > 0; i--, spal++, dpal++) {
    t = *spal;
#if defined(USE_BGR555)
    t = ((t & 0x00030003)<< 3) | ((t & 0x000c000c)<<6) | ((t & 0x00300030)<<9);
    t |= (t >> 2) | ((t >> 4) & 0x04210421);
#elif defined(USE_BGR565)
    t = ((t & 0x00030003)<< 3) | ((t & 0x000c000c)<<7) | ((t & 0x00300030)<<10);
    t |= (t >> 2) | ((t >> 4) & 0x08610861);
#else
    t = ((t & 0x00030003)<<14) | ((t & 0x000c000c)<<7) | ((t & 0x00300030)>>1);
    t |= (t >> 2) | ((t >> 4) & 0x08610861);
#endif
    *dpal = t;
  }
  Pico.est.HighPal[0xe0] = 0;
}

static void FinalizeLineRGB555M4(int line)
{
  if (Pico.m.dirtyPal)
    PicoDoHighPal555M4();

  // standard FinalizeLine can finish it for us,
  // with features like scaling and such
  FinalizeLine555(0, line, &Pico.est);
}

static void FinalizeLine8bitM4(int line)
{
  unsigned char *pd = Pico.est.DrawLineDest;

  if (DrawLineDestIncrement)
    memcpy(pd + line_offset, Pico.est.HighCol + line_offset + 8, 256);
}

void PicoDrawSetOutputMode4(pdso_t which)
{
  line_offset = PicoIn.opt & POPT_DIS_32C_BORDER ? 0 : 32;
  switch (which)
  {
    case PDF_8BIT:   FinalizeLineM4 = FinalizeLine8bitM4; break;
    case PDF_RGB555: FinalizeLineM4 = FinalizeLineRGB555M4;
                     line_offset = 0 /* done in FinalizeLine */; break;
    default:         FinalizeLineM4 = NULL;
                     PicoDrawSetInternalBuf(Pico.est.Draw2FB, 328); break;
  }
}

// vim:shiftwidth=2:ts=2:expandtab
