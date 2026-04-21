/*
 * doomgeneric_idun.c - IDUN Cartridge backend for Doom
 *
 * Runs on the Raspberry Pi Zero 2 W under Arch Linux ARM.
 * Communicates with the C64 side via named pipes which are
 * relayed through the IDUN TTY interface (Propeller bridge,
 * $DE00/$DE01 I/O registers on C64 side).
 *
 * Protocol (Pi -> C64):
 *   Frame packet:
 *     [0x01]                   frame-start marker
 *     [count_lo] [count_hi]    number of dirty 4x8 blocks (little-endian)
 *     per block:
 *       [idx_lo] [idx_hi]      block index 0..999
 *       [bm0..bm7]             8 bitmap bytes (one per pixel row)
 *       [screen]               screen RAM byte  (hi-nybble=color1, lo=color2)
 *       [color]                color  RAM byte  (lo-nybble=color3)
 *     [audio_count]            number of audio samples following (0 = none)
 *     [sample...]              raw 4-bit SID volume samples (packed, see below)
 *
 * Protocol (C64 -> Pi):
 *   Input packet:
 *     [0x10]                   input marker
 *     [joy]                    joystick byte (active-low, same as $DC01)
 *     [keys]                   keyboard flags (bit0=escape, bit1=enter, ...)
 *
 * C64 video layout (VIC bank 2, $8000-$BFFF):
 *   Screen RAM : $8000-$83E7  (1000 bytes)
 *   Bitmap     : $A000-$BF3F  (8000 bytes)
 *   Color RAM  : $D800-$DBE7  (1000 bytes, always mapped here)
 *   Background : $D021 = 0    (black, fixed)
 *
 * Copyright (c) 2026, IDUN Doom contributors.
 * GPLv3 - see LICENSE.
 */

#include "doomgeneric.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <limits.h>

/* ─── named-pipe paths (Lua launcher creates these before doom-idun starts) ── */
#define PIPE_TO_C64   "/tmp/idun_doom_out"   /* Pi writes, C64 reads  */
#define PIPE_FROM_C64 "/tmp/idun_doom_in"    /* C64 writes, Pi reads  */

/* ─── display constants ────────────────────────────────────────────────────── */
#define C64_COLS       40
#define C64_ROWS       25
#define NUM_BLOCKS     (C64_COLS * C64_ROWS)   /* 1000 */
#define PIXELS_PER_ROW 4     /* multicolor: 4 fat-pixels per byte per row */
#define TARGET_FPS     30
#define FRAME_MS       (1000 / TARGET_FPS)

/* ─── audio constants ───────────────────────────────────────────────────────── */
#define AUDIO_RATE        4000   /* Hz, CIA timer fires at this rate on C64  */
#define AUDIO_BUF_FRAMES  8     /* send this many frames of audio per video frame */
#define AUDIO_CHUNK       (AUDIO_RATE / TARGET_FPS * AUDIO_BUF_FRAMES)

/* ─── C64 Pepto palette (sRGB) ─────────────────────────────────────────────── */
static const uint8_t c64_r[16] = {
     0, 255, 136,  103, 139,  85,  64, 191,
   139,  87, 184,   80, 120, 148, 120, 159 };
static const uint8_t c64_g[16] = {
     0, 255,  57,  182,  63, 160,  49, 206,
    84,  66, 105,   80, 120, 224, 105, 159 };
static const uint8_t c64_b[16] = {
     0, 255,  50,  189, 150,  73, 141, 114,
    41,   0,  98,   80, 120, 137, 196, 159 };

/* ─── state ─────────────────────────────────────────────────────────────────── */
static uint8_t  lut[4096];           /* RGB-4-4-4 → C64 palette index          */
static uint8_t  prev_bm[NUM_BLOCKS * 8];
static uint8_t  prev_sc[NUM_BLOCKS];
static uint8_t  prev_cr[NUM_BLOCKS];

static int      fd_out = -1;
static int      fd_in  = -1;

/* input state decoded from C64 */
static int      joy_state = 0;       /* last joystick byte (inverted = pressed) */
static int      key_flags = 0;       /* bit-field of special keys               */

