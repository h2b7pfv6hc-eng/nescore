/*
 * nescore for Wii - Homebrew Channel frontend.
 *
 * ROMs:   sd:/nes/ (or usb:/nes/), .nes files
 * Saves:  <rom folder>/saves/  (.sav battery RAM, .st0 savestate)
 * Config: <rom folder>/nescore.cfg
 *
 * Controls (in game)
 *   Wii Remote sideways: D-pad, 2 = A, 1 = B, + = Start, - = Select
 *   Wii Remote + Nunchuk: stick/D-pad, A = A, B = B
 *   Classic / Wii U Pro: D-pad/left stick, A/X = A, B/Y = B
 *   GameCube: D-pad/stick, A = A, B = B, Start, Z = Select
 *   Menu: HOME (Wii Remote/Classic) or L+R (GameCube)
 */
#include <gccore.h>
#include <wiiuse/wpad.h>
#include <ogc/lwp_watchdog.h>
#include <fat.h>
#include <dirent.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "nes.h"

#define SAMPLE_RATE 48000
#define FIFO_SIZE (256 * 1024)
#define MENU_W 512
#define MENU_H 480

extern u8 console_font_8x16[];

/* ------------------------------------------------------------------ */
/* video                                                               */

static GXRModeObj *rmode;
static void *xfb[2];
static int xfb_idx;
static void *gp_fifo;
static u16 *game_tex;   /* 256x240 RGB565, 4x4 tiled */
static u16 *menu_tex;   /* 512x480 RGB565, 4x4 tiled */
static u16 *menu_lin;   /* 512x480 RGB565, linear (drawn into, then tiled) */
static GXTexObj game_obj, menu_obj;
static u16 pal565[512];
static int widescreen;

static struct {
    int runahead;    /* 0..2 frames */
    int smooth;      /* bilinear filtering */
    int crop;        /* hide the top/bottom 8 lines like a CRT */
    int stretch;     /* 0 = 8:7, 1 = 4:3 stretch, 2 = sharp 2x */
    int region;      /* 0 auto (file name / header), 1 NTSC, 2 PAL */
} cfg = {0, 0, 1, 2, 0};

/* Output modes: NTSC games are shown at 60 Hz, PAL games at 50 Hz (on a
 * PAL console), so every game runs at its own speed, locked to the TV. */
static GXRModeObj *mode60, *mode50;
static float scr_w = 640, scr_h = 480;
static double display_hz = 59.94;

static void video_apply(GXRModeObj *m)
{
    rmode = m;
    /* keep viWidth == fbWidth: any other value makes the video interface
     * rescale every line horizontally, which blurs the pixels */
    display_hz = ((rmode->viTVMode >> 2) == VI_PAL) ? 50.0 : 59.94;
    VIDEO_Configure(rmode);
    VIDEO_ClearFrameBuffer(rmode, xfb[0], COLOR_BLACK);
    VIDEO_ClearFrameBuffer(rmode, xfb[1], COLOR_BLACK);
    VIDEO_SetNextFramebuffer(xfb[xfb_idx]);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    if (rmode->viTVMode & VI_NON_INTERLACE)
        VIDEO_WaitVSync();
    if (!gp_fifo)
        return;
    GX_SetViewport(0, 0, rmode->fbWidth, rmode->efbHeight, 0, 1);
    f32 yscale = GX_GetYScaleFactor(rmode->efbHeight, rmode->xfbHeight);
    u32 xfb_h = GX_SetDispCopyYScale(yscale);
    GX_SetScissor(0, 0, rmode->fbWidth, rmode->efbHeight);
    GX_SetDispCopySrc(0, 0, rmode->fbWidth, rmode->efbHeight);
    GX_SetDispCopyDst(rmode->fbWidth, xfb_h);
    GX_SetCopyFilter(rmode->aa, rmode->sample_pattern, GX_FALSE, rmode->vfilter); /* no deflicker blur */
    GX_SetFieldMode(rmode->field_rendering,
                    (rmode->viHeight == 2 * rmode->xfbHeight) ? GX_ENABLE : GX_DISABLE);
    GX_SetPixelFmt(rmode->aa ? GX_PF_RGB565_Z16 : GX_PF_RGB8_Z24, GX_ZC_LINEAR);
    /* draw in real framebuffer pixels (640x480, or 640x528 for PAL) */
    scr_w = rmode->fbWidth;
    scr_h = rmode->efbHeight;
    Mtx44 proj;
    guOrtho(proj, 0, scr_h, 0, scr_w, 0, 1);
    GX_LoadProjectionMtx(proj, GX_ORTHOGRAPHIC);
}

