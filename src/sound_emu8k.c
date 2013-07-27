/*12log2(r) * 4096

  freq = 2^((in - 0xe000) / 4096)*/
/*LFO - lowest (0.042 Hz) = 2^20 steps = 1048576
        highest (10.72 Hz) = 2^12 steps = 4096*/
#include <stdlib.h>
#include <math.h>
#include "ibm.h"
#include "device.h"
#include "sound.h"
#include "sound_emu8k.h"
#include "timer.h"

enum
{
        ENV_STOPPED = 0,
        ENV_ATTACK  = 1,
        ENV_DECAY   = 2,
        ENV_SUSTAIN = 3,
        ENV_RELEASE = 4
};

static int64_t freqtable[65536];
static int attentable[256];
static int envtable[4096];
static int lfotable[4096];

static int32_t filt_w0[256];
/*static float filt_w0[256];*/

#define READ16(addr, var)       switch ((addr) & 2)                                     \
                                {                                                       \
                                        case 0: ret = (var) & 0xffff;         break;    \
                                        case 2: ret = ((var) >> 16) & 0xffff; break;    \
                                }
                                
#define WRITE16(addr, var, val) switch ((addr) & 2)                                               \
                                {                                                                 \
                                        case 0: var = (var & 0xffff0000) | (val);         break;  \
                                        case 2: var = (var & 0x0000ffff) | ((val) << 16); break;  \
                                }

static inline int16_t EMU8K_READ(emu8k_t *emu8k, uint32_t addr)
{
        addr &= 0xffffff;
        if (addr < 0x80000)
                return emu8k->rom[addr];
        if (addr < 0x200000 || addr >= 0x240000)
                return 0;
        return emu8k->ram[addr & 0x3ffff];
}

static inline int16_t EMU8K_READ_INTERP(emu8k_t *emu8k, uint32_t addr)
{
        int16_t dat1 = EMU8K_READ(emu8k, addr >> 8);
        int16_t dat2 = EMU8K_READ(emu8k, (addr >> 8) + 1);
        return ((dat1 * (0xff - (addr & 0xff))) + (dat2 * (addr & 0xff))) >> 8;
}

static inline void EMU8K_WRITE(emu8k_t *emu8k, uint32_t addr, uint16_t val)
{
        addr &= 0xffffff;
        if (addr >= 0x200000 && addr <= 0x240000)
                emu8k->ram[addr & 0x3ffff] = val;
}

