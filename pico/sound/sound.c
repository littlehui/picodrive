/*
 * PicoDrive
 * (c) Copyright Dave, 2004
 * (C) notaz, 2006-2009
 *
 * This work is licensed under the terms of MAME license.
 * See COPYING file in the top-level directory.
 */

#include <string.h>
#include "ym2612.h"
#include "sn76496.h"
#include "../pico_int.h"
#include "../cd/cue.h"
#include "mix.h"
#include "emu2413/emu2413.h"

void (*PsndMix_32_to_16l)(short *dest, int *src, int count) = mix_32_to_16l_stereo;

// master int buffer to mix to
static int PsndBuffer[2*(44100+100)/50];

// cdda output buffer
short cdda_out_buffer[2*1152];

// sn76496
extern int *sn76496_regs;

// ym2413
#define YM2413_CLK 3579545
OPLL old_opll;
static OPLL *opll = NULL;
unsigned YM2413_reg;


PICO_INTERNAL void PsndInit(void)
{
  opll = OPLL_new(YM2413_CLK, PicoIn.sndRate);
  OPLL_setChipType(opll,0);
  OPLL_reset(opll);
}

PICO_INTERNAL void PsndExit(void)
{
  OPLL_delete(opll);
  opll = NULL;
}

PICO_INTERNAL void PsndReset(void)
{
  // PsndRerate calls YM2612Init, which also resets
  PsndRerate(0);
  timers_reset();

  mix_reset(PicoIn.sndFilter ? PicoIn.sndFilterRange : 0);
}


// to be called after changing sound rate or chips
void PsndRerate(int preserve_state)
{
  void *state = NULL;
  int target_fps = Pico.m.pal ? 50 : 60;
  int target_lines = Pico.m.pal ? 313 : 262;

  if (preserve_state) {
    state = malloc(0x204);
    if (state == NULL) return;
    ym2612_pack_state();
    memcpy(state, YM2612GetRegs(), 0x204);
  }
  YM2612Init(Pico.m.pal ? OSC_PAL/7 : OSC_NTSC/7, PicoIn.sndRate, !(PicoIn.opt&POPT_DIS_FM_SSGEG));
  if (preserve_state) {
    // feed it back it's own registers, just like after loading state
    memcpy(YM2612GetRegs(), state, 0x204);
    ym2612_unpack_state();
  }

  if (preserve_state) memcpy(state, sn76496_regs, 28*4); // remember old state
  SN76496_init(Pico.m.pal ? OSC_PAL/15 : OSC_NTSC/15, PicoIn.sndRate);
  if (preserve_state) memcpy(sn76496_regs, state, 28*4); // restore old state

  if(opll != NULL){
    if (preserve_state) memcpy(&old_opll, opll, sizeof(OPLL)); // remember old state
    OPLL_setRate(opll, PicoIn.sndRate);
    OPLL_reset(opll);
  }

  if (state)
    free(state);

  // calculate Pico.snd.len
  Pico.snd.len = PicoIn.sndRate / target_fps;
  Pico.snd.len_e_add = ((PicoIn.sndRate - Pico.snd.len * target_fps) << 16) / target_fps;
  Pico.snd.len_e_cnt = 0; // Q16

  // samples per line (Q16)
  Pico.snd.smpl_mult = 65536LL * PicoIn.sndRate / (target_fps*target_lines);
  // samples per z80 clock (Q20)
  Pico.snd.clkl_mult = 16 * Pico.snd.smpl_mult * 15/7 / 488;

  // clear all buffers
  memset32(PsndBuffer, 0, sizeof(PsndBuffer)/4);
  memset(cdda_out_buffer, 0, sizeof(cdda_out_buffer));
  if (PicoIn.sndOut)
    PsndClear();

  // set mixer
  PsndMix_32_to_16l = (PicoIn.opt & POPT_EN_STEREO) ? mix_32_to_16l_stereo : mix_32_to_16_mono;

  if (PicoIn.AHW & PAHW_PICO)
    PicoReratePico();
}


PICO_INTERNAL void PsndStartFrame(void)
{
  // compensate for float part of Pico.snd.len
  Pico.snd.len_use = Pico.snd.len;
  Pico.snd.len_e_cnt += Pico.snd.len_e_add;
  if (Pico.snd.len_e_cnt >= 0x10000) {
    Pico.snd.len_e_cnt -= 0x10000;
    Pico.snd.len_use++;
  }
}