static void video_init(void)
{
    VIDEO_Init();
    GXRModeObj *pref = VIDEO_GetPreferredMode(NULL);
    int fmt = pref->viTVMode >> 2;
    int console_pal = (fmt == VI_PAL || fmt == VI_EURGB60);
    int progressive = (pref->viTVMode & 3) == VI_PROGRESSIVE;
    if (console_pal) {
        mode60 = progressive ? &TVEurgb60Hz480Prog : &TVEurgb60Hz480IntDf;
        mode50 = &TVPal528IntDf;
    } else {
        mode60 = pref;
        mode50 = pref; /* NTSC consoles cannot output 50 Hz */
    }
    widescreen = CONF_GetAspectRatio() == CONF_ASPECT_16_9;

    /* framebuffers big enough for every mode we may switch to */
    xfb[0] = MEM_K0_TO_K1(SYS_AllocateFramebuffer(&TVPal576IntDfScale));
    xfb[1] = MEM_K0_TO_K1(SYS_AllocateFramebuffer(&TVPal576IntDfScale));
    gp_fifo = memalign(32, FIFO_SIZE);
    memset(gp_fifo, 0, FIFO_SIZE);
    GX_Init(gp_fifo, FIFO_SIZE);
    GXColor black = {0, 0, 0, 0xFF};
    GX_SetCopyClear(black, 0x00FFFFFF);
    video_apply(mode60);
    VIDEO_SetBlack(false);
    VIDEO_Flush();
    GX_SetCullMode(GX_CULL_NONE);
    GX_SetDispCopyGamma(GX_GM_1_0);
    GX_CopyDisp(xfb[0], GX_TRUE);

    GX_ClearVtxDesc();
    GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
    GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XY, GX_F32, 0);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
    GX_SetNumChans(0);
    GX_SetNumTexGens(1);
    GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
    GX_SetNumTevStages(1);
    GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLORNULL);
    GX_SetTevOp(GX_TEVSTAGE0, GX_REPLACE);
    GX_SetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
    GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
    GX_SetAlphaUpdate(GX_TRUE);
    GX_SetColorUpdate(GX_TRUE);

    Mtx mv;
    guMtxIdentity(mv);
    GX_LoadPosMtxImm(mv, GX_PNMTX0);

    game_tex = memalign(32, 256 * 240 * 2);
    menu_tex = memalign(32, MENU_W * MENU_H * 2);
    menu_lin = memalign(32, MENU_W * MENU_H * 2);

    uint32_t rgb[512];
    nes_palette_rgb(rgb);
    for (int i = 0; i < 512; i++) {
        u32 c = rgb[i];
        pal565[i] = (u16)(((c >> 8) & 0xF800) | ((c >> 5) & 0x07E0) | ((c >> 3) & 0x001F));
    }
}

static void tex_setup(GXTexObj *obj, void *data, int w, int h, int smooth)
{
    GX_InitTexObj(obj, data, w, h, GX_TF_RGB565, GX_CLAMP, GX_CLAMP, GX_FALSE);
    u8 f = smooth ? GX_LINEAR : GX_NEAR;
    GX_InitTexObjLOD(obj, f, f, 0, 0, 0, GX_FALSE, GX_FALSE, GX_ANISO_1);
}

static void draw_quad(GXTexObj *obj, float x0, float y0, float x1, float y1,
                      float s0, float t0, float s1, float t1)
{
    GX_LoadTexObj(obj, GX_TEXMAP0);
    GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
    GX_Position2f32(x0, y0); GX_TexCoord2f32(s0, t0);
    GX_Position2f32(x1, y0); GX_TexCoord2f32(s1, t0);
    GX_Position2f32(x1, y1); GX_TexCoord2f32(s1, t1);
    GX_Position2f32(x0, y1); GX_TexCoord2f32(s0, t1);
    GX_End();
}

static void present(void)
{
    GX_DrawDone();
    xfb_idx ^= 1;
    GX_CopyDisp(xfb[xfb_idx], GX_TRUE);
    GX_DrawDone();
    VIDEO_SetNextFramebuffer(xfb[xfb_idx]);
    VIDEO_Flush();
    VIDEO_WaitVSync();
}

/* Convert the core's palette-index frame into a tiled RGB565 texture. */
static void upload_game(const uint16_t *fb)
{
    u16 *dst = game_tex;
    for (int ty = 0; ty < 240; ty += 4) {
        for (int tx = 0; tx < 256; tx += 4) {
            const uint16_t *src = fb + ty * 256 + tx;
            for (int y = 0; y < 4; y++, src += 256) {
                dst[0] = pal565[src[0]];
                dst[1] = pal565[src[1]];
                dst[2] = pal565[src[2]];
                dst[3] = pal565[src[3]];
                dst += 4;
            }
        }
    }
    DCFlushRange(game_tex, 256 * 240 * 2);
    GX_InvalidateTexAll();
}

static void draw_game(void)
{
    tex_setup(&game_obj, game_tex, 256, 240, cfg.smooth);
    /* Every NES line is exactly 2 framebuffer lines; nothing is scaled
     * by a fractional amount vertically.
     * Width: sharp = exactly 2 framebuffer pixels per NES pixel (512);
     * 8:7 = the shape a real NES has on a 4:3 TV (585); 4:3 = full (640). */
    float w = cfg.stretch == 2 ? 512.f : cfg.stretch == 1 ? 640.f : 585.f;
    if (widescreen && cfg.stretch != 2)
        w *= 0.75f;
    float x0 = (float)(int)((scr_w - w) / 2.f);
    float y0 = (float)(int)((scr_h - 480.f) / 2.f);
    float t0 = 0.f, t1 = 1.f, top = y0, bottom = y0 + 480.f;
    if (cfg.crop) { /* hide 8 lines top and bottom, keep the 2x scale */
        t0 = 8.f / 240.f;
        t1 = 232.f / 240.f;
        top += 16.f;
        bottom -= 16.f;
    }
    draw_quad(&game_obj, x0, top, x0 + w, bottom, 0, t0, 1, t1);
}