uint16_t emu8k_inw(uint32_t addr, void *p)
{
        emu8k_t *emu8k = (emu8k_t *)p;
        uint16_t ret;
/*        pclog("emu8k_inw %04X  reg=%i voice=%i\n", addr, emu8k->cur_reg, emu8k->cur_voice);*/
        addr -= 0x220;
        switch (addr & 0xc02)
        {
                case 0x400: case 0x402: /*Data0*/
                switch (emu8k->cur_reg)
                {
                        case 0:
                        READ16(addr, emu8k->voice[emu8k->cur_voice].cpf);
                        return ret;
                        
                        case 1:
                        READ16(addr, emu8k->voice[emu8k->cur_voice].ptrx);
                        return ret;
                        
                        case 2:
                        READ16(addr, emu8k->voice[emu8k->cur_voice].cvcf);
                        return ret;
                        
                        case 3:
                        READ16(addr, emu8k->voice[emu8k->cur_voice].vtft);
                        return ret;
                        
                        case 4: case 5: /*???*/
                        return 0xffff;
                        
                        case 6:
                        READ16(addr, emu8k->voice[emu8k->cur_voice].psst);
                        return ret;
                                                
                        case 7:
                        READ16(addr, emu8k->voice[emu8k->cur_voice].cpf);
                        return ret;
                }
                break;

                case 0x800: /*Data1*/
                switch (emu8k->cur_reg)
                {
                        case 0:
                        {
                                uint32_t val = (emu8k->voice[emu8k->cur_voice].ccca & 0xff000000) | (emu8k->voice[emu8k->cur_voice].addr >> 8);
                                READ16(addr, emu8k->voice[emu8k->cur_voice].ccca);
                                return ret;
                        }

                        case 1:
                        switch (emu8k->cur_voice)
                        {
                                case 20:
                                READ16(addr, emu8k->smalr);
                                return ret;
                                case 21:
                                READ16(addr, emu8k->smarr);
                                return ret;
                                case 22:
                                READ16(addr, emu8k->smalw);
                                return ret;
                                case 23:
                                READ16(addr, emu8k->smarw);
                                return ret;
                                
                                case 26:
                                {
                                        uint16_t val = emu8k->smld_buffer;
                                        emu8k->smld_buffer = EMU8K_READ(emu8k, emu8k->smalr);
                                        emu8k->smalr++;
                                        return val;
                                }

                                case 29: /*Configuration Word 1*/
                                return emu8k->hwcf1;
                                case 30: /*Configuration Word 2*/
                                return emu8k->hwcf2 | 3;
                                case 31: /*Configuration Word 3*/
                                return emu8k->hwcf3;
                        }
                        break;

                        case 2: /*INIT1*/
                        case 3: /*INIT3*/
                        return 0xffff; /*Can we read anything useful from here?*/
                        
                        case 5:
                        return emu8k->voice[emu8k->cur_voice].dcysusv;

                        case 7:
                        return emu8k->voice[emu8k->cur_voice].dcysus;
                }
                break;

                case 0x802: /*Data2*/
                switch (emu8k->cur_reg)
                {
                        case 0:
                        {
                                uint32_t val = (emu8k->voice[emu8k->cur_voice].ccca & 0xff000000) | (emu8k->voice[emu8k->cur_voice].addr >> 8);
                                READ16(addr, emu8k->voice[emu8k->cur_voice].ccca);
                                return ret;
                        }

                        case 1:
                        switch (emu8k->cur_voice)
                        {
                                case 20:
                                READ16(addr, emu8k->smalr);
                                return ret;
                                case 21:
                                READ16(addr, emu8k->smarr);
                                return ret;
                                case 22:
                                READ16(addr, emu8k->smalw);
                                return ret;
                                case 23:
                                READ16(addr, emu8k->smarw);
                                return ret;
                                
                                case 26:
                                {
                                        uint16_t val = emu8k->smrd_buffer;
                                        emu8k->smrd_buffer = EMU8K_READ(emu8k, emu8k->smarr);
                                        emu8k->smarr++;
                                        return val;
                                }

                                case 27: /*Sample Counter*/
                                return emu8k->wc;
                        }
                        break;

                        case 2: /*INIT2*/
                        case 3: /*INIT4*/
                        return 0xffff; /*Can we read anything useful from here?*/
                        
                        case 4:                        
                        return emu8k->voice[emu8k->cur_voice].atkhldv;
                }
                break;
                                
                case 0xc00: /*Data3*/
                switch (emu8k->cur_reg)
                {
                        case 0:
                        return emu8k->voice[emu8k->cur_voice].ip;

                        case 1:
                        return emu8k->voice[emu8k->cur_voice].ifatn;

                        case 2:
                        return emu8k->voice[emu8k->cur_voice].pefe;

                        case 3:
                        return emu8k->voice[emu8k->cur_voice].fmmod;

                        case 4:
                        return emu8k->voice[emu8k->cur_voice].tremfrq;

                        case 5:
                        return emu8k->voice[emu8k->cur_voice].fm2frq2;

                        case 6:
                        return 0xffff;
                        
                        case 7: /*ID?*/
                        return 0xc;
                }
                break;
                case 0xc02: /*Status - I think!*/
                emu8k->c02_read ^= 0x1000;
                return emu8k->c02_read;
        }
/*        fatal("Bad EMU8K inw from %08X\n", addr);*/
        return 0xffff;
}