PICO_INTERNAL void PsndDoDAC(int cyc_to)
{
  int pos, len;
  int dout = ym2612.dacout;

  // number of samples to fill in buffer (Q20)
  len = (cyc_to * Pico.snd.clkl_mult) - Pico.snd.dac_pos;

  // update position and calculate buffer offset and length
  pos = (Pico.snd.dac_pos+0x80000) >> 20;
  Pico.snd.dac_pos += len;
  len = ((Pico.snd.dac_pos+0x80000) >> 20) - pos;

  // avoid loss of the 1st sample of a new block (Q rounding issues)
  if (pos+len == 0)
    len = 1, Pico.snd.dac_pos += 0x80000;
  if (len <= 0)
    return;

  if (!PicoIn.sndOut)
    return;

  // fill buffer, applying a rather weak order 1 bessel IIR on the way
  // y[n] = (x[n] + x[n-1])*(1/2) (3dB cutoff at 11025 Hz, no gain)
  // 1 sample delay for correct IIR filtering over audio frame boundaries
  if (PicoIn.opt & POPT_EN_STEREO) {
    short *d = PicoIn.sndOut + pos*2;
    // left channel only, mixed ro right channel in mixing phase
    *d++ += Pico.snd.dac_val2; d++;
    while (--len) *d++ += Pico.snd.dac_val, d++;
  } else {
    short *d = PicoIn.sndOut + pos;
    *d++ += Pico.snd.dac_val2;
    while (--len) *d++ += Pico.snd.dac_val;
  }
  Pico.snd.dac_val2 = (Pico.snd.dac_val + dout) >> 1;
  Pico.snd.dac_val = dout;
}

PICO_INTERNAL void PsndDoPSG(int line_to)
{
  int pos, len;
  int stereo = 0;

  // Q16, number of samples since last call
  len = ((line_to+1) * Pico.snd.smpl_mult) - Pico.snd.psg_pos;
  if (len <= 0)
    return;

  // update position and calculate buffer offset and length
  pos = (Pico.snd.psg_pos+0x8000) >> 16;
  Pico.snd.psg_pos += len;
  len = ((Pico.snd.psg_pos+0x8000) >> 16) - pos;

  if (!PicoIn.sndOut || !(PicoIn.opt & POPT_EN_PSG))
    return;

  if (PicoIn.opt & POPT_EN_STEREO) {
    stereo = 1;
    pos <<= 1;
  }
  SN76496Update(PicoIn.sndOut + pos, len, stereo);
}

#if 0
PICO_INTERNAL void PsndDoYM2413(int line_to)
{
  int pos, len;
  int stereo = 0;
  short *buf;

  // Q16, number of samples since last call
  len = ((line_to+1) * Pico.snd.smpl_mult) - Pico.snd.ym2413_pos;
  if (len <= 0)
    return;

  // update position and calculate buffer offset and length
  pos = (Pico.snd.ym2413_pos+0x8000) >> 16;
  Pico.snd.ym2413_pos += len;
  len = ((Pico.snd.ym2413_pos+0x8000) >> 16) - pos;

  if (!PicoIn.sndOut || !(PicoIn.opt & POPT_EN_YM2413))
    return;

  if (PicoIn.opt & POPT_EN_STEREO) {
    stereo = 1;
    pos <<= 1;
  }

  buf = PicoIn.sndOut + pos;
  while (len-- > 0) {
    int16_t getdata = OPLL_calc(opll) * 3;
    *buf++ += getdata;
    buf += stereo; // only left for stereo, to be mixed to right later
  }
}
#endif

void YM2413_regWrite(unsigned data){
  OPLL_writeIO(opll,0,data);
}
void YM2413_dataWrite(unsigned data){
  OPLL_writeIO(opll,1,data);
}