/* ------------------------------------------------------------------ */
/* menu drawing (8x16 console font into a 512x480 texture)              */

#define C_BG     0x0841
#define C_TEXT   0xDEFB
#define C_DIM    0x8410
#define C_HI     0xFFE0
#define C_BAR    0x2945
#define C_ACCENT 0xF9A6

static void m_clear(void)
{
    for (int i = 0; i < MENU_W * MENU_H; i++)
        menu_lin[i] = C_BG;
}

static void m_rect(int x, int y, int w, int h, u16 c)
{
    for (int j = y; j < y + h && j < MENU_H; j++)
        for (int i = x; i < x + w && i < MENU_W; i++)
            menu_lin[j * MENU_W + i] = c;
}

static void m_text(int col, int row, const char *s, u16 color)
{
    int x0 = 16 + col * 8, y0 = 16 + row * 16;
    for (; *s && x0 + 8 <= MENU_W - 8; s++, x0 += 8) {
        const u8 *g = &console_font_8x16[(u8)*s * 16];
        for (int y = 0; y < 16; y++)
            for (int x = 0; x < 8; x++)
                if (g[y] & (0x80 >> x))
                    menu_lin[(y0 + y) * MENU_W + x0 + x] = color;
    }
}

static void m_show(void)
{
    u16 *dst = menu_tex;
    for (int ty = 0; ty < MENU_H; ty += 4)
        for (int tx = 0; tx < MENU_W; tx += 4)
            for (int y = 0; y < 4; y++) {
                const u16 *src = menu_lin + (ty + y) * MENU_W + tx;
                dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2]; dst[3] = src[3];
                dst += 4;
            }
    DCFlushRange(menu_tex, MENU_W * MENU_H * 2);
    GX_InvalidateTexAll();
    tex_setup(&menu_obj, menu_tex, MENU_W, MENU_H, 1);
    float w = widescreen ? scr_w * 0.75f : scr_w;
    float x0 = (scr_w - w) / 2.f;
    float y0 = (scr_h - 480.f) / 2.f;
    draw_quad(&menu_obj, x0, y0, x0 + w, y0 + 480.f, 0, 0, 1, 1);
    present();
}

/* ------------------------------------------------------------------ */
/* audio: 48 kHz stereo DMA fed from a lock-free ring                   */

#define DMA_FRAMES 512
#define RING 8192
static u32 dma_buf[2][DMA_FRAMES] ATTRIBUTE_ALIGN(32);
static int dma_cur;
static s16 ring[RING];
static volatile u32 ring_r, ring_w;
static s16 last_sample;

static void audio_cb(void)
{
    u32 *b = dma_buf[dma_cur];
    u32 r = ring_r, w = ring_w;
    for (int i = 0; i < DMA_FRAMES; i++) {
        if (r != w) {
            last_sample = ring[r & (RING - 1)];
            r++;
        }
        u16 s = (u16)last_sample;
        b[i] = ((u32)s << 16) | s;
    }
    ring_r = r;
    DCFlushRange(b, sizeof(dma_buf[0]));
    AUDIO_InitDMA((u32)b, sizeof(dma_buf[0]));
    dma_cur ^= 1;
}

static void audio_init(void)
{
    AUDIO_Init(NULL);
    AUDIO_SetDSPSampleRate(AI_SAMPLERATE_48KHZ);
    memset(dma_buf, 0, sizeof(dma_buf));
    DCFlushRange(dma_buf, sizeof(dma_buf));
    AUDIO_RegisterDMACallback(audio_cb);
    AUDIO_InitDMA((u32)dma_buf[0], sizeof(dma_buf[0]));
    AUDIO_StartDMA();
}

static void audio_push(nes_t *n)
{
    static s16 tmp[2048];
    int c;
    while ((c = nes_audio_read(n, tmp, 2048)) > 0) {
        for (int i = 0; i < c; i++) {
            if (ring_w - ring_r >= RING)
                break;
            ring[ring_w & (RING - 1)] = tmp[i];
            ring_w++;
        }
    }
    /* Dynamic rate control: the TV runs at 59.94/50 Hz, the NES at 60.0988/50.007 Hz.
     * Nudge the core's output rate (max +-0.5%) to keep the ring ~2 DMA
     * buffers full, which hides the mismatch without audible pitch change. */
    int fill = (int)(ring_w - ring_r);
    const int target = DMA_FRAMES * 3;
    double base = SAMPLE_RATE * nes_fps(n) / display_hz;
    double adj = (double)(target - fill) / target * 0.005;
    if (adj > 0.005) adj = 0.005;
    if (adj < -0.005) adj = -0.005;
    nes_set_audio_rate(n, base * (1.0 + adj));
}

/* ------------------------------------------------------------------ */
/* input                                                               */