void emu8k_outw(uint32_t addr, uint16_t val, void *p)
{
        emu8k_t *emu8k = (emu8k_t *)p;

/*        pclog("emu8k_outw : addr=%08X reg=%i voice=%i  val=%04X\n", addr, emu8k->cur_reg, emu8k->cur_voice, val);*/
        addr -= 0x220;
        switch (addr & 0xc02)
        {
                case 0x400: case 0x402: /*Data0*/
                switch (emu8k->cur_reg)
                {
                        case 0:
                        WRITE16(addr, emu8k->voice[emu8k->cur_voice].cpf, val);
                        return;
                        
                        case 1:
                        WRITE16(addr, emu8k->voice[emu8k->cur_voice].ptrx, val);
                        return;
                        
                        case 2:
                        WRITE16(addr, emu8k->voice[emu8k->cur_voice].cvcf, val);
                        return;
                        
                        case 3:
                        WRITE16(addr, emu8k->voice[emu8k->cur_voice].vtft, val);
                        return;
                        
                        case 6:
                        WRITE16(addr, emu8k->voice[emu8k->cur_voice].psst, val);
                        emu8k->voice[emu8k->cur_voice].loop_start = (uint64_t)(emu8k->voice[emu8k->cur_voice].psst & 0xffffff) << 32;
                        if (addr & 2)
                        {
                                emu8k->voice[emu8k->cur_voice].vol_l = val >> 8;
                                emu8k->voice[emu8k->cur_voice].vol_r = 255 - (val >> 8);
                        }
/*                        pclog("emu8k_outl : write PSST %08X l %i r %i\n", emu8k->voice[emu8k->cur_voice].psst, emu8k->voice[emu8k->cur_voice].vol_l, emu8k->voice[emu8k->cur_voice].vol_r);*/
                        return;
                        
                        case 7:
                        WRITE16(addr, emu8k->voice[emu8k->cur_voice].cpf, val);
                        emu8k->voice[emu8k->cur_voice].loop_end = (uint64_t)(emu8k->voice[emu8k->cur_voice].cpf & 0xffffff) << 32;
/*                        pclog("emu8k_outl : write CPF %08X\n", emu8k->voice[emu8k->cur_voice].cpf);*/
                        return;
                }
                break;

                case 0x800: /*Data1*/
                switch (emu8k->cur_reg)
                {
                        case 0:
                        WRITE16(addr, emu8k->voice[emu8k->cur_voice].ccca, val);
                        emu8k->voice[emu8k->cur_voice].addr = (uint64_t)(emu8k->voice[emu8k->cur_voice].ccca & 0xffffff) << 32;
/*                        pclog("emu8k_outl : write CCCA %08X\n", emu8k->voice[emu8k->cur_voice].ccca);*/
                        return;

                        case 1:
                        switch (emu8k->cur_voice)
                        {
                                case 20:
                                WRITE16(addr, emu8k->smalr, val);
                                return;
                                case 21:
                                WRITE16(addr, emu8k->smarr, val);
                                return;
                                case 22:
                                WRITE16(addr, emu8k->smalw, val);
                                return;
                                case 23:
                                WRITE16(addr, emu8k->smarw, val);
                                return;
                                
                                case 26:
                                EMU8K_WRITE(emu8k, emu8k->smalw, val);
                                emu8k->smalw++;
                                break;

                                case 29: /*Configuration Word 1*/
                                emu8k->hwcf1 = val;
                                return;
                                case 30: /*Configuration Word 2*/
                                emu8k->hwcf2 = val;
                                return;
                                case 31: /*Configuration Word 3*/
                                emu8k->hwcf3 = val;
                                return;
                        }
                        break;

                        case 5:
/*                        pclog("emu8k_outw : write DCYSUSV %04X\n", val);*/
                        emu8k->voice[emu8k->cur_voice].dcysusv = val;
                        emu8k->voice[emu8k->cur_voice].env_sustain = (((val >> 8) & 0x7f) << 5) << 9;
                        if (val & 0x8000) /*Release*/
                        {
                                emu8k->voice[emu8k->cur_voice].env_state = ENV_RELEASE;
                                emu8k->voice[emu8k->cur_voice].env_release = val & 0x7f;
                        }
                        else    /*Decay*/
                                emu8k->voice[emu8k->cur_voice].env_decay = val & 0x7f;
                        if (val & 0x80)
                                emu8k->voice[emu8k->cur_voice].env_state = ENV_STOPPED;
                        return;

                        case 7:
/*                        pclog("emu8k_outw : write DCYSUS %04X\n", val);*/
                        emu8k->voice[emu8k->cur_voice].dcysus = val;
                        emu8k->voice[emu8k->cur_voice].menv_sustain = (((val >> 8) & 0x7f) << 5) << 9;
                        if (val & 0x8000) /*Release*/
                        {
                                emu8k->voice[emu8k->cur_voice].menv_state = ENV_RELEASE;
                                emu8k->voice[emu8k->cur_voice].menv_release = val & 0x7f;
                        }
                        else    /*Decay*/
                                emu8k->voice[emu8k->cur_voice].menv_decay = val & 0x7f;
                        if (val & 0x80)
                                emu8k->voice[emu8k->cur_voice].menv_state = ENV_STOPPED;
                        return;
                }
                break;
                
                case 0x802: /*Data2*/
                switch (emu8k->cur_reg)
                {
                        case 0:
                        {
                                float q;
                                
                                WRITE16(addr, emu8k->voice[emu8k->cur_voice].ccca, val);
                                emu8k->voice[emu8k->cur_voice].addr = (uint64_t)(emu8k->voice[emu8k->cur_voice].ccca & 0xffffff) << 32;

                                q = (float)(emu8k->voice[emu8k->cur_voice].ccca >> 28) / 15.0f;
                                q /= 10.0f; /*Horrible and wrong hack*/
                                emu8k->voice[emu8k->cur_voice].q = (int32_t)((1.0f / (0.707f + q)) * 256.0f);

/*                                pclog("emu8k_outl : write CCCA %08X Q %f invQ %X\n", emu8k->voice[emu8k->cur_voice].ccca, q, emu8k->voice[emu8k->cur_voice].q);*/
                        }
                        return;

                        case 1:
                        switch (emu8k->cur_voice)
                        {
                                case 20:
                                WRITE16(addr, emu8k->smalr, val);
                                return;
                                case 21:
                                WRITE16(addr, emu8k->smarr, val);
                                return;
                                case 22:
                                WRITE16(addr, emu8k->smalw, val);
                                return;
                                case 23:
                                WRITE16(addr, emu8k->smarw, val);
                                return;

                                case 26:
                                EMU8K_WRITE(emu8k, emu8k->smarw, val);
                                emu8k->smarw++;
                                break;
                        }
                        break;

                        case 4:
/*                        pclog("emu8k_outw : write ATKHLDV %04X\n", val);*/
                        emu8k->voice[emu8k->cur_voice].atkhldv = val;
                        emu8k->voice[emu8k->cur_voice].env_attack = (val & 0x7f) << 6;
                        if (!(val & 0x8000)) /*Trigger attack*/
                                emu8k->voice[emu8k->cur_voice].env_state = ENV_ATTACK;
                        return;

                        case 6:
/*                        pclog("emu8k_outw : write ATKHLD %04X\n", val);*/
                        emu8k->voice[emu8k->cur_voice].atkhld = val;
                        emu8k->voice[emu8k->cur_voice].menv_attack = (val & 0x7f) << 6;
                        if (!(val & 0x8000)) /*Trigger attack*/
                                emu8k->voice[emu8k->cur_voice].menv_state = ENV_ATTACK;
                        return;
                }
                break;
                
                case 0xc00: /*Data3*/
                switch (emu8k->cur_reg)
                {
                        case 0:
                        emu8k->voice[emu8k->cur_voice].ip = val;
                        emu8k->voice[emu8k->cur_voice].pitch = val;
                        return;
                        
                        case 1:
                        emu8k->voice[emu8k->cur_voice].ifatn = val;
                        emu8k->voice[emu8k->cur_voice].attenuation = attentable[val & 0xff];
                        emu8k->voice[emu8k->cur_voice].cutoff = (val >> 8);
/*                        pclog("Attenuation now %02X %i\n", val & 0xff, emu8k->voice[emu8k->cur_voice].attenuation);*/
                        return;

                        case 2:
                        emu8k->voice[emu8k->cur_voice].pefe = val;
                        emu8k->voice[emu8k->cur_voice].fe_height = (int8_t)(val & 0xff);
                        return;

                        case 3:
                        emu8k->voice[emu8k->cur_voice].fmmod = val;
                        emu8k->voice[emu8k->cur_voice].lfo1_fmmod = (val >> 8);
                        return;

                        case 4:
                        emu8k->voice[emu8k->cur_voice].tremfrq = val;
                        emu8k->voice[emu8k->cur_voice].lfo1_trem = (val >> 8);
                        return;

                        case 5:
                        emu8k->voice[emu8k->cur_voice].fm2frq2 = val;
                        emu8k->voice[emu8k->cur_voice].lfo2_fmmod = (val >> 8);
                        return;
                }
                break;
                
                case 0xc02: /*Pointer*/
                emu8k->cur_voice = (val & 31);
                emu8k->cur_reg   = ((val >> 5) & 7);
                return;
        }
}

