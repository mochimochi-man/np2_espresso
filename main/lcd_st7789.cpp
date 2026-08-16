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
#include <nvs.h>
#include <nvs_flash.h>

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

// ---- SPI write clock ---------------------------------------------------------
// 80MHz is the fastest the ESP32-S3 SPI peripheral reaches from its 80MHz source
// and is what the blit throughput likes, but it is out of spec for ST7789 and
// only survives on short, clean wiring: some panels/cables show tearing, dropped
// or shifted pixels, or a blank screen. 40MHz is the safe default; 20MHz is the
// last resort. All three are exact divisors of the 80MHz source, so the
// peripheral runs at exactly these rates.
// The setting lives in TFT_eSPI's tft_spi_write_freq, which is read at every
// transaction start, so a change applies from the next drawing call - no re-init.
static const int s_spi_mhz[] = { 80, 40, 20 };
#define SPI_MHZ_COUNT   ((int)(sizeof(s_spi_mhz) / sizeof(s_spi_mhz[0])))
#define SPI_MHZ_DEFAULT 1                      // index of 40MHz

static int s_spi_idx = SPI_MHZ_DEFAULT;

extern "C" int  lcd_get_spi_idx(void)   { return s_spi_idx; }
extern "C" int  lcd_spi_idx_count(void) { return SPI_MHZ_COUNT; }
extern "C" int  lcd_spi_mhz(void)       { return s_spi_mhz[s_spi_idx]; }

// Safe to call from the menu (core 1): the emulator is paused there, so the
// core-0 blit task is idle and no transaction is in flight.
extern "C" void lcd_set_spi_idx(int i) {
    if (i < 0 || i >= SPI_MHZ_COUNT) return;
    s_spi_idx = i;
    tft_spi_write_freq = (uint32_t)s_spi_mhz[i] * 1000000u;
}

// Power-on colour depth (see the RGB444 section further down): 1 = 12-bit
// pixels (25% less SPI traffic, and lossless for the PC-98's 4-bit-per-channel
// palette), 0 = plain RGB565. This is only the value used when NVS holds no
// choice yet - the menu's setting is persisted and wins (some displays, notably
// an ST7789 *emulator* rather than the real controller, garble 12-bit pixels and
// must be able to stay on RGB565 across reboots).
#define RGB444_DEFAULT 1
static volatile int s_rgb444 = RGB444_DEFAULT;

// Load the persisted clock BEFORE tft.init(), so even the panel init sequence
// goes out at the rate this display is known to tolerate. main.cpp's NVS block
// runs later (after the LCD is up), hence the local nvs_flash_init(): it is
// idempotent, and if the partition needs erasing we just keep the default here
// and let main.cpp do the repair.
static void lcd_load_nvs(void) {
    if (nvs_flash_init() != ESP_OK) return;
    nvs_handle_t nh;
    if (nvs_open("pc98", NVS_READONLY, &nh) != ESP_OK) return;
    uint8_t v;
    if (nvs_get_u8(nh, "spiidx", &v) == ESP_OK && v < SPI_MHZ_COUNT) lcd_set_spi_idx(v);
    // Colour depth is persisted too: a panel that cannot do 12-bit pixels (an
    // ST7789 *emulator* rather than the real controller, for instance) has to be
    // able to stay on RGB565 across a reboot.
    if (nvs_get_u8(nh, "rgb444", &v) == ESP_OK) s_rgb444 = v ? 1 : 0;
    nvs_close(nh);
}