/* pending key queue (small ring buffer) */
#define KEY_QUEUE_SIZE 16
static struct { int pressed; unsigned char key; } key_queue[KEY_QUEUE_SIZE];
static int kq_head = 0, kq_tail = 0;

/* joystick → Doom key mapping */
static const struct { int bit; unsigned char doomkey; } joy_map[] = {
    { 0x01, DOOM_KEY_UP_ARROW    },   /* joy up    */
    { 0x02, DOOM_KEY_DOWN_ARROW  },   /* joy down  */
    { 0x04, DOOM_KEY_LEFT_ARROW  },   /* joy left  */
    { 0x08, DOOM_KEY_RIGHT_ARROW },   /* joy right */
    { 0x10, DOOM_KEY_FIRE        },   /* fire      */
    { 0,    0 }
};

/* ─── helpers ───────────────────────────────────────────────────────────────── */

static void build_lut(void)
{
    for (int r4 = 0; r4 < 16; r4++)
    for (int g4 = 0; g4 < 16; g4++)
    for (int b4 = 0; b4 < 16; b4++) {
        int r = r4 * 17, g = g4 * 17, b = b4 * 17;
        int best = 0, bestd = INT_MAX;
        for (int c = 0; c < 16; c++) {
            int dr = r - c64_r[c], dg = g - c64_g[c], db = b - c64_b[c];
            int d  = dr*dr + dg*dg + db*db;
            if (d < bestd) { bestd = d; best = c; }
        }
        lut[(r4 << 8) | (g4 << 4) | b4] = (uint8_t)best;
    }
}