enum { K_UP = 1, K_DOWN = 2, K_LEFT = 4, K_RIGHT = 8, K_OK = 16, K_BACK = 32, K_MENU = 64 };

static int stick_dir(float ang, float mag)
{
    if (mag < 0.45f)
        return 0;
    int d = 0;
    if (ang >= 300 || ang <= 60) d |= NES_UP;
    if (ang >= 120 && ang <= 240) d |= NES_DOWN;
    if (ang >= 30 && ang <= 150) d |= NES_RIGHT;
    if (ang >= 210 && ang <= 330) d |= NES_LEFT;
    return d;
}

static int gc_stick_dir(int x, int y)
{
    int d = 0;
    if (y > 40) d |= NES_UP;
    if (y < -40) d |= NES_DOWN;
    if (x > 40) d |= NES_RIGHT;
    if (x < -40) d |= NES_LEFT;
    return d;
}

static int menu_requested;

static u8 read_pad(int chan)
{
    u8 b = 0;
    u32 h = WPAD_ButtonsHeld(chan);
    WPADData *wd = WPAD_Data(chan);
    int exp = (wd && wd->err == WPAD_ERR_NONE) ? wd->exp.type : WPAD_EXP_NONE;

    if (exp == WPAD_EXP_CLASSIC) {
        if (h & WPAD_CLASSIC_BUTTON_UP) b |= NES_UP;
        if (h & WPAD_CLASSIC_BUTTON_DOWN) b |= NES_DOWN;
        if (h & WPAD_CLASSIC_BUTTON_LEFT) b |= NES_LEFT;
        if (h & WPAD_CLASSIC_BUTTON_RIGHT) b |= NES_RIGHT;
        if (h & (WPAD_CLASSIC_BUTTON_A | WPAD_CLASSIC_BUTTON_X)) b |= NES_A;
        if (h & (WPAD_CLASSIC_BUTTON_B | WPAD_CLASSIC_BUTTON_Y)) b |= NES_B;
        if (h & WPAD_CLASSIC_BUTTON_PLUS) b |= NES_START;
        if (h & WPAD_CLASSIC_BUTTON_MINUS) b |= NES_SELECT;
        b |= (u8)stick_dir(wd->exp.classic.ljs.ang, wd->exp.classic.ljs.mag);
    } else if (exp == WPAD_EXP_NUNCHUK) {
        if (h & WPAD_BUTTON_UP) b |= NES_UP;
        if (h & WPAD_BUTTON_DOWN) b |= NES_DOWN;
        if (h & WPAD_BUTTON_LEFT) b |= NES_LEFT;
        if (h & WPAD_BUTTON_RIGHT) b |= NES_RIGHT;
        if (h & WPAD_BUTTON_A) b |= NES_A;
        if (h & WPAD_BUTTON_B) b |= NES_B;
        b |= (u8)stick_dir(wd->exp.nunchuk.js.ang, wd->exp.nunchuk.js.mag);
    } else {
        /* held sideways, D-pad on the left */
        if (h & WPAD_BUTTON_RIGHT) b |= NES_UP;
        if (h & WPAD_BUTTON_LEFT) b |= NES_DOWN;
        if (h & WPAD_BUTTON_UP) b |= NES_LEFT;
        if (h & WPAD_BUTTON_DOWN) b |= NES_RIGHT;
        if (h & WPAD_BUTTON_2) b |= NES_A;
        if (h & WPAD_BUTTON_1) b |= NES_B;
    }
    if (h & WPAD_BUTTON_PLUS) b |= NES_START;
    if (h & WPAD_BUTTON_MINUS) b |= NES_SELECT;
    if (h & (WPAD_BUTTON_HOME | WPAD_CLASSIC_BUTTON_HOME))
        menu_requested = 1;

    u16 g = PAD_ButtonsHeld(chan);
    if (g & PAD_BUTTON_UP) b |= NES_UP;
    if (g & PAD_BUTTON_DOWN) b |= NES_DOWN;
    if (g & PAD_BUTTON_LEFT) b |= NES_LEFT;
    if (g & PAD_BUTTON_RIGHT) b |= NES_RIGHT;
    if (g & PAD_BUTTON_A) b |= NES_A;
    if (g & PAD_BUTTON_B) b |= NES_B;
    if (g & PAD_BUTTON_START) b |= NES_START;
    if (g & PAD_TRIGGER_Z) b |= NES_SELECT;
    if ((g & PAD_TRIGGER_L) && (g & PAD_TRIGGER_R))
        menu_requested = 1;
    b |= (u8)gc_stick_dir(PAD_StickX(chan), PAD_StickY(chan));
    return b;
}