extern "C" bool lcd_init(void) {
    lcd_set_spi_idx(SPI_MHZ_DEFAULT);   // ignore whatever SPI_FREQUENCY was built in
    lcd_load_nvs();
    tft.init();
    tft.setRotation(1);              // landscape 320x240
    tft.setSwapBytes(true);          // SCREEN_SWAP_BYTES: swap RGB565 bytes on push
    tft.fillScreen(TFT_BLACK);
    s_pan_w = tft.width();
    s_pan_h = tft.height();

    s_img = (uint16_t *)heap_caps_malloc((size_t)DST_W * DST_H * 2,
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ets_printf("lcd: TFT_eSPI init OK, size=%dx%d, spi=%dMHz, color=%s\n",
               tft.width(), tft.height(), s_spi_mhz[s_spi_idx],
               s_rgb444 ? "RGB444" : "RGB565");
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

// ---- RGB444 (12 bit/pixel) output ------------------------------------------
// The panel is told to accept 12-bit pixels (COLMOD 0x53), so a frame is 1.5
// bytes/pixel instead of 2: 96,000 bytes instead of 128,000 for the 320x200
// image, i.e. 25% less time on the SPI bus.
// Colour cost is nil for real PC-98 output: the 9801 analogue palette is 4 bits
// per channel (4096 colours), which is exactly RGB444 - only the intermediate
// shades the AVG/MID downscalers invent get quantised to 16 levels per channel.
//
// Deliberately NOT persisted in NVS: if a panel dislikes 12-bit mode the screen
// becomes unreadable, and a setting that survived the reboot would make it
// unrecoverable without a re-flash. Toggle it in the disk menu.
// (s_rgb444 / RGB444_DEFAULT are declared above, next to the SPI clock, because
// lcd_init logs the mode.)
static int s_panel12 = 0;                    // what COLMOD the panel is in now

extern "C" int  lcd_get_rgb444(void) { return s_rgb444; }
extern "C" void lcd_set_rgb444(int on) {
    on = on ? 1 : 0;
    if (on == s_rgb444) return;
    s_rgb444 = on;
    s_clear_req = 1;                         // repaint everything in the new format
}

// Switch the panel's pixel format, only when it actually has to change.
static void set_colmod(int want12) {
    if (want12 == s_panel12) return;
    tft.writecommand(0x3A);                  // COLMOD
    tft.writedata(want12 ? 0x53 : 0x55);     // 65k RGB iface + 12 bit / 16 bit ctrl
    s_panel12 = want12;
}

// Pack a row of RGB565 into the panel's 12-bit format: two pixels per 3 bytes,
// RRRRGGGG BBBBRRRR GGGGBBBB. npx must be even (DST_W is 320).
//
// swap: the source holds byte-swapped RGB565. TFT_eSprite stores its pixels that
// way (ready for the SPI bus), so reading a sprite as plain RGB565 turns yellow
// (0xFFE0 -> 0xE0FF) into magenta. The emulator's own scratch image is native
// order and passes false.
static inline void pack444_row(const uint16_t *src, uint8_t *dst, int npx, bool swap) {
    for (int i = 0; i < npx; i += 2) {
        uint16_t a = src[i], b = src[i + 1];
        if (swap) { a = (uint16_t)((a >> 8) | (a << 8)); b = (uint16_t)((b >> 8) | (b << 8)); }
        dst[0] = (uint8_t)(((a >> 8) & 0xf0) | ((a >> 7) & 0x0f));   // Ra Ga
        dst[1] = (uint8_t)(((a << 3) & 0xf0) | ((b >> 12) & 0x0f));  // Ba Rb
        dst[2] = (uint8_t)(((b >> 3) & 0xf0) | ((b >> 1) & 0x0f));   // Gb Bb
        dst += 3;
    }
}

// Push a w*h RGB565 image as 12-bit pixels, one row at a time (the packing
// buffer stays small and no second frame buffer is needed). w must be even.
static void push_rect444(int x, int y, int w, int h, const uint16_t *img, int stride,
                         bool swap) {
    static uint8_t row[DST_W * 3 / 2];
    if (w > DST_W) return;
    SPIClass &spi = TFT_eSPI::getSPIinstance();
    tft.startWrite();
    tft.setAddrWindow(x, y, w, h);           // one window: the panel auto-increments
    for (int r = 0; r < h; r++) {
        pack444_row(img + (size_t)r * stride, row, w, swap);
        spi.writeBytes(row, (size_t)w * 3 / 2);
    }
    tft.endWrite();
}

static void push_image444(int x, int y, const uint16_t *img) {
    push_rect444(x, y, DST_W, DST_H, img, DST_W, false);
}

static void do_blit(const uint8_t *fb) {
    if (!fb) return;
    if (s_clear_req) {
        s_clear_req = 0;
        set_colmod(0);                       // fillScreen writes 16-bit pixels
        tft.fillScreen(TFT_BLACK);
    }

    if (s_scale_mode == SCALE_1X) {
        // pushImage clips against the viewport, so a panel smaller than the
        // PC-98 screen simply shows its top-left corner (no scratch buffer and
        // no per-pixel work: rows go straight from PSRAM to the SPI bus).
        int x = (s_pan_w > SRC_W) ? (s_pan_w - SRC_W) / 2 : 0;
        int y = (s_pan_h > SRC_H) ? (s_pan_h - SRC_H) / 2 : 0;
        set_colmod(0);                   // 1:1 pushes the RGB565 framebuffer as-is
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
    const int x0 = (s_pan_w - DST_W) / 2, y0 = (s_pan_h - DST_H) / 2;
    if (s_rgb444) {
        set_colmod(1);
        push_image444(x0, y0, s_img);
    } else {
        set_colmod(0);
        tft.pushImage(x0, y0, DST_W, DST_H, s_img);
    }
}

// ---- disk-menu text UI (menu_disk.cpp). Drawn on core 1 while the emulator
// is paused; the async blit task is idle then (no lcd_blit calls), so direct
// tft access here is safe.
//
// In RGB444 mode the menu is NOT drawn straight to the panel. TFT_eSPI's text
// and rectangle primitives emit 16-bit pixels, and switching the panel back to
// 16 bit for them (COLMOD 0x55) does not take on every display - one panel here
// keeps decoding at 12 bit and the menu comes out as coloured noise with a
// striped band, while the 12-bit emulator image on the same panel is perfect.
// So the menu is rendered into an off-screen sprite with the same primitives and
// then pushed through the 12-bit path: the panel never changes format at all.
static TFT_eSprite s_menu_spr = TFT_eSprite(&tft);
static bool s_menu_spr_ok = false;

static bool menu_sprite(void) {
    if (s_menu_spr_ok) return true;
    s_menu_spr.setColorDepth(16);
    if (!s_menu_spr.createSprite(s_pan_w, s_pan_h)) {   // PSRAM: 320x240x2 = 150KB
        ets_printf("lcd: menu sprite alloc FAIL (menu falls back to RGB565)\n");
        return false;
    }
    s_menu_spr_ok = true;
    return true;
}

extern "C" void lcd_menu_clear(void) {
    if (s_rgb444 && menu_sprite()) {
        s_menu_spr.fillSprite(TFT_BLACK);
        push_rect444(0, 0, s_pan_w, s_pan_h, (const uint16_t *)s_menu_spr.getPointer(), s_pan_w, true);
        return;
    }
    set_colmod(0);           // the menu is drawn with TFT_eSPI's 16-bit primitives
    tft.fillScreen(TFT_BLACK);
}

extern "C" void lcd_menu_line(int row, const char *s, uint16_t fg, uint16_t bg) {
    if (row < 0 || (row + 1) * 10 > s_pan_h) return;   // GLCD font (6x8) = 10px rows
    if (s_rgb444 && menu_sprite()) {
        s_menu_spr.setTextFont(1);
        s_menu_spr.setTextSize(1);
        s_menu_spr.fillRect(0, row * 10, s_pan_w, 10, bg);
        s_menu_spr.setTextColor(fg, bg);
        s_menu_spr.drawString(s, 4, row * 10 + 1, 1);
        // Only the row that changed goes to the panel (10 lines = ~1ms at 40MHz).
        const uint16_t *buf = (const uint16_t *)s_menu_spr.getPointer();
        push_rect444(0, row * 10, s_pan_w, 10, buf + (size_t)(row * 10) * s_pan_w, s_pan_w, true);
        return;
    }
    set_colmod(0);
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