static uint8_t nearest_c64(uint32_t argb)
{
    uint8_t r = (argb >> 16) & 0xFF;
    uint8_t g = (argb >>  8) & 0xFF;
    uint8_t b =  argb        & 0xFF;
    return lut[((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4)];
}

static uint32_t ms_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

/* blocking write that retries on EINTR */
static int write_all(int fd, const void *buf, size_t n)
{
    const uint8_t *p = buf;
    size_t rem = n;
    while (rem > 0) {
        ssize_t r = write(fd, p, rem);
        if (r < 0) { if (errno == EINTR) continue; return -1; }
        p   += r;
        rem -= (size_t)r;
    }
    return 0;
}

/* non-blocking read of one byte, returns -1 if nothing available */
static int read_byte_nb(void)
{
    uint8_t b;
    ssize_t r = read(fd_in, &b, 1);
    if (r == 1) return (int)b;
    return -1;
}

/* ─── input processing ──────────────────────────────────────────────────────── */

static void push_key(int pressed, unsigned char key)
{
    int next = (kq_tail + 1) % KEY_QUEUE_SIZE;
    if (next != kq_head) {          /* drop if full */
        key_queue[kq_tail].pressed = pressed;
        key_queue[kq_tail].key     = key;
        kq_tail = next;
    }
}

static void process_input_packet(void)
{
    /* already consumed the 0x10 marker; read 2 more bytes */
    uint8_t bytes[2];
    /* try a short blocking read with timeout handled by caller */
    for (int i = 0; i < 2; i++) {
        int b = -1;
        for (int t = 0; t < 50 && b < 0; t++) {
            b = read_byte_nb();
            if (b < 0) usleep(1000);
        }
        if (b < 0) return;          /* timeout, discard */
        bytes[i] = (uint8_t)b;
    }

    int new_joy  = (int)bytes[0];
    int new_keys = (int)bytes[1];

    /* compare with previous state, emit events */
    for (int i = 0; joy_map[i].bit; i++) {
        int was = (joy_state & joy_map[i].bit) != 0;
        int now = (new_joy   & joy_map[i].bit) != 0;
        if (now && !was) push_key(1, joy_map[i].doomkey);
        if (!now && was) push_key(0, joy_map[i].doomkey);
    }

    /* escape key (bit 0 of key_flags) */
    {
        int was = (key_flags & 0x01) != 0;
        int now = (new_keys  & 0x01) != 0;
        if (now && !was) push_key(1, DOOM_KEY_ESCAPE);
        if (!now && was) push_key(0, DOOM_KEY_ESCAPE);
    }

    /* enter / use (bit 1) */
    {
        int was = (key_flags & 0x02) != 0;
        int now = (new_keys  & 0x02) != 0;
        if (now && !was) push_key(1, DOOM_KEY_ENTER);
        if (!now && was) push_key(0, DOOM_KEY_ENTER);
    }

    joy_state = new_joy;
    key_flags = new_keys;
}

static void poll_input(void)
{
    /* drain available bytes looking for input packets */
    int b;
    while ((b = read_byte_nb()) >= 0) {
        if (b == 0x10) {
            process_input_packet();
        }
        /* ignore unknown markers */
    }
}

/* ─── frame rendering ───────────────────────────────────────────────────────── */

/*
 * Convert one 40-column × 25-row multicolor block.
 * block_idx : 0..999  (row*40 + col)
 * bm_out    : 8 bitmap bytes
 * sc_out    : screen RAM byte
 * cr_out    : color  RAM byte
 */
static void encode_block(int block_idx,
                          uint8_t *bm_out,
                          uint8_t *sc_out,
                          uint8_t *cr_out)
{
    int col = block_idx % C64_COLS;
    int row = block_idx / C64_COLS;

    /* ── sample 4 × 8 multicolor pixels → find C64 colors ── */
    uint8_t pix[8][4];          /* [row][col] C64 palette index */
    uint32_t hist[16] = {0};

    for (int py = 0; py < 8; py++) {
        int dy = row * 8 + py;
        for (int px = 0; px < 4; px++) {
            /* each multicolor pixel = 2 Doom pixels wide */
            int dx = col * 8 + px * 2;
            uint32_t rgb = DG_ScreenBuffer[dy * DOOMGENERIC_RESX + dx];
            uint8_t  c   = nearest_c64(rgb);
            pix[py][px]  = c;
            hist[c]++;
        }
    }

    /* ── pick 3 most-common non-background colors ── */
    /* background is always color index 0 (black, $D021 = 0) */
    uint8_t c1 = 0, c2 = 0, c3 = 0;
    uint32_t f1 = 0, f2 = 0, f3 = 0;

    for (int c = 1; c < 16; c++) {
        if (hist[c] > f1)      { c3=c2; f3=f2; c2=c1; f2=f1; c1=(uint8_t)c; f1=hist[c]; }
        else if (hist[c] > f2) { c3=c2; f3=f2; c2=(uint8_t)c; f2=hist[c]; }
        else if (hist[c] > f3) { c3=(uint8_t)c; f3=hist[c]; }
    }

    *sc_out = (uint8_t)((c1 << 4) | c2);
    *cr_out = c3;                        /* only low nybble used by VIC-II */

    /* ── encode bitmap: 4 pixels × 2 bits per row ── */
    for (int py = 0; py < 8; py++) {
        uint8_t bm = 0;
        for (int px = 0; px < 4; px++) {
            uint8_t pc = pix[py][px];
            /* find nearest of { 0=bg, c1=%01, c2=%10, c3=%11 } */
            int d0 = (pc == 0)  ? 0 : 1;
            int d1 = (pc == c1) ? 0 : 1;
            int d2 = (pc == c2) ? 0 : 1;
            int d3 = (pc == c3) ? 0 : 1;
            /* exact match first */
            uint8_t code = 0;
            if      (d1 == 0) code = 1;
            else if (d2 == 0) code = 2;
            else if (d3 == 0) code = 3;
            else {
                /* no exact match: find closest by RGB distance */
                const uint8_t opts[4] = { 0, c1, c2, c3 };
                uint32_t rgb = DG_ScreenBuffer[(row*8+py)*DOOMGENERIC_RESX + col*8+px*2];
                int br=(rgb>>16)&0xFF, bg=(rgb>>8)&0xFF, bb=rgb&0xFF;
                int best_d = INT_MAX; code = 0;
                for (int k = 0; k < 4; k++) {
                    uint8_t oc = opts[k];
                    int dr=br-c64_r[oc], dg=bg-c64_g[oc], db=bb-c64_b[oc];
                    int dd = dr*dr + dg*dg + db*db;
                    if (dd < best_d) { best_d = dd; code = (uint8_t)k; }
                }
                (void)d0; (void)d3; /* suppress warnings */
            }
            bm = (uint8_t)((bm << 2) | code);
        }
        bm_out[py] = bm;
    }
}

/* ─── public DG_ interface ──────────────────────────────────────────────────── */

void DG_Init(void)
{
    build_lut();
    memset(prev_bm, 0, sizeof(prev_bm));
    memset(prev_sc, 0xFF, sizeof(prev_sc));  /* force full first frame */
    memset(prev_cr, 0xFF, sizeof(prev_cr));

    /* open pipes (Lua launcher creates them before us) */
    fprintf(stderr, "IDUN Doom: opening pipes...\n");
    fd_out = open(PIPE_TO_C64,   O_WRONLY);
    if (fd_out < 0) { perror("open " PIPE_TO_C64);   exit(1); }
    fd_in  = open(PIPE_FROM_C64, O_RDONLY | O_NONBLOCK);
    if (fd_in  < 0) { perror("open " PIPE_FROM_C64); exit(1); }
    fprintf(stderr, "IDUN Doom: pipes open, starting...\n");
}

void DG_DrawFrame(void)
{
    static uint32_t last_ms = 0;
    uint32_t now = ms_now();
    if (now - last_ms < (uint32_t)FRAME_MS) return;
    last_ms = now;

    /* encode all blocks and collect dirty ones */
    static uint8_t  new_bm[NUM_BLOCKS * 8];
    static uint8_t  new_sc[NUM_BLOCKS];
    static uint8_t  new_cr[NUM_BLOCKS];

    /* packet buffer: header(3) + max 1000 blocks × 12 + audio header(1) */
    static uint8_t  pkt[3 + NUM_BLOCKS * 12 + 1 + AUDIO_CHUNK];
    int pos = 0;
    int ndirty = 0;

    pkt[pos++] = 0x01;   /* frame start */
    pkt[pos++] = 0;      /* count_lo placeholder */
    pkt[pos++] = 0;      /* count_hi placeholder */

    for (int i = 0; i < NUM_BLOCKS; i++) {
        uint8_t bm[8], sc, cr;
        encode_block(i, bm, &sc, &cr);
        new_bm[i*8+0] = bm[0]; new_bm[i*8+1] = bm[1];
        new_bm[i*8+2] = bm[2]; new_bm[i*8+3] = bm[3];
        new_bm[i*8+4] = bm[4]; new_bm[i*8+5] = bm[5];
        new_bm[i*8+6] = bm[6]; new_bm[i*8+7] = bm[7];
        new_sc[i] = sc;
        new_cr[i] = cr;

        int dirty = (sc != prev_sc[i]) || (cr != prev_cr[i]) ||
                    memcmp(bm, &prev_bm[i*8], 8) != 0;
        if (dirty) {
            pkt[pos++] = (uint8_t)(i & 0xFF);
            pkt[pos++] = (uint8_t)(i >> 8);
            memcpy(&pkt[pos], bm, 8); pos += 8;
            pkt[pos++] = sc;
            pkt[pos++] = cr;
            ndirty++;
        }
    }

    /* fill in dirty count */
    pkt[1] = (uint8_t)(ndirty & 0xFF);
    pkt[2] = (uint8_t)(ndirty >> 8);

    /* no audio in v1 */
    pkt[pos++] = 0;

    /* send packet */
    write_all(fd_out, pkt, (size_t)pos);

    /* update previous frame state */
    memcpy(prev_bm, new_bm, sizeof(prev_bm));
    memcpy(prev_sc, new_sc, sizeof(prev_sc));
    memcpy(prev_cr, new_cr, sizeof(prev_cr));

    /* poll input while we're here */
    poll_input();
}

void DG_SleepMs(uint32_t ms)
{
    usleep(ms * 1000);
}

uint32_t DG_GetTicksMs(void)
{
    return ms_now();
}

int DG_GetKey(int *pressed, unsigned char *key)
{
    poll_input();
    if (kq_head == kq_tail) return 0;
    *pressed = key_queue[kq_head].pressed;
    *key     = key_queue[kq_head].key;
    kq_head  = (kq_head + 1) % KEY_QUEUE_SIZE;
    return 1;
}

int DG_MouseData(int *mb, int *rx, int *ry)
{
    (void)mb; (void)rx; (void)ry;
    return 0;   /* no mouse support */
}

void DG_SetWindowTitle(const char *title)
{
    (void)title;
}