void emu8k_poll(void *p)
{
        emu8k_t *emu8k = (emu8k_t *)p;
        int c;
        int32_t out_l = 0, out_r = 0;
        
        emu8k->timer_count += (int)(TIMER_USEC * (1000000.0 / 44100.0));
        
        for (c = 0; c < 32; c++)
        {
                int32_t voice_l, voice_r;
                int32_t dat;
                int lfo1_vibrato, lfo2_vibrato;
                int tremolo;
                
                tremolo = ((lfotable[(emu8k->voice[c].lfo1_count >> 8) & 4095] * emu8k->voice[c].lfo1_trem) * 4) >> 12;

                if (freqtable[emu8k->voice[c].pitch] >> 32)
                        dat = EMU8K_READ(emu8k, emu8k->voice[c].addr >> 32);
                else
                        dat = EMU8K_READ_INTERP(emu8k, emu8k->voice[c].addr >> 24);

                dat = (dat * emu8k->voice[c].attenuation) >> 16;

                dat = (dat * envtable[emu8k->voice[c].env_vol >> 9]) >> 16;
                        
                if ((emu8k->voice[c].ccca >> 28) || (emu8k->voice[c].cutoff != 0xff))
                {
                        int cutoff = emu8k->voice[c].cutoff + ((emu8k->voice[c].menv_vol * emu8k->voice[c].fe_height) >> 20);
                        if (cutoff < 0)
                                cutoff = 0;
                        if (cutoff > 255)
                                cutoff = 255;

                        emu8k->voice[c].vhp = ((-emu8k->voice[c].vbp * emu8k->voice[c].q) >> 8) - emu8k->voice[c].vlp - dat;
                        emu8k->voice[c].vlp += (emu8k->voice[c].vbp * filt_w0[cutoff]) >> 8;
                        emu8k->voice[c].vbp += (emu8k->voice[c].vhp * filt_w0[cutoff]) >> 8;
                        if (emu8k->voice[c].vlp < -32767)
                                dat = -32767;
                        else if (emu8k->voice[c].vlp > 32767)
                                dat = 32767;
                        else
                                dat = (int16_t)emu8k->voice[c].vlp;
                }
                        
                voice_l = (dat * emu8k->voice[c].vol_l) >> 7;
                voice_r = (dat * emu8k->voice[c].vol_r) >> 7;
                        
                out_l += voice_l * 8192;
                out_r += voice_r * 8192;

                switch (emu8k->voice[c].env_state)
                {
                        case ENV_ATTACK:
                        emu8k->voice[c].env_vol += emu8k->voice[c].env_attack;
                        if (emu8k->voice[c].env_vol >= (1 << 21))
                        {
                                emu8k->voice[c].env_vol = 1 << 21;
                                emu8k->voice[c].env_state = ENV_DECAY;
                        }
                        break;
                        
                        case ENV_DECAY:
                        emu8k->voice[c].env_vol -= emu8k->voice[c].env_decay;
                        if (emu8k->voice[c].env_vol <= emu8k->voice[c].env_sustain)
                        {
                                emu8k->voice[c].env_vol = emu8k->voice[c].env_sustain;
                                emu8k->voice[c].env_state = ENV_SUSTAIN;
                        }
                        break;

                        case ENV_RELEASE:
                        emu8k->voice[c].env_vol -= emu8k->voice[c].env_release;
                        if (emu8k->voice[c].env_vol <= 0)
                        {
                                emu8k->voice[c].env_vol = 0;
                                emu8k->voice[c].env_state = ENV_STOPPED;
                        }
                        break;
                }

                switch (emu8k->voice[c].menv_state)
                {
                        case ENV_ATTACK:
                        emu8k->voice[c].menv_vol += emu8k->voice[c].menv_attack;
                        if (emu8k->voice[c].menv_vol >= (1 << 21))
                        {
                                emu8k->voice[c].menv_vol = 1 << 21;
                                emu8k->voice[c].menv_state = ENV_DECAY;
                        }
                        break;
                        
                        case ENV_DECAY:
                        emu8k->voice[c].menv_vol -= emu8k->voice[c].menv_decay;
                        if (emu8k->voice[c].menv_vol <= emu8k->voice[c].menv_sustain)
                        {
                                emu8k->voice[c].menv_vol = emu8k->voice[c].menv_sustain;
                                emu8k->voice[c].menv_state = ENV_SUSTAIN;
                        }
                        break;

                        case ENV_RELEASE:
                        emu8k->voice[c].menv_vol -= emu8k->voice[c].menv_release;
                        if (emu8k->voice[c].menv_vol <= 0)
                        {
                                emu8k->voice[c].menv_vol = 0;
                                emu8k->voice[c].menv_state = ENV_STOPPED;
                        }
                        break;
                }

                lfo1_vibrato = (lfotable[(emu8k->voice[c].lfo1_count >> 8) & 4095] * emu8k->voice[c].lfo1_fmmod) >> 9;
                lfo2_vibrato = (lfotable[(emu8k->voice[c].lfo2_count >> 8) & 4095] * emu8k->voice[c].lfo2_fmmod) >> 9;
                                
                emu8k->voice[c].addr += freqtable[(emu8k->voice[c].pitch + lfo1_vibrato + lfo2_vibrato) & 0xffff];
                if (emu8k->voice[c].addr >= emu8k->voice[c].loop_end)
                        emu8k->voice[c].addr -= (emu8k->voice[c].loop_end - emu8k->voice[c].loop_start);

                emu8k->voice[c].lfo1_count += (emu8k->voice[c].tremfrq & 0xff);
                emu8k->voice[c].lfo2_count += (emu8k->voice[c].fm2frq2 & 0xff);
        }
        
        emu8k->out_l = out_l >> 16;
        emu8k->out_r = out_r >> 16;
        
        emu8k->wc++;
}