/* Menu navigation with key repeat; works with every controller. */
static int menu_keys(void)
{
    static int held_prev, repeat;
    WPAD_ScanPads();
    PAD_ScanPads();
    int k = 0;
    for (int c = 0; c < 4; c++) {
        u32 h = WPAD_ButtonsHeld(c);
        WPADData *wd = WPAD_Data(c);
        int exp = (wd && wd->err == WPAD_ERR_NONE) ? wd->exp.type : WPAD_EXP_NONE;
        if (exp == WPAD_EXP_NONE) {
            /* menus are used with the remote pointing at the screen */
            if (h & WPAD_BUTTON_UP) k |= K_UP;
            if (h & WPAD_BUTTON_DOWN) k |= K_DOWN;
            if (h & WPAD_BUTTON_LEFT) k |= K_LEFT;
            if (h & WPAD_BUTTON_RIGHT) k |= K_RIGHT;
            if (h & (WPAD_BUTTON_A | WPAD_BUTTON_2)) k |= K_OK;
            if (h & (WPAD_BUTTON_B | WPAD_BUTTON_1)) k |= K_BACK;
        } else {
            if (h & (WPAD_BUTTON_UP | WPAD_CLASSIC_BUTTON_UP)) k |= K_UP;
            if (h & (WPAD_BUTTON_DOWN | WPAD_CLASSIC_BUTTON_DOWN)) k |= K_DOWN;
            if (h & (WPAD_BUTTON_LEFT | WPAD_CLASSIC_BUTTON_LEFT)) k |= K_LEFT;
            if (h & (WPAD_BUTTON_RIGHT | WPAD_CLASSIC_BUTTON_RIGHT)) k |= K_RIGHT;
            if (h & (WPAD_BUTTON_A | WPAD_CLASSIC_BUTTON_A)) k |= K_OK;
            if (h & (WPAD_BUTTON_B | WPAD_CLASSIC_BUTTON_B)) k |= K_BACK;
        }
        if (h & (WPAD_BUTTON_HOME | WPAD_CLASSIC_BUTTON_HOME)) k |= K_MENU;
        u16 g = PAD_ButtonsHeld(c);
        int sy = PAD_StickY(c);
        if ((g & PAD_BUTTON_UP) || sy > 50) k |= K_UP;
        if ((g & PAD_BUTTON_DOWN) || sy < -50) k |= K_DOWN;
        if (g & PAD_BUTTON_LEFT) k |= K_LEFT;
        if (g & PAD_BUTTON_RIGHT) k |= K_RIGHT;
        if (g & PAD_BUTTON_A) k |= K_OK;
        if (g & PAD_BUTTON_B) k |= K_BACK;
        if ((g & PAD_TRIGGER_L) && (g & PAD_TRIGGER_R)) k |= K_MENU;
    }
    int pressed = k & ~held_prev;
    if (k && k == held_prev) {
        if (++repeat > 18 && (repeat & 3) == 0)
            pressed = k & (K_UP | K_DOWN | K_LEFT | K_RIGHT);
    } else {
        repeat = 0;
    }
    held_prev = k;
    return pressed;
}

/* ------------------------------------------------------------------ */
/* files                                                               */

static char rom_dir[256] = "sd:/nes";
static char rom_path[512];
static char rom_name[256];
static nes_t *nes;
static int quit_requested;

static void power_cb(void) { quit_requested = 2; }
static void wpad_power_cb(s32 chan) { (void)chan; quit_requested = 2; }
static void reset_cb(u32 irq, void *ctx) { (void)irq; (void)ctx; quit_requested = 1; }

static void make_path(char *out, size_t sz, const char *ext)
{
    char base[256];
    strncpy(base, rom_name, sizeof(base) - 1);
    base[sizeof(base) - 1] = 0;
    char *dot = strrchr(base, '.');
    if (dot) *dot = 0;
    snprintf(out, sz, "%s/saves/%s%s", rom_dir, base, ext);
}

static int write_file(const char *path, const void *data, size_t size)
{
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    size_t w = fwrite(data, 1, size, f);
    fclose(f);
    return w == size;
}

static void *read_file(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long s = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (s <= 0 || s > 8 * 1024 * 1024) { fclose(f); return NULL; }
    void *buf = malloc((size_t)s);
    if (buf && fread(buf, 1, (size_t)s, f) != (size_t)s) { free(buf); buf = NULL; }
    fclose(f);
    if (buf && size) *size = (size_t)s;
    return buf;
}

static u32 sram_crc;

static u32 checksum(const u8 *p, size_t n)
{
    u32 h = 2166136261u;
    for (size_t i = 0; i < n; i++)
        h = (h ^ p[i]) * 16777619u;
    return h;
}

static void save_sram(int force)
{
    size_t size;
    u8 *ram = nes ? nes_battery_ram(nes, &size) : NULL;
    if (!ram) return;
    u32 c = checksum(ram, size);
    if (!force && c == sram_crc) return;
    char p[512];
    make_path(p, sizeof p, ".sav");
    if (write_file(p, ram, size))
        sram_crc = c;
}

static void load_config(void)
{
    char p[300];
    snprintf(p, sizeof p, "%s/nescore_v2.cfg", rom_dir);
    FILE *f = fopen(p, "r");
    if (!f) return;
    fscanf(f, "%d %d %d %d %d", &cfg.runahead, &cfg.smooth, &cfg.crop, &cfg.stretch, &cfg.region);
    if (cfg.region < 0 || cfg.region > 2) cfg.region = 0;
    if (cfg.stretch < 0 || cfg.stretch > 2) cfg.stretch = 2;
    fclose(f);
    if (cfg.runahead < 0 || cfg.runahead > 2) cfg.runahead = 0;
}

