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
 *   Quick save: hold B, press +   Quick load: hold B, press -
 */
#include <gccore.h>
#include <wiiuse/wpad.h>
#include <ogc/lwp_watchdog.h>
#include <ogc/machine/processor.h>
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
#include "../stub/dolboot.h"

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
    int palette;     /* see nes_palette_name() */
} cfg = {1, 0, 1, 2, 0, 1};

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

static void build_palette(void)
{
    uint32_t rgb[512];
    nes_palette_rgb_ex(cfg.palette, rgb);
    for (int i = 0; i < 512; i++) {
        u32 c = rgb[i];
        pal565[i] = (u16)(((c >> 8) & 0xF800) | ((c >> 5) & 0x07E0) | ((c >> 3) & 0x001F));
    }
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

    build_palette();
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

/* short on-screen message, drawn into the (4x4 tiled) game texture */
static char osd_text[40];
static int osd_frames;

static void osd_show(const char *msg)
{
    strncpy(osd_text, msg, sizeof(osd_text) - 1);
    osd_frames = 120;
}

static void osd_draw(void)
{
    if (osd_frames <= 0)
        return;
    osd_frames--;
    int len = (int)strlen(osd_text);
    for (int y = 6; y < 6 + 20; y++)
        for (int x = 6; x < 6 + len * 8 + 6 && x < 256; x++) {
            int gy = y - 8, gx = x - 9, on = 0;
            if (gy >= 0 && gy < 16 && gx >= 0 && gx < len * 8)
                on = console_font_8x16[(u8)osd_text[gx >> 3] * 16 + gy] & (0x80 >> (gx & 7));
            game_tex[((y >> 2) * 64 + (x >> 2)) * 16 + (y & 3) * 4 + (x & 3)] = on ? 0xFFFF : 0x0000;
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
/* menu drawing: a 512x480 RGB565 canvas, shown full screen              */

#define RGB(r, g, b) (u16)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3))
/* dark theme in the style of Retro Shelf 8: near-black, red accents */
#define C_TEXT    RGB(236, 237, 240)
#define C_DIM     RGB(139, 143, 153)
#define C_LINE    RGB(52, 54, 62)
#define C_CARD    RGB(32, 34, 41)
#define C_HOVER   RGB(46, 40, 44)
#define C_BLUE    RGB(226, 56, 56)      /* accent (red) */
#define C_BLUE_DK RGB(170, 36, 36)
#define C_FOLDER  RGB(232, 180, 64)
#define C_CART    RGB(110, 114, 124)
#define C_OK      RGB(46, 158, 90)
#define C_WHITE   RGB(255, 255, 255)
#define C_BLACK   RGB(10, 10, 12)
#define C_HEAD    RGB(24, 25, 31)

static void m_clear(void)
{
    /* soft vertical gradient */
    for (int y = 0; y < MENU_H; y++) {
        int t = y * 255 / MENU_H;
        u16 c = RGB(22 - t * 8 / 255, 23 - t * 8 / 255, 28 - t * 9 / 255);
        u16 *row = &menu_lin[y * MENU_W];
        for (int x = 0; x < MENU_W; x++)
            row[x] = c;
    }
}

static void m_rect(int x, int y, int w, int h, u16 c)
{
    for (int j = y < 0 ? 0 : y; j < y + h && j < MENU_H; j++)
        for (int i = x < 0 ? 0 : x; i < x + w && i < MENU_W; i++)
            menu_lin[j * MENU_W + i] = c;
}

/* filled rectangle with rounded corners */
static void m_round(int x, int y, int w, int h, int r, u16 c)
{
    for (int j = 0; j < h; j++) {
        int yy = y + j;
        if (yy < 0 || yy >= MENU_H) continue;
        int inset = 0;
        int dy = j < r ? r - j : (j >= h - r ? j - (h - r - 1) : 0);
        if (dy) {
            while (inset < r && (r - inset) * (r - inset) + dy * dy > r * r + r)
                inset++;
        }
        for (int i = x + inset; i < x + w - inset; i++)
            if (i >= 0 && i < MENU_W)
                menu_lin[yy * MENU_W + i] = c;
    }
}

/* rounded box with a border */
static void m_box(int x, int y, int w, int h, int r, int border, u16 bc, u16 fill)
{
    m_round(x, y, w, h, r, bc);
    m_round(x + border, y + border, w - 2 * border, h - 2 * border, r > border ? r - border : 1, fill);
}

/* text at pixel position, scale 1 or 2 */
static void m_textp(int x0, int y0, const char *s, u16 color, int scale)
{
    for (; *s; s++, x0 += 8 * scale) {
        if (x0 + 8 * scale > MENU_W) break;
        const u8 *g = &console_font_8x16[(u8)*s * 16];
        for (int y = 0; y < 16; y++)
            for (int x = 0; x < 8; x++)
                if (g[y] & (0x80 >> x))
                    m_rect(x0 + x * scale, y0 + y * scale, scale, scale, color);
    }
}

/* text clipped to max_px wide, with "..." when it does not fit */
static void m_textfit(int x, int y, const char *s, u16 color, int max_px)
{
    char buf[96];
    int max = max_px / 8;
    if (max > 95) max = 95;
    int len = (int)strlen(s);
    if (len <= max) {
        m_textp(x, y, s, color, 1);
        return;
    }
    if (max < 4) return;
    memcpy(buf, s, (size_t)(max - 3));
    strcpy(buf + max - 3, "...");
    m_textp(x, y, buf, color, 1);
}

static void m_text_center(int cx, int y, const char *s, u16 color, int scale)
{
    m_textp(cx - (int)strlen(s) * 4 * scale, y, s, color, scale);
}

static void m_tri(int cx, int cy, int size, int up, u16 c)
{
    for (int j = 0; j < size; j++) {
        int half = up ? j : size - 1 - j;
        m_rect(cx - half, cy - size / 2 + j, 2 * half + 1, 1, c);
    }
}

static void m_arrow_lr(int cx, int cy, int size, int right, u16 c)
{
    for (int i = 0; i < size; i++) {
        int half = right ? size - 1 - i : i;
        m_rect(cx - size / 2 + i, cy - half, 1, 2 * half + 1, c);
    }
}

static void m_icon_folder(int x, int y)
{
    m_round(x, y + 2, 8, 4, 1, C_FOLDER);
    m_round(x, y + 4, 18, 12, 2, C_FOLDER);
}

static void m_icon_cart(int x, int y)
{
    m_round(x + 1, y, 16, 16, 2, C_CART);
    m_rect(x + 4, y + 3, 10, 6, RGB(180, 184, 192));
    for (int i = 0; i < 4; i++)
        m_rect(x + 3 + i * 3, y + 13, 2, 3, RGB(110, 116, 126));
}

/* Pointer: an arrow with a white outline. Rows of the shape, '#' = outline,
 * '.' = fill. */
static void m_cursor(int px, int py)
{
    static const char *shape[] = {
        "#",          "##",         "#.#",        "#..#",       "#...#",
        "#....#",     "#.....#",    "#......#",   "#.......#",  "#........#",
        "#.........#", "#......####", "#...#..#",   "#..##..#",   "#.#  #..#",
        "##   #..#",  "#     #..#", "      #..#", "       ##",  NULL};
    for (int j = 0; shape[j]; j++)
        for (int i = 0; shape[j][i]; i++) {
            char ch = shape[j][i];
            if (ch == '#')
                m_rect(px + i - 1, py + j - 1, 3, 3, C_BLACK);
        }
    for (int j = 0; shape[j]; j++)
        for (int i = 0; shape[j][i]; i++) {
            char ch = shape[j][i];
            if (ch == '#') m_rect(px + i, py + j, 1, 1, j < 4 ? C_BLUE : C_WHITE);
            else if (ch == '.') m_rect(px + i, py + j, 1, 1, j < 4 ? C_BLUE : C_WHITE);
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

enum { K_UP = 1, K_DOWN = 2, K_LEFT = 4, K_RIGHT = 8, K_OK = 16, K_BACK = 32, K_MENU = 64,
       K_PLUS = 128, K_MINUS = 256 };

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
static int hotkey;          /* 1 = quick save, 2 = quick load */
static u32 prev_held[4];

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
    /* B + plus = save state, B + minus = load state (Wii Remote B trigger,
     * or B on a Classic Controller). While B is held, plus/minus are hotkeys
     * and do not reach the game. */
    u32 pressed = h & ~prev_held[chan];
    prev_held[chan] = h;
    int b_held = (h & (WPAD_BUTTON_B | WPAD_CLASSIC_BUTTON_B)) != 0;
    if (b_held) {
        if (pressed & (WPAD_BUTTON_PLUS | WPAD_CLASSIC_BUTTON_PLUS)) hotkey = 1;
        if (pressed & (WPAD_BUTTON_MINUS | WPAD_CLASSIC_BUTTON_MINUS)) hotkey = 2;
        b &= (u8)~(NES_START | NES_SELECT);
    } else {
        if (h & (WPAD_BUTTON_PLUS | WPAD_CLASSIC_BUTTON_PLUS)) b |= NES_START;
        if (h & (WPAD_BUTTON_MINUS | WPAD_CLASSIC_BUTTON_MINUS)) b |= NES_SELECT;
    }
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
        if (h & (WPAD_BUTTON_PLUS | WPAD_CLASSIC_BUTTON_PLUS)) k |= K_PLUS;
        if (h & (WPAD_BUTTON_MINUS | WPAD_CLASSIC_BUTTON_MINUS)) k |= K_MINUS;
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
            pressed = k & (K_UP | K_DOWN | K_LEFT | K_RIGHT | K_PLUS | K_MINUS);
    } else {
        repeat = 0;
    }
    held_prev = k;
    return pressed;
}

/* Wii Remote pointer, in menu canvas coordinates */
static int ptr_valid, ptr_x, ptr_y, ptr_chan, ptr_active;
static int rumble_frames, rumble_chan;

static void read_pointer(void)
{
    static int last_x = -100, last_y = -100;
    ptr_valid = 0;
    for (int c = 0; c < 4; c++) {
        WPADData *wd = WPAD_Data(c);
        if (!wd || wd->err != WPAD_ERR_NONE || !wd->ir.valid)
            continue;
        /* IR is reported in a 640x480 virtual screen; the canvas is drawn
         * over w x 480 units, centred, on a scr_w x scr_h screen */
        float sx = wd->ir.x * scr_w / 640.f, sy = wd->ir.y * scr_h / 480.f;
        float w = widescreen ? scr_w * 0.75f : scr_w;
        float x0 = (scr_w - w) / 2.f, y0 = (scr_h - 480.f) / 2.f;
        ptr_x = (int)((sx - x0) * MENU_W / w);
        ptr_y = (int)(sy - y0);
        ptr_valid = ptr_x >= 0 && ptr_x < MENU_W && ptr_y >= 0 && ptr_y < MENU_H;
        ptr_chan = c;
        break;
    }
    if (ptr_valid && (abs(ptr_x - last_x) > 3 || abs(ptr_y - last_y) > 3)) {
        ptr_active = 1; /* the pointer moved: it drives the focus again */
        last_x = ptr_x;
        last_y = ptr_y;
    }
    if (!ptr_valid)
        ptr_active = 0;
}

static void rumble_tick(void)
{
    if (rumble_frames > 0 && --rumble_frames == 0)
        WPAD_Rumble(rumble_chan, 0);
}

static void rumble_tap(void)
{
    rumble_chan = ptr_chan;
    rumble_frames = 3;
    WPAD_Rumble(rumble_chan, 1);
}

static void rumble_off(void)
{
    rumble_frames = 0;
    for (int c = 0; c < 4; c++)
        WPAD_Rumble(c, 0);
}

/* clickable areas of the current screen */
typedef struct { short x, y, w, h, id, kind; } hit_t; /* kind 1 = option with < > */
#define MAX_HITS 32
static hit_t hits[MAX_HITS];
static int n_hits;

static void hit_add(int x, int y, int w, int h, int id, int kind)
{
    if (n_hits < MAX_HITS)
        hits[n_hits++] = (hit_t){(short)x, (short)y, (short)w, (short)h, (short)id, (short)kind};
}

static int hit_at(int x, int y)
{
    for (int i = 0; i < n_hits; i++)
        if (x >= hits[i].x && x < hits[i].x + hits[i].w && y >= hits[i].y && y < hits[i].y + hits[i].h)
            return i;
    return -1;
}

/*
 * Shared input step for menus. *focus is an index into hits[]; it follows
 * the pointer when the pointer moves, and the D-pad otherwise.
 * Returns the pressed keys; *act is set to the hit index that A activates
 * (or -1), and *dir to -1/+1 when an option is changed with left/right or
 * by clicking its arrows.
 */
static int ui_step(int *focus, int *act, int *dir)
{
    int k = menu_keys();
    read_pointer();
    rumble_tick();
    *act = -1;
    *dir = 0;
    if (ptr_active) {
        int h = hit_at(ptr_x, ptr_y);
        if (h >= 0 && h != *focus)
            rumble_tap();
        *focus = h;
    }
    if (k & (K_UP | K_DOWN)) {
        ptr_active = 0;
        if (*focus < 0) *focus = 0;
        else if (k & K_UP) *focus = (*focus + n_hits - 1) % n_hits;
        else *focus = (*focus + 1) % n_hits;
    }
    if (*focus >= n_hits)
        *focus = n_hits - 1;
    if (*focus >= 0 && hits[*focus].kind == 1) {
        if (k & K_LEFT) *dir = -1;
        if (k & K_RIGHT) *dir = 1;
    }
    if ((k & K_OK) && *focus >= 0) {
        hit_t *h = &hits[*focus];
        *act = *focus;
        if (h->kind == 1) {
            *dir = 1;
            if (ptr_active && ptr_x < h->x + 34)
                *dir = -1;
        }
    }
    if (*dir && *act < 0)
        *act = *focus;
    return k;
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

static int want_shelf;       /* leave for Retro Shelf after the game */
static int profile = -1;     /* Retro Shelf user, -1 when started otherwise */
static char save_dir[300];

/* <rom folder>/saves, or <rom folder>/saves/profile<N> for a Retro Shelf user */
static void setup_save_dir(void)
{
    snprintf(save_dir, sizeof save_dir, "%s/saves", rom_dir);
    mkdir(save_dir, 0777);
    if (profile >= 0) {
        size_t l = strlen(save_dir);
        snprintf(save_dir + l, sizeof(save_dir) - l, "/profile%d", profile);
        mkdir(save_dir, 0777);
    }
}

static void make_path(char *out, size_t sz, const char *ext)
{
    char base[256];
    strncpy(base, rom_name, sizeof(base) - 1);
    base[sizeof(base) - 1] = 0;
    char *dot = strrchr(base, '.');
    if (dot) *dot = 0;
    snprintf(out, sz, "%s/%s%s", save_dir, base, ext);
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
    fscanf(f, "%d %d %d %d %d %d", &cfg.runahead, &cfg.smooth, &cfg.crop, &cfg.stretch, &cfg.region, &cfg.palette);
    if (cfg.palette < 0 || cfg.palette >= NES_PALETTE_COUNT) cfg.palette = 1;
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
    fprintf(f, "%d %d %d %d %d %d\n", cfg.runahead, cfg.smooth, cfg.crop, cfg.stretch, cfg.region, cfg.palette);
    fclose(f);
}

static char player[40];      /* "Player: NAME" when started by Retro Shelf */
static int from_shelf;

static void header(const char *subtitle, int show_player)
{
    m_rect(0, 0, MENU_W, 56, C_HEAD);
    m_rect(0, 56, MENU_W, 2, C_BLUE);
    m_round(14, 10, 124, 36, 4, C_BLUE);
    m_textp(20, 12, "nescore", C_WHITE, 2);
    int right = show_player && player[0] ? (int)strlen(player) * 8 + 24 : 0;
    if (subtitle)
        m_textfit(152, 20, subtitle, C_DIM, MENU_W - 152 - (right ? right : 120));
    if (right)
        m_textp(MENU_W - (int)strlen(player) * 8 - 16, 20, player, C_TEXT, 1);
}

static void button(hit_t *h, const char *label, int focused, int primary)
{
    u16 fill = primary ? (focused ? C_BLUE_DK : C_BLUE) : (focused ? C_HOVER : C_CARD);
    u16 border = focused ? C_BLUE : C_LINE;
    m_box(h->x, h->y, h->w, h->h, 8, focused ? 3 : 1, border, fill);
    m_text_center(h->x + h->w / 2, h->y + h->h / 2 - 8, label, primary ? C_WHITE : C_TEXT, 1);
}

static void message(const char *title, const char *msg)
{
    int focus = 0, act, dir;
    while (menu_keys()) {}
    while (!quit_requested) {
        n_hits = 0;
        hit_add(196, 300, 120, 40, 0, 0);
        int k = ui_step(&focus, &act, &dir);
        if (act == 0 || (k & K_BACK)) break;
        m_clear();
        header(NULL, 0);
        m_box(56, 150, 400, 210, 14, 1, C_LINE, C_CARD);
        m_text_center(256, 180, title, C_TEXT, 1);
        m_rect(76, 204, 360, 1, C_LINE);
        m_text_center(256, 224, msg, C_DIM, 1);
        button(&hits[0], "OK", focus == 0, 1);
        if (ptr_valid) m_cursor(ptr_x, ptr_y);
        m_show();
    }
    rumble_off();
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
enum { B_BACK = 1, B_UP, B_DOWN, B_EXIT, B_ROW = 100 };

static void draw_browser(const char *cur, int top, int rows, int n_rows, int focus)
{
    m_clear();
    header("Choose a game", 0);
    for (int i = 0; i < n_hits; i++) {
        hit_t *h = &hits[i];
        int f = i == focus;
        if (h->id >= B_ROW) {
            entry_t *e = &entries[h->id - B_ROW];
            m_box(h->x, h->y, h->w, h->h, 8, f ? 3 : 1, f ? C_BLUE : C_LINE, f ? C_HOVER : C_CARD);
            if (e->is_dir) m_icon_folder(h->x + 12, h->y + 9);
            else m_icon_cart(h->x + 12, h->y + 9);
            char name[128];
            strncpy(name, e->name, sizeof(name) - 1);
            name[sizeof(name) - 1] = 0;
            char *dot = e->is_dir ? NULL : strrchr(name, '.');
            if (dot) *dot = 0;
            m_textfit(h->x + 40, h->y + 9, name, C_TEXT, h->w - 52);
        } else if (h->id == B_UP || h->id == B_DOWN) {
            int can = h->id == B_UP ? top > 0 : top + rows < n_entries;
            m_box(h->x, h->y, h->w, h->h, 8, f ? 3 : 1, f ? C_BLUE : C_LINE, f ? C_HOVER : C_CARD);
            m_tri(h->x + h->w / 2, h->y + h->h / 2, 10, h->id == B_UP, can ? C_TEXT : C_LINE);
        } else {
            button(h, h->id == B_BACK ? "< Back" : "Exit", f, 0);
        }
    }
    m_textfit(20, 64, cur, C_DIM, 470);
    if (!n_entries) {
        m_box(56, 160, 400, 120, 14, 1, C_LINE, C_CARD);
        m_text_center(256, 190, "No .nes files here", C_TEXT, 1);
        m_text_center(256, 220, "Put your games in sd:/nes or sd:/roms/nes", C_DIM, 1);
    }
    if (n_entries > rows) {
        char pos[32];
        snprintf(pos, sizeof pos, "%d-%d of %d", top + 1, top + n_rows, n_entries);
        m_textp(496 - (int)strlen(pos) * 8, 438, pos, C_DIM, 1);
    }
    m_textp(20, 438, "Point + A: open   B: back   +/-: page", C_DIM, 1);
}

static int browse(void)
{
    char cur[256];
    strcpy(cur, rom_dir);
    scan_dir(cur);
    int top = 0, focus = -1, act, dir;
    const int rows = 9, row_h = 34, row_gap = 4, list_y = 84;
    while (menu_keys()) {}
    while (!quit_requested) {
        int at_root = !(strrchr(cur, '/') && strchr(cur, '/') != strrchr(cur, '/'));
        if (top > n_entries - rows) top = n_entries - rows;
        if (top < 0) top = 0;

        /* layout */
        n_hits = 0;
        for (int i = 0; i < rows && top + i < n_entries; i++)
            hit_add(16, list_y + i * (row_h + row_gap), 436, row_h, B_ROW + top + i, 0);
        int n_rows = n_hits;
        if (n_entries > rows) {
            hit_add(460, list_y, 36, 166, B_UP, 0);
            hit_add(460, list_y + 172, 36, 166, B_DOWN, 0);
        }
        if (!at_root)
            hit_add(388, 12, 104, 34, B_BACK, 0);
        else
            hit_add(388, 12, 104, 34, B_EXIT, 0);

        int old_top = top, old_focus = focus;
        int k = ui_step(&focus, &act, &dir);
        int id = act >= 0 ? hits[act].id : 0;
        /* The D-pad walks through the games only (the pointer reaches the
         * buttons too) and scrolls the list when it reaches an edge. */
        if (!ptr_active && (k & (K_UP | K_DOWN)) && n_entries) {
            int sel = (old_focus >= 0 && old_focus < n_rows) ? top + old_focus : top - ((k & K_DOWN) ? 1 : 0);
            sel += (k & K_DOWN) ? 1 : -1;
            if (sel < 0) sel = n_entries - 1;
            if (sel >= n_entries) sel = 0;
            if (sel < top) top = sel;
            if (sel >= top + rows) top = sel - rows + 1;
            focus = sel - top;
        }
        if (k & K_PLUS) top += rows;
        if (k & K_MINUS) top -= rows;
        if (k & K_BACK) id = at_root ? 0 : B_BACK;
        if (k & K_MENU) return 0;

        if (id == B_EXIT) { rumble_off(); return 0; }
        if (id == B_UP) top -= rows - 1;
        if (id == B_DOWN) top += rows - 1;
        if (id == B_BACK) {
            char *slash = strrchr(cur, '/');
            if (slash && strchr(cur, '/') != slash) {
                *slash = 0;
                scan_dir(cur);
                top = 0;
                focus = -1;
            }
        }
        if (id >= B_ROW && id - B_ROW < n_entries) {
            entry_t *e = &entries[id - B_ROW];
            char full[512];
            snprintf(full, sizeof full, "%s/%s", cur, e->name);
            if (e->is_dir) {
                strncpy(cur, full, sizeof(cur) - 1);
                scan_dir(cur);
                top = 0;
                focus = -1;
            } else {
                strncpy(rom_path, full, sizeof(rom_path) - 1);
                strncpy(rom_name, e->name, sizeof(rom_name) - 1);
                strncpy(rom_dir, cur, sizeof(rom_dir) - 1);
                rumble_off();
                return 1;
            }
        }
        if (top > n_entries - rows) top = n_entries - rows;
        if (top < 0) top = 0;
        if (top != old_top) continue; /* relayout before drawing */

        draw_browser(cur, top, rows, n_rows, focus);
        if (ptr_valid) m_cursor(ptr_x, ptr_y);
        m_show();
        if (ptr_valid) m_cursor(ptr_x, ptr_y);
        m_show();
    }
    rumble_off();
    return 0;
}

/* ------------------------------------------------------------------ */
/* in-game menu                                                        */

static u32 frame_us_avg;

enum { M_RESUME, M_SAVE, M_LOAD, M_RESET, M_REGION, M_PALETTE, M_RUNAHEAD, M_SMOOTH, M_CROP, M_STRETCH, M_ROMS, M_EXIT, M_COUNT };

static int save_state_file(void)
{
    char p[512];
    size_t sz = nes_state_size(nes);
    void *buf = malloc(sz);
    if (!buf) return 0;
    nes_save_state(nes, buf, sz);
    make_path(p, sizeof p, ".st0");
    int ok = write_file(p, buf, sz);
    free(buf);
    return ok;
}

static int load_state_file(void)
{
    char p[512];
    size_t sz;
    make_path(p, sizeof p, ".st0");
    void *buf = read_file(p, &sz);
    int ok = buf && nes_load_state(nes, buf, sz) == NES_OK;
    free(buf);
    return ok;
}

/* small picture of the paused game, drawn into the canvas */
static void m_thumbnail(int x0, int y0)
{
    const uint16_t *fb = nes_framebuffer(nes);
    m_round(x0 - 4, y0 - 4, 136, 128, 6, C_LINE);
    for (int y = 0; y < 120; y++)
        for (int x = 0; x < 128; x++)
            menu_lin[(y0 + y) * MENU_W + x0 + x] = pal565[fb[(y * 2) * 256 + x * 2] & 511];
}

static void draw_game_menu(int focus, const char *status, int status_ok)
{
    static const char *ra_names[3] = {"off", "1 frame", "2 frames"};
    static const char *region_names[3] = {"auto", "NTSC (60 Hz)", "PAL (50 Hz)"};
    static const char *aspect_names[3] = {"8:7 like a NES", "stretch 4:3", "sharp 2x pixels"};
    char title[128];
    strncpy(title, rom_name, sizeof(title) - 1);
    title[sizeof(title) - 1] = 0;
    char *dot = strrchr(title, '.');
    if (dot) *dot = 0;
    m_clear();
    header(title, 1);
    m_thumbnail(52, 72);
    for (int i = 0; i < n_hits; i++) {
        hit_t *h = &hits[i];
        int f = i == focus;
        static const char *labels[M_COUNT] = {
            [M_RESUME] = "Resume", [M_SAVE] = "Save state", [M_LOAD] = "Load state",
            [M_RESET] = "Reset", [M_ROMS] = "Other game", [M_EXIT] = "Exit to loader"};
        if (h->kind == 0 && h->id == M_ROMS && from_shelf) {
            button(h, "Retro Shelf 8", f, 0);
            continue;
        }
        if (h->kind == 0) {
            button(h, labels[h->id], f, h->id == M_RESUME);
            continue;
        }
        const char *name = "", *value = "";
        char vbuf[48];
        switch (h->id) {
        case M_REGION:
            name = "Region";
            snprintf(vbuf, sizeof vbuf, "%s (%s)", region_names[cfg.region], nes_is_pal(nes) ? "PAL" : "NTSC");
            value = vbuf;
            break;
        case M_PALETTE: name = "Colours"; value = nes_palette_name(cfg.palette); break;
        case M_RUNAHEAD: name = "Run-ahead (input lag)"; value = ra_names[cfg.runahead]; break;
        case M_SMOOTH: name = "Smooth scaling"; value = cfg.smooth ? "on" : "off"; break;
        case M_CROP: name = "Hide overscan"; value = cfg.crop ? "on" : "off"; break;
        case M_STRETCH: name = "Picture"; value = aspect_names[cfg.stretch]; break;
        }
        m_box(h->x, h->y, h->w, h->h, 8, f ? 3 : 1, f ? C_BLUE : C_LINE, f ? C_HOVER : C_CARD);
        m_arrow_lr(h->x + 16, h->y + h->h / 2, 8, 0, f ? C_BLUE : C_DIM);
        m_arrow_lr(h->x + h->w - 16, h->y + h->h / 2, 8, 1, f ? C_BLUE : C_DIM);
        m_textfit(h->x + 34, h->y + 5, name, C_DIM, h->w - 68);
        m_textfit(h->x + 34, h->y + 22, value, C_TEXT, h->w - 68);
    }
    char info[80];
    snprintf(info, sizeof info, "Mapper %d   CPU load %u%%", nes_mapper_number(nes),
             (unsigned)(frame_us_avg * 100 / 16683));
    m_textp(232, 378, info, C_DIM, 1);
    if (status[0]) {
        m_round(232, 400, 264, 26, 6, status_ok ? C_OK : RGB(210, 80, 60));
        m_text_center(364, 405, status, C_WHITE, 1);
    }
    m_textp(20, 446, "Point + A: select   B / HOME: back to game", C_DIM, 1);
}

/* returns 0 resume, 1 back to ROM list, 2 quit, 3 restart (region changed), 4 Retro Shelf */
static int game_menu(void)
{
    int focus = 0, act, dir;
    char status[64] = "";
    int status_ok = 1;
    while (menu_keys()) {}
    while (!quit_requested) {
        /* layout: actions on the left, settings on the right */
        n_hits = 0;
        static const int left_ids[6] = {M_RESUME, M_SAVE, M_LOAD, M_RESET, M_ROMS, M_EXIT};
        for (int i = 0; i < 6; i++)
            hit_add(16, 200 + i * 38, 200, 32, left_ids[i], 0);
        /* started by Retro Shelf: "Other game" goes back to the shelf */
        static const int right_ids[6] = {M_REGION, M_PALETTE, M_RUNAHEAD, M_SMOOTH, M_CROP, M_STRETCH};
        for (int i = 0; i < 6; i++)
            hit_add(232, 70 + i * 50, 264, 44, right_ids[i], 1);

        int k = ui_step(&focus, &act, &dir);
        if (k & (K_BACK | K_MENU)) { rumble_off(); return 0; }
        if (act >= 0) {
            int id = hits[act].id, d = dir ? dir : 1;
            switch (id) {
            case M_RESUME: rumble_off(); return 0;
            case M_SAVE:
                status_ok = save_state_file();
                strcpy(status, status_ok ? "State saved" : "Could not write the state");
                break;
            case M_LOAD:
                if (load_state_file()) { rumble_off(); return 0; }
                status_ok = 0;
                strcpy(status, "No state saved yet");
                break;
            case M_RESET: nes_reset(nes); rumble_off(); return 0;
            case M_ROMS: rumble_off(); return from_shelf ? 4 : 1;
            case M_EXIT: rumble_off(); return 2;
            case M_RUNAHEAD: cfg.runahead = (cfg.runahead + 3 + d) % 3; save_config(); break;
            case M_SMOOTH: cfg.smooth ^= 1; save_config(); break;
            case M_CROP: cfg.crop ^= 1; save_config(); break;
            case M_STRETCH: cfg.stretch = (cfg.stretch + 3 + d) % 3; save_config(); break;
            case M_PALETTE:
                cfg.palette = (cfg.palette + NES_PALETTE_COUNT + d) % NES_PALETTE_COUNT;
                build_palette();
                save_config();
                break;
            case M_REGION:
                cfg.region = (cfg.region + 3 + d) % 3;
                save_config();
                rumble_off();
                return 3; /* restart the game with the new timing */
            }
        }

        draw_game_menu(focus, status, status_ok);
        if (ptr_valid) m_cursor(ptr_x, ptr_y);
        m_show();
        if (ptr_valid) m_cursor(ptr_x, ptr_y);
        m_show();
    }
    rumble_off();
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
    setup_save_dir();
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
            if (m == 4) { save_sram(1); want_shelf = 1; return 0; }
            continue;
        }

        u64 t0 = gettime();
        nes_run_frame_runahead(nes, cfg.runahead);
        audio_push(nes);
        if (hotkey) {
            int h = hotkey;
            hotkey = 0;
            if (h == 1)
                osd_show(save_state_file() ? "State saved" : "Save failed");
            else
                osd_show(load_state_file() ? "State loaded" : "No state saved yet");
        }
        upload_game(nes_framebuffer(nes));
        osd_draw();
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

/* ------------------------------------------------------------------ */
/* Retro Shelf                                                          */

static const char *shelf_paths[] = {"sd:/apps/retroshelf/boot.dol", "usb:/apps/retroshelf/boot.dol", NULL};

static int has_rom_ext(const char *s)
{
    const char *e = strrchr(s, '.');
    return e && (!strcasecmp(e, ".nes") || !strcasecmp(e, ".unf") || !strcasecmp(e, ".unif"));
}

/* first printable line of a small text file */
static int read_line(const char *path, char *out, size_t n)
{
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char buf[128];
    int ok = fgets(buf, sizeof buf, f) != NULL;
    fclose(f);
    if (!ok) return 0;
    size_t j = 0;
    for (size_t i = 0; buf[i] && buf[i] != '\n' && buf[i] != '\r' && buf[i] != '\t' && j + 1 < n; i++)
        if (buf[i] >= 32 && buf[i] < 127)
            out[j++] = buf[i];
    out[j] = 0;
    return j > 0;
}

/*
 * Arguments from a loader. Retro Shelf 8 starts its emulator the way
 * FCE Ultra GX expects: the ROM folder and the file name, plus
 * "rsprofile=<user>;<retro shelf folder>/achievements/;..." for its users.
 * A single full path (forwarders) works too.
 */
static int parse_args(int argc, char **argv)
{
    char folder[256] = "", file[256] = "", achdir[256] = "";
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!a || !a[0]) continue;
        if (!strncmp(a, "rsprofile=", 10)) {
            from_shelf = 1;
            profile = atoi(a + 10);
            const char *semi = strchr(a, ';');
            if (semi) {
                const char *end = strchr(semi + 1, ';');
                size_t len = end ? (size_t)(end - semi - 1) : strlen(semi + 1);
                if (len < sizeof achdir) {
                    memcpy(achdir, semi + 1, len);
                    achdir[len] = 0;
                }
            }
        } else if (has_rom_ext(a)) {
            strncpy(file, a, sizeof(file) - 1);
        } else if (strchr(a, '/')) {
            strncpy(folder, a, sizeof(folder) - 1);
        }
    }
    if (!file[0])
        return 0;
    char full[512];
    if (strchr(file, '/')) {
        strncpy(full, file, sizeof(full) - 1);
        full[sizeof(full) - 1] = 0;
    } else {
        size_t l = strlen(folder);
        while (l && folder[l - 1] == '/') folder[--l] = 0;
        snprintf(full, sizeof full, "%s/%s", folder, file);
    }
    /* a path without a device: try SD, then USB */
    if (!strchr(full, ':')) {
        char tmp[520];
        struct stat st;
        snprintf(tmp, sizeof tmp, "sd:%s%s", full[0] == '/' ? "" : "/", full);
        if (stat(tmp, &st) != 0)
            snprintf(tmp, sizeof tmp, "usb:%s%s", full[0] == '/' ? "" : "/", full);
        strncpy(full, tmp, sizeof(full) - 1);
        full[sizeof(full) - 1] = 0;
    }
    strncpy(rom_path, full, sizeof(rom_path) - 1);
    const char *slash = strrchr(rom_path, '/');
    strncpy(rom_name, slash ? slash + 1 : rom_path, sizeof(rom_name) - 1);
    if (slash) {
        size_t len = (size_t)(slash - rom_path);
        memcpy(rom_dir, rom_path, len);
        rom_dir[len] = 0;
    }
    /* started with a game while Retro Shelf is installed: offer the way back */
    struct stat st;
    for (int i = 0; shelf_paths[i]; i++)
        if (stat(shelf_paths[i], &st) == 0)
            from_shelf = 1;
    if (profile >= 0) {
        char name[32] = "", path[320];
        snprintf(path, sizeof path, "%sprofile%d/user.txt", achdir, profile);
        if (!achdir[0] || !read_line(path, name, sizeof name)) {
            snprintf(path, sizeof path, "%suser.txt", achdir);
            if (!achdir[0] || profile > 0 || !read_line(path, name, sizeof name))
                snprintf(name, sizeof name, "%d", profile + 1);
        }
        snprintf(player, sizeof player, "Player: %s", name);
    }
    return 1;
}

/*
 * Start another program (a .dol file), the way the Homebrew Channel does:
 * the file and a small copy routine (stub/dolboot.S) go to the top of MEM2,
 * everything is shut down, and the routine copies the program into place
 * and jumps to it. Returns only when the file could not be used.
 */
static int launch_dol(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 0x100 || size > 16 * 1024 * 1024) { fclose(f); return 0; }

    u32 need = ((u32)size + 256 + 63) & ~31u;
    u32 hi = (u32)SYS_GetArena2Hi();
    u32 base = (hi - need) & ~31u;
    if (base <= (u32)SYS_GetArena2Lo()) { fclose(f); return 0; }
    u8 *stub = (u8 *)base, *dol = (u8 *)(base + 256);
    if (fread(dol, 1, (size_t)size, f) != (size_t)size) { fclose(f); return 0; }
    fclose(f);

    /* sanity checks: an entry point in MEM1 and no section over our copy */
    u32 *hdr = (u32 *)dol;
    if (hdr[56] < 0x80000000u || hdr[56] >= 0x81800000u) return 0;
    for (int i = 0; i < 18; i++) {
        u32 a = hdr[18 + i], sz = hdr[36 + i];
        if (!sz) continue;
        if (hdr[i] + sz > (u32)size) return 0;
        if (a < base + need && a + sz > base) return 0;
    }
    SYS_SetArena2Hi((void *)base);
    memcpy(stub, dolboot_code, sizeof dolboot_code);
    DCFlushRange(stub, 256);
    ICInvalidateRange(stub, 256);
    DCFlushRange(dol, (u32)size);

    /* shut everything down */
    rumble_off();
    AUDIO_StopDMA();
    AUDIO_RegisterDMACallback(NULL);
    WPAD_Shutdown();
    VIDEO_SetBlack(true);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    fatUnmount("sd:");
    fatUnmount("usb:");
    SYS_ResetSystem(SYS_SHUTDOWN, 0, 0);
    u32 level;
    _CPU_ISR_Disable(level);
    (void)level;
    ((void (*)(void *))stub)(dol);
    return 0; /* not reached */
}


static void go_to_shelf(void)
{
    for (int i = 0; shelf_paths[i]; i++)
        launch_dol(shelf_paths[i]);
    message("Could not start Retro Shelf 8", "Is it in sd:/apps/retroshelf/?");
}

int main(int argc, char **argv)
{
    video_init();
    WPAD_Init();
    PAD_Init();
    WPAD_SetDataFormat(WPAD_CHAN_ALL, WPAD_FMT_BTNS_ACC_IR);
    WPAD_SetVRes(WPAD_CHAN_ALL, 640, 480);
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
    /* a game passed by a loader (Retro Shelf, a forwarder) starts directly */
    if (parse_args(argc, argv)) {
        int r;
        while ((r = run_game()) == 2 && !quit_requested) {}
        /* a game started from Retro Shelf that could not run goes back too */
        if (from_shelf && r == 1 && !quit_requested)
            want_shelf = 1;
        if (want_shelf && !quit_requested) {
            nes_destroy(nes);
            nes = NULL;
            go_to_shelf();
        }
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
    if (nes)
        nes_destroy(nes);
    AUDIO_StopDMA();
    if (quit_requested == 2)
        SYS_ResetSystem(SYS_POWEROFF, 0, 0);
    return 0;
}