PICO_INTERNAL void PsndDoFM(int cyc_to)
{
  int pos, len;
  int stereo = 0;

  // Q16, number of samples since last call
  len = (cyc_to * Pico.snd.clkl_mult) - Pico.snd.fm_pos;

  // don't do this too often (about once every canline)
  if (len >> 16 <= PicoIn.sndRate >> 10)
    return;

  // update position and calculate buffer offset and length
  pos = (Pico.snd.fm_pos+0x80000) >> 20;
  Pico.snd.fm_pos += len;
  len = ((Pico.snd.fm_pos+0x80000) >> 20) - pos;

  // fill buffer
  if (PicoIn.opt & POPT_EN_STEREO) {
    stereo = 1;
    pos <<= 1;
  }
  if (PicoIn.opt & POPT_EN_FM)
    YM2612UpdateOne(PsndBuffer + pos, len, stereo, 1);
}

// cdda
static void cdda_raw_update(int *buffer, int length)
{
  int ret, cdda_bytes, mult = 1;

  cdda_bytes = length*4;
  if (PicoIn.sndRate <= 22050 + 100) mult = 2;
  if (PicoIn.sndRate <  22050 - 100) mult = 4;
  cdda_bytes *= mult;

  ret = pm_read(cdda_out_buffer, cdda_bytes, Pico_mcd->cdda_stream);
  if (ret < cdda_bytes) {
    memset((char *)cdda_out_buffer + ret, 0, cdda_bytes - ret);
    Pico_mcd->cdda_stream = NULL;
    return;
  }

  // now mix
  switch (mult) {
    case 1: mix_16h_to_32(buffer, cdda_out_buffer, length*2); break;
    case 2: mix_16h_to_32_s1(buffer, cdda_out_buffer, length*2); break;
    case 4: mix_16h_to_32_s2(buffer, cdda_out_buffer, length*2); break;
  }
}

void cdda_start_play(int lba_base, int lba_offset, int lb_len)
{
  if (Pico_mcd->cdda_type == CT_MP3)
  {
    int pos1024 = 0;

    if (lba_offset)
      pos1024 = lba_offset * 1024 / lb_len;

    mp3_start_play(Pico_mcd->cdda_stream, pos1024);
    return;
  }

  pm_seek(Pico_mcd->cdda_stream, (lba_base + lba_offset) * 2352, SEEK_SET);
  if (Pico_mcd->cdda_type == CT_WAV)
  {
    // skip headers, assume it's 44kHz stereo uncompressed
    pm_seek(Pico_mcd->cdda_stream, 44, SEEK_CUR);
  }
}


PICO_INTERNAL void PsndClear(void)
{
  int len = Pico.snd.len;
  if (Pico.snd.len_e_add) len++;
  if (PicoIn.opt & POPT_EN_STEREO)
    memset32((int *) PicoIn.sndOut, 0, len); // assume PicoIn.sndOut to be aligned
  else {
    short *out = PicoIn.sndOut;
    if ((uintptr_t)out & 2) { *out++ = 0; len--; }
    memset32((int *) out, 0, len/2);
    if (len & 1) out[len-1] = 0;
  }
  if (!(PicoIn.opt & POPT_EN_FM))
    memset32(PsndBuffer, 0, PicoIn.opt & POPT_EN_STEREO ? len*2 : len);
  // drop pos remainder to avoid rounding errors (not entirely correct though)
  Pico.snd.dac_pos = Pico.snd.fm_pos = Pico.snd.psg_pos = Pico.snd.ym2413_pos = 0;
}