static void save_config(void)
{
    char p[300];
    snprintf(p, sizeof p, "%s/nescore_v2.cfg", rom_dir);
    FILE *f = fopen(p, "w");
    if (!f) return;
    fprintf(f, "%d %d %d %d %d\n", cfg.runahead, cfg.smooth, cfg.crop, cfg.stretch, cfg.region);
    fclose(f);
}

static void message(const char *title, const char *msg)
{
    for (int i = 0; i < 150; i++) {
        m_clear();
        m_text(1, 1, title, C_HI);
        m_text(1, 3, msg, C_TEXT);
        m_text(1, 26, "Press A", C_DIM);
        m_show();
        if (menu_keys() & (K_OK | K_BACK)) break;
    }
}

/* ------------------------------------------------------------------ */
/* ROM browser                                                         */

typedef struct { char name[128]; int is_dir; } entry_t;
static entry_t *entries;
static int n_entries;

static int cmp_entry(const void *a, const void *b)
{
    const entry_t *x = a, *y = b;
    if (x->is_dir != y->is_dir) return y->is_dir - x->is_dir;
    return strcasecmp(x->name, y->name);
}

static void scan_dir(const char *path)
{
    free(entries);
    entries = NULL;
    n_entries = 0;
    int cap = 0;
    DIR *d = opendir(path);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char full[512];
        snprintf(full, sizeof full, "%s/%s", path, e->d_name);
        struct stat st;
        int is_dir = stat(full, &st) == 0 && S_ISDIR(st.st_mode);
        if (!is_dir) {
            const char *ext = strrchr(e->d_name, '.');
            if (!ext || strcasecmp(ext, ".nes")) continue;
        } else if (!strcasecmp(e->d_name, "saves")) {
            continue;
        }
        if (n_entries == cap) {
            cap = cap ? cap * 2 : 64;
            entries = realloc(entries, cap * sizeof(entry_t));
        }
        strncpy(entries[n_entries].name, e->d_name, 127);
        entries[n_entries].name[127] = 0;
        entries[n_entries].is_dir = is_dir;
        n_entries++;
    }
    closedir(d);
    qsort(entries, n_entries, sizeof(entry_t), cmp_entry);
}

