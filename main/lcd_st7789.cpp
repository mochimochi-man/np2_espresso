// ST7789 LCD output for the PC-98 framebuffer (milestone M3) via TFT_eSPI.
//
// Uses the exact library + settings that the Anemoia-ESP32 project runs on this
// hardware (ST7789 240x320 on HSPI, rotation 1 -> 320x240, BGR, byte-swapped,
// inversion off). Config comes from the TFT_eSPI Kconfig options in sdkconfig
// (CONFIG_TFT_*). TFT_eSPI owns the init sequence / MADCTL / offsets, so we don't
// hand-tune any of that here.
//
// The np2 framebuffer (verified clean) is 640x480 RGB565, pitch 640, of which
// 640x400 is the active PC-98 screen. By default we show it 2x-downscaled to
// 320x200, letterboxed inside 320x240; the 1:1 scaler mode pushes it unscaled.
//
// Panel geometry is read from TFT_eSPI at init rather than hardcoded, so both
// the downscaled and the 1:1 output land correctly on a larger panel too.

#include <Arduino.h>
#include <TFT_eSPI.h>
#include "esp_heap_caps.h"

extern "C" int ets_printf(const char *fmt, ...);

// Source = the PSRAM framebuffer (platform_stubs.cpp: PC98_W x PC98_H).
#define SRC_W    640
#define SRC_H    480                 // framebuffer height (640x400 active + slack)
#define DST_W    320                 // 640 / 2  (whole width shown)
#define DST_H    200                 // 400 / 2

static TFT_eSPI  tft;
static uint16_t *s_img = nullptr;    // 320x200 downscaled frame (PSRAM)
static int s_pan_w = 320, s_pan_h = 240;   // panel size, read from TFT_eSPI at init

// ---- async blit: core 0 worker ----------------------------------------------
// lcd_blit() only hands the framebuffer pointer to a core-0 task (notify) and
// returns immediately, so the ~8-16ms downscale+SPI push no longer stalls the
// emulator on core 1. Priority 4 < audio task 6, so audio is never preempted.
static TaskHandle_t s_blit_task = nullptr;
static volatile const uint8_t *s_fb = nullptr;   // handoff: frame to blit
static volatile int s_blit_busy = 0;             // 1 while core-0 blit is reading fb

static void blit_task(void *arg);