void emu8k_poll_getsamp(emu8k_t *emu8k, int16_t *l, int16_t *r)
{
        *l = emu8k->out_l;
        *r = emu8k->out_r;
}

void emu8k_init(emu8k_t *emu8k)
{
        FILE *f;
        int c;
        double out;
        
        f = romfopen("roms/awe32.raw", "rb");
        if (!f)
                fatal("ROMS/AWE32.RAW not found\n");
                
        emu8k->ram = malloc(512 * 1024);
        emu8k->rom = malloc(1024 * 1024);        
        
        fread(emu8k->rom, 1024 * 1024, 1, f);
        fclose(f);
        
        io_sethandler(0x0620, 0x0004, NULL, emu8k_inw, NULL, NULL, emu8k_outw, NULL, emu8k);
        io_sethandler(0x0a20, 0x0004, NULL, emu8k_inw, NULL, NULL, emu8k_outw, NULL, emu8k);
        io_sethandler(0x0e20, 0x0004, NULL, emu8k_inw, NULL, NULL, emu8k_outw, NULL, emu8k);

        timer_add(emu8k_poll, &emu8k->timer_count, TIMER_ALWAYS_ENABLED, emu8k);
                        
        /*Create frequency table*/
        for (c = 0; c < 0x10000; c++)
        {
                freqtable[c] = (uint64_t)(exp2((double)(c - 0xe000) / 4096.0) * 65536.0 * 65536.0);
        }

        out = 65536.0;
        
        for (c = 0; c < 256; c++)
        {
                attentable[c] = (int)out;     
                out /= sqrt(1.09018); /*0.375 dB steps*/
        }
        
        out = 65536;
        
        for (c = 0; c < 4096; c++)
        {
                envtable[4095 - c] = (int)out;
                out /= 1.002709201; /*0.0235 dB Steps*/
        }
        
        for (c = 0; c < 4096; c++)
        {
                int d = (c + 1024) & 4095;
                if (d >= 2048)
                        lfotable[c] = 4096 - ((2048 - d) * 4);
                else
                        lfotable[c] = (d * 4) - 4096;
        }

        out = 125.0;
        for (c = 0; c < 256; c++)
        {
/*                filt_w0[c] = (int32_t)((2.0 * 3.142 * (out / 44100.0)) * 0.707 * 256.0);*/
/*                filt_w0[c] = 2.0 * 3.142 * (out / 44100.0);*/
                filt_w0[c] = (int32_t)(2.0 * 3.142 * (out / 44100.0) * 256.0);
                out *= 1.016378315;
        }
        
        emu8k->hwcf1 = 0x59;
        emu8k->hwcf2 = 0x03;
}