static int PsndRender(int offset, int length)
{
  int *buf32;
  int stereo = (PicoIn.opt & 8) >> 3;
  int fmlen = ((Pico.snd.fm_pos+0x80000) >> 20);
  int daclen = ((Pico.snd.dac_pos+0x80000) >> 20);
  int psglen = ((Pico.snd.psg_pos+0x8000) >> 16);

  buf32 = PsndBuffer+(offset<<stereo);

  pprof_start(sound);

  if (PicoIn.AHW & PAHW_PICO) {
    PicoPicoPCMUpdate(PicoIn.sndOut+(offset<<stereo), length-offset, stereo);
    return length;
  }

  // Fill up DAC output in case of missing samples (Q16 rounding errors)
  if (length-daclen > 0) {
    short *dacbuf = PicoIn.sndOut + (daclen << stereo);
    Pico.snd.dac_pos += (length-daclen) << 20;
    *dacbuf++ += Pico.snd.dac_val2;
    if (stereo) dacbuf++;
    for (daclen++; length-daclen > 0; daclen++) {
      *dacbuf++ += Pico.snd.dac_val;
      if (stereo) dacbuf++;
    }
    Pico.snd.dac_val2 = Pico.snd.dac_val;
  }

  // Add in parts of the PSG output not yet done
  if (length-psglen > 0) {
    short *psgbuf = PicoIn.sndOut + (psglen << stereo);
    Pico.snd.psg_pos += (length-psglen) << 16;
    if (PicoIn.opt & POPT_EN_PSG)
      SN76496Update(psgbuf, length-psglen, stereo);
  }

  // Add in parts of the FM buffer not yet done
  if (length-fmlen > 0) {
    int *fmbuf = buf32 + ((fmlen-offset) << stereo);
    Pico.snd.fm_pos += (length-fmlen) << 20;
    if (PicoIn.opt & POPT_EN_FM)
      YM2612UpdateOne(fmbuf, length-fmlen, stereo, 1);
  }

  // CD: PCM sound
  if (PicoIn.AHW & PAHW_MCD) {
    pcd_pcm_update(buf32, length-offset, stereo);
  }

  // CD: CDDA audio
  // CD mode, cdda enabled, not data track, CDC is reading
  if ((PicoIn.AHW & PAHW_MCD) && (PicoIn.opt & POPT_EN_MCD_CDDA)
      && Pico_mcd->cdda_stream != NULL
      && !(Pico_mcd->s68k_regs[0x36] & 1))
  {
    // note: only 44, 22 and 11 kHz supported, with forced stereo
    if (Pico_mcd->cdda_type == CT_MP3)
      mp3_update(buf32, length-offset, stereo);
    else
      cdda_raw_update(buf32, length-offset);
  }

  if ((PicoIn.AHW & PAHW_32X) && (PicoIn.opt & POPT_EN_PWM))
    p32x_pwm_update(buf32, length-offset, stereo);

  // convert + limit to normal 16bit output
  PsndMix_32_to_16l(PicoIn.sndOut+(offset<<stereo), buf32, length-offset);

  pprof_end(sound);

  return length;
}

PICO_INTERNAL void PsndGetSamples(int y)
{
  static int curr_pos = 0;

  curr_pos  = PsndRender(0, Pico.snd.len_use);

  if (PicoIn.writeSound)
    PicoIn.writeSound(curr_pos * ((PicoIn.opt & POPT_EN_STEREO) ? 4 : 2));
  // clear sound buffer
  PsndClear();
}

static int PsndRenderMS(int offset, int length)
{
  int stereo = (PicoIn.opt & 8) >> 3;
  int psglen = ((Pico.snd.psg_pos+0x8000) >> 16);
  int ym2413len = ((Pico.snd.ym2413_pos+0x8000) >> 16);

  pprof_start(sound);

  // Add in parts of the PSG output not yet done
  if (length-psglen > 0) {
    short *psgbuf = PicoIn.sndOut + (psglen << stereo);
    Pico.snd.psg_pos += (length-psglen) << 16;
    if (PicoIn.opt & POPT_EN_PSG)
      SN76496Update(psgbuf, length-psglen, stereo);
  }

  if (length-ym2413len > 0) {
    short *ym2413buf = PicoIn.sndOut + (ym2413len << stereo);
    Pico.snd.ym2413_pos += (length-ym2413len) << 16;
    int len = (length-ym2413len);
    if (PicoIn.opt & POPT_EN_YM2413){
      while (len-- > 0) {
        int16_t getdata = OPLL_calc(opll) * 3;
        *ym2413buf += getdata;
        ym2413buf += 1<<stereo;
      }
    }
  }

  // upmix to "stereo" if needed
  if (PicoIn.opt & POPT_EN_STEREO) {
    int i;
    short *p;
    for (i = length, p = (short *)PicoIn.sndOut; i > 0; i--, p+=2)
      *(p + 1) = *p;
  }

  pprof_end(sound);

  return length;
}

PICO_INTERNAL void PsndGetSamplesMS(int y)
{
  static int curr_pos = 0;

  curr_pos  = PsndRenderMS(0, Pico.snd.len_use);

  if (PicoIn.writeSound != NULL)
    PicoIn.writeSound(curr_pos * ((PicoIn.opt & POPT_EN_STEREO) ? 4 : 2));
  PsndClear();
}

// vim:shiftwidth=2:ts=2:expandtab