/* returns 1 when a ROM was chosen, 0 to quit */
static int browse(void)
{
    char cur[256];
    strcpy(cur, rom_dir);
    scan_dir(cur);
    int sel = 0, top = 0;
    const int rows = 22;
    while (!quit_requested) {
        int k = menu_keys();
        if (k & K_UP) sel--;
        if (k & K_DOWN) sel++;
        if (k & K_LEFT) sel -= rows;
        if (k & K_RIGHT) sel += rows;
        if (sel < 0) sel = n_entries ? n_entries - 1 : 0;
        if (sel >= n_entries) sel = 0;
        if (k & K_BACK) {
            char *slash = strrchr(cur, '/');
            if (slash && strchr(cur, '/') != slash) {
                *slash = 0;
                scan_dir(cur);
                sel = top = 0;
            }
        }
        if ((k & K_OK) && n_entries) {
            entry_t *e = &entries[sel];
            char full[512];
            snprintf(full, sizeof full, "%s/%s", cur, e->name);
            if (e->is_dir) {
                strncpy(cur, full, sizeof(cur) - 1);
                scan_dir(cur);
                sel = top = 0;
            } else {
                strncpy(rom_path, full, sizeof(rom_path) - 1);
                strncpy(rom_name, e->name, sizeof(rom_name) - 1);
                strncpy(rom_dir, cur, sizeof(rom_dir) - 1);
                return 1;
            }
        }
        if (k & K_MENU) return 0;
        if (sel < top) top = sel;
        if (sel >= top + rows) top = sel - rows + 1;

        m_clear();
        m_rect(0, 0, MENU_W, 44, C_BAR);
        m_text(1, 0, "nescore", C_ACCENT);
        m_text(10, 0, "cycle-accurate NES for Wii", C_DIM);
        m_text(1, 1, cur, C_TEXT);
        if (!n_entries) {
            m_text(1, 4, "No .nes files here.", C_TEXT);
            m_text(1, 6, "Put your ROMs in sd:/nes/ or usb:/nes/", C_DIM);
        }
        for (int i = 0; i < rows && top + i < n_entries; i++) {
            entry_t *e = &entries[top + i];
            char line[64];
            snprintf(line, sizeof line, "%s%.56s", e->is_dir ? "/" : " ", e->name);
            if (top + i == sel) {
                m_rect(8, 16 + (3 + i) * 16, MENU_W - 16, 16, C_BAR);
                m_text(1, 3 + i, line, C_HI);
            } else {
                m_text(1, 3 + i, line, e->is_dir ? C_ACCENT : C_TEXT);
            }
        }
        m_text(1, 27, "A: open   B: up   HOME: exit to loader", C_DIM);
        m_show();
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* in-game menu                                                        */

static u32 frame_us_avg;

enum { M_RESUME, M_SAVE, M_LOAD, M_RESET, M_REGION, M_RUNAHEAD, M_SMOOTH, M_CROP, M_STRETCH, M_ROMS, M_EXIT, M_COUNT };

/* returns 0 resume, 1 back to ROM list, 2 quit, 3 restart (region changed) */
static int game_menu(void)
{
    static const char *ra_names[3] = {"off", "1 frame", "2 frames"};
    int sel = 0;
    char status[80] = "";
    while (menu_keys()) {} /* wait for release */
    while (!quit_requested) {
        int k = menu_keys();
        if (k & K_UP) sel = (sel + M_COUNT - 1) % M_COUNT;
        if (k & K_DOWN) sel = (sel + 1) % M_COUNT;
        if (k & (K_BACK | K_MENU)) return 0;
        int toggle = (k & (K_OK | K_LEFT | K_RIGHT)) != 0;
        if (toggle) {
            char p[512];
            switch (sel) {
            case M_RESUME: return 0;
            case M_SAVE: {
                size_t sz = nes_state_size(nes);
                void *buf = malloc(sz);
                nes_save_state(nes, buf, sz);
                make_path(p, sizeof p, ".st0");
                strcpy(status, write_file(p, buf, sz) ? "State saved." : "Could not write state!");
                free(buf);
            } break;
            case M_LOAD: {
                size_t sz;
                make_path(p, sizeof p, ".st0");
                void *buf = read_file(p, &sz);
                if (buf && nes_load_state(nes, buf, sz) == NES_OK) {
                    free(buf);
                    return 0;
                }
                free(buf);
                strcpy(status, "No valid savestate.");
            } break;
            case M_RESET: nes_reset(nes); return 0;
            case M_RUNAHEAD: cfg.runahead = (cfg.runahead + ((k & K_LEFT) ? 2 : 1)) % 3; save_config(); break;
            case M_SMOOTH: cfg.smooth ^= 1; save_config(); break;
            case M_CROP: cfg.crop ^= 1; save_config(); break;
            case M_STRETCH: cfg.stretch = (cfg.stretch + ((k & K_LEFT) ? 2 : 1)) % 3; save_config(); break;
            case M_REGION:
                cfg.region = (cfg.region + ((k & K_LEFT) ? 2 : 1)) % 3;
                save_config();
                return 3; /* restart the game with the new timing */
            case M_ROMS: return 1;
            case M_EXIT: return 2;
            }
        }
        char items[M_COUNT][64];
        snprintf(items[M_RESUME], 64, "Resume");
        snprintf(items[M_SAVE], 64, "Save state");
        snprintf(items[M_LOAD], 64, "Load state");
        snprintf(items[M_RESET], 64, "Reset console");
        static const char *region_names[3] = {"auto", "NTSC (60 Hz)", "PAL (50 Hz)"};
        snprintf(items[M_REGION], 64, "Region: %s, now %s", region_names[cfg.region],
                 nes_is_pal(nes) ? "PAL" : "NTSC");
        snprintf(items[M_RUNAHEAD], 64, "Run-ahead (less input lag): %s", ra_names[cfg.runahead]);
        snprintf(items[M_SMOOTH], 64, "Smooth scaling: %s", cfg.smooth ? "on" : "off");
        snprintf(items[M_CROP], 64, "Hide overscan: %s", cfg.crop ? "on" : "off");
        static const char *aspect_names[3] = {"8:7 like a real NES", "stretch 4:3", "sharp (exact 2x pixels)"};
        snprintf(items[M_STRETCH], 64, "Picture: %s", aspect_names[cfg.stretch]);
        snprintf(items[M_ROMS], 64, "Choose another game");
        snprintf(items[M_EXIT], 64, "Exit to loader");

        m_clear();
        m_rect(0, 0, MENU_W, 44, C_BAR);
        m_text(1, 0, "nescore", C_ACCENT);
        m_text(1, 1, rom_name, C_TEXT);
        for (int i = 0; i < M_COUNT; i++) {
            if (i == sel) {
                m_rect(8, 16 + (4 + i * 2) * 16, MENU_W - 16, 16, C_BAR);
                m_text(2, 4 + i * 2, items[i], C_HI);
            } else {
                m_text(2, 4 + i * 2, items[i], C_TEXT);
            }
        }
        char perf[96];
        snprintf(perf, sizeof perf, "Mapper %d   CPU load: %u%% of a frame",
                 nes_mapper_number(nes), (unsigned)(frame_us_avg * 100 / 16683));
        m_text(1, 25, perf, C_DIM);
        m_text(1, 26, status, C_HI);
        m_text(1, 27, "A: select   B/HOME: back to game", C_DIM);
        m_show();
    }
    return 2;
}

/* ------------------------------------------------------------------ */

/* GoodNES / No-Intro region tags for European and Australian releases */
static int name_is_pal(const char *name)
{
    static const char *tags[] = {"(E)", "(Europe", "(EU)", "(PAL", "(Australia", "(A)",
                                 "(G)", "(F)", "(I)", "(Sw)", "(Germany", "(France",
                                 "(Italy", "(Spain", "(Sweden", "(Netherlands", NULL};
    if (strstr(name, "(USA") || strstr(name, "(U)") || strstr(name, "(J") || strstr(name, "(World"))
        return 0;
    for (int i = 0; tags[i]; i++)
        if (strstr(name, tags[i]))
            return 1;
    return 0;
}

/* returns 0 to quit, 1 for the ROM list, 2 to restart the same game */
static int run_game(void)
{
    size_t size;
    void *data = read_file(rom_path, &size);
    if (!data) {
        message("Error", "Could not read the ROM file.");
        return 1;
    }
    nes_set_region(nes, cfg.region == 1 ? NES_REGION_NTSC :
                        cfg.region == 2 ? NES_REGION_PAL :
                        name_is_pal(rom_name) ? NES_REGION_PAL : NES_REGION_AUTO);
    int r = nes_load_rom(nes, data, size);
    free(data);
    if (r != NES_OK) {
        char msg[128];
        snprintf(msg, sizeof msg, "%s", nes_error_string(r));
        message("Cannot start this game", msg);
        return 1;
    }
    char p[512];
    snprintf(p, sizeof p, "%s/saves", rom_dir);
    mkdir(p, 0777);
    size_t ram_size;
    u8 *ram = nes_battery_ram(nes, &ram_size);
    if (ram) {
        size_t s;
        make_path(p, sizeof p, ".sav");
        void *sav = read_file(p, &s);
        if (sav && s == ram_size) memcpy(ram, sav, ram_size);
        free(sav);
        sram_crc = checksum(ram, ram_size);
    }

    GXRModeObj *want = nes_is_pal(nes) ? mode50 : mode60;
    if (want != rmode)
        video_apply(want);

    int frames = 0;
    while (!quit_requested) {
        WPAD_ScanPads();
        PAD_ScanPads();
        nes_set_buttons(nes, 0, read_pad(0));
        nes_set_buttons(nes, 1, read_pad(1));

        if (menu_requested) {
            menu_requested = 0;
            save_sram(0);
            int m = game_menu();
            while (menu_keys()) {}
            menu_requested = 0;
            if (m == 1) { save_sram(1); return 1; }
            if (m == 2) { save_sram(1); return 0; }
            if (m == 3) { save_sram(1); return 2; }
            continue;
        }

        u64 t0 = gettime();
        nes_run_frame_runahead(nes, cfg.runahead);
        audio_push(nes);
        upload_game(nes_framebuffer(nes));
        u32 us = ticks_to_microsecs(gettime() - t0);
        frame_us_avg = (frame_us_avg * 15 + us) / 16;
        /* if run-ahead doesn't fit in a frame on this console, back off */
        if (cfg.runahead && frames > 120 && frame_us_avg > 15500) {
            cfg.runahead--;
            frame_us_avg = 0;
            frames = 0;
        }
        draw_game();
        present();
        if (++frames % 600 == 0)
            save_sram(0);
    }
    save_sram(1);
    return 0;
}

int main(int argc, char **argv)
{
    video_init();
    WPAD_Init();
    PAD_Init();
    WPAD_SetDataFormat(WPAD_CHAN_ALL, WPAD_FMT_BTNS_ACC_IR);
    SYS_SetPowerCallback(power_cb);
    SYS_SetResetCallback(reset_cb);
    WPAD_SetPowerButtonCallback(wpad_power_cb);
    audio_init();

    if (!fatInitDefault()) {
        message("No storage found", "Insert an SD card or USB drive with a /nes folder.");
        return 0;
    }
    struct stat st;
    if (stat("sd:/nes", &st) == 0) strcpy(rom_dir, "sd:/nes");
    else if (stat("sd:/roms/nes", &st) == 0) strcpy(rom_dir, "sd:/roms/nes");
    else if (stat("usb:/nes", &st) == 0) strcpy(rom_dir, "usb:/nes");
    else if (stat("usb:/roms/nes", &st) == 0) strcpy(rom_dir, "usb:/roms/nes");
    else strcpy(rom_dir, "sd:");
    load_config();

    nes = nes_create(SAMPLE_RATE);
    /* a ROM path passed by a loader (e.g. a forwarder) starts directly */
    if (argc > 1 && argv[1] && strstr(argv[1], ".nes")) {
        strncpy(rom_path, argv[1], sizeof(rom_path) - 1);
        const char *slash = strrchr(rom_path, '/');
        strncpy(rom_name, slash ? slash + 1 : rom_path, sizeof(rom_name) - 1);
        if (slash) {
            size_t len = (size_t)(slash - rom_path);
            memcpy(rom_dir, rom_path, len);
            rom_dir[len] = 0;
        }
        int r;
        while ((r = run_game()) == 2 && !quit_requested) {}
        if (!r || quit_requested)
            goto out;
    }
    while (!quit_requested && browse()) {
        int r;
        while ((r = run_game()) == 2 && !quit_requested) {}
        if (!r)
            break;
        if (rmode != mode60)
            video_apply(mode60);
    }
out:
    nes_destroy(nes);
    AUDIO_StopDMA();
    if (quit_requested == 2)
        SYS_ResetSystem(SYS_POWEROFF, 0, 0);
    return 0;
}