extern "C" bool lcd_init(void) {
    tft.init();
    tft.setRotation(1);              // landscape 320x240
    tft.setSwapBytes(true);          // SCREEN_SWAP_BYTES: swap RGB565 bytes on push
    tft.fillScreen(TFT_BLACK);
    s_pan_w = tft.width();
    s_pan_h = tft.height();

    s_img = (uint16_t *)heap_caps_malloc((size_t)DST_W * DST_H * 2,
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ets_printf("lcd: TFT_eSPI init OK, size=%dx%d\n", tft.width(), tft.height());
    xTaskCreatePinnedToCore(blit_task, "blit", 4096, nullptr, 4, &s_blit_task, 0);
    return s_img != nullptr && s_blit_task != nullptr;
}

// Downscale the whole 640x400 screen 2x -> 320x200 (letterboxed in 320x240) so
// the entire DOS screen (incl. the prompt/cursor at the bottom) is visible. Each
// dest pixel is the box average of its 2x2 source block. (The previous "brightest
// of 2x2" rule kept white-on-black text but erased black-on-white text such as
// the function-key display line; averaging keeps both, as gray strokes.)
static inline uint16_t avg4(uint16_t a, uint16_t b, uint16_t c, uint16_t d) {
    uint32_t r = ((a >> 11) & 31) + ((b >> 11) & 31) + ((c >> 11) & 31) + ((d >> 11) & 31);
    uint32_t g = ((a >> 5) & 63) + ((b >> 5) & 63) + ((c >> 5) & 63) + ((d >> 5) & 63);
    uint32_t u = (a & 31) + (b & 31) + (c & 31) + (d & 31);
    return (uint16_t)(((r >> 2) << 11) | ((g >> 2) << 5) | (u >> 2));
}

// Scaler modes (runtime-switchable from the disk menu):
//   0 MAX - brightest of the 2x2 block (original; white-on-black text kept,
//           black-on-white text erased)
//   1 AVG - box average of the 2x2 block (default; both polarities survive)
//   2 MID - midpoint of the darkest and brightest block pixels (keeps stroke
//           contrast better than AVG on thin text)
//   3 1X  - no scaling at all: one framebuffer pixel per panel pixel. On the
//           320x240 ST7789 only the top-left corner of the PC-98 screen is
//           visible, so this is meant for a future 640x480 panel, where it
//           shows the whole screen at true resolution (1px text strokes intact,
//           which no 2x downscale can preserve).
#define SCALE_MAX   0
#define SCALE_AVG   1
#define SCALE_MID   2
#define SCALE_1X    3
#define SCALE_COUNT 4
static volatile int s_scale_mode = SCALE_AVG;
// Set on a mode change: the modes cover different areas of the panel, so the
// next blit wipes the screen first or the old mode's pixels stay in the margins.
static volatile int s_clear_req = 0;

extern "C" int  lcd_get_scale_mode(void)   { return s_scale_mode; }
extern "C" int  lcd_scale_mode_count(void) { return SCALE_COUNT; }
extern "C" void lcd_set_scale_mode(int m) {
    if (m < 0 || m >= SCALE_COUNT) return;
    if (m != s_scale_mode) s_clear_req = 1;
    s_scale_mode = m;
}

static inline uint16_t avg2(uint16_t a, uint16_t b) {
    uint32_t r = (((a >> 11) & 31) + ((b >> 11) & 31)) >> 1;
    uint32_t g = (((a >> 5) & 63) + ((b >> 5) & 63)) >> 1;
    uint32_t u = ((a & 31) + (b & 31)) >> 1;
    return (uint16_t)((r << 11) | (g << 5) | u);
}

static void do_blit(const uint8_t *fb) {
    if (!fb) return;
    if (s_clear_req) { s_clear_req = 0; tft.fillScreen(TFT_BLACK); }

    if (s_scale_mode == SCALE_1X) {
        // pushImage clips against the viewport, so a panel smaller than the
        // PC-98 screen simply shows its top-left corner (no scratch buffer and
        // no per-pixel work: rows go straight from PSRAM to the SPI bus).
        int x = (s_pan_w > SRC_W) ? (s_pan_w - SRC_W) / 2 : 0;
        int y = (s_pan_h > SRC_H) ? (s_pan_h - SRC_H) / 2 : 0;
        tft.pushImage(x, y, SRC_W, SRC_H, (uint16_t *)fb);
        return;
    }

    if (!s_img) return;
    const uint16_t *src = (const uint16_t *)fb;
    for (int dy = 0; dy < DST_H; dy++) {
        const uint16_t *r0 = src + (dy * 2) * SRC_W;
        const uint16_t *r1 = r0 + SRC_W;
        uint16_t *d = s_img + dy * DST_W;
        const int mode = s_scale_mode;
        for (int dx = 0; dx < DST_W; dx++) {
            int sx = dx * 2;
            uint16_t p00 = r0[sx], p01 = r0[sx + 1], p10 = r1[sx], p11 = r1[sx + 1];
            uint16_t out;
            if (mode == SCALE_AVG) {
                out = avg4(p00, p01, p10, p11);
            } else {
                uint16_t mn = p00, mx = p00;
                if (p01 < mn) mn = p01; else mx = p01;
                if (p10 < mn) mn = p10; else if (p10 > mx) mx = p10;
                if (p11 < mn) mn = p11; else if (p11 > mx) mx = p11;
                out = (mode == SCALE_MID) ? avg2(mn, mx) : mx;
            }
            d[dx] = out;
        }
    }
    tft.pushImage((s_pan_w - DST_W) / 2, (s_pan_h - DST_H) / 2, DST_W, DST_H, s_img);
}

// ---- disk-menu text UI (menu_disk.cpp). Drawn on core 1 while the emulator
// is paused; the async blit task is idle then (no lcd_blit calls), so direct
// tft access here is safe.
extern "C" void lcd_menu_clear(void) {
    tft.fillScreen(TFT_BLACK);
}

extern "C" void lcd_menu_line(int row, const char *s, uint16_t fg, uint16_t bg) {
    if (row < 0 || (row + 1) * 10 > s_pan_h) return;   // GLCD font (6x8) = 10px rows
    tft.setTextFont(1);                          // small font: matches the 2x-downscaled emu screen
    tft.setTextSize(1);
    tft.fillRect(0, row * 10, s_pan_w, 10, bg);
    tft.setTextColor(fg, bg);
    tft.drawString(s, 4, row * 10 + 1, 1);
}

static void blit_task(void *arg) {
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        // Drain every queued frame (latest wins) BEFORE clearing busy: if a new
        // frame was handed over while we were blitting, busy must stay 1 or the
        // core-1 tear-free guard (while (lcd_blit_busy())) passes and lets
        // drawscreen rewrite fb under our feet -> corrupted LCD output.
        while (s_fb) {
            const uint8_t *fb = (const uint8_t *)s_fb;
            s_fb = nullptr;
            do_blit(fb);
        }
        s_blit_busy = 0;
        // lcd_blit() may have queued a frame just after the last s_fb check; it
        // also left a pending notify, so re-arm busy and let the top loop take it.
        if (s_fb) s_blit_busy = 1;
    }
}

// Non-blocking: enqueue the frame for the core-0 blit task and return.
// A frame already queued is overwritten (latest wins).
extern "C" void lcd_blit(const uint8_t *fb) {
    if (!s_blit_task || !fb) return;
    s_blit_busy = 1;
    s_fb = fb;
    xTaskNotifyGive(s_blit_task);
}

// Tear-free sync: the emulator loop waits for this to go false before letting
// drawscreen touch the framebuffer again (normally already done = zero wait).
extern "C" bool lcd_blit_busy(void) { return s_blit_busy != 0; }
