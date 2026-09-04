// ST7796 LCD output for the PC-98 framebuffer, on ESP-IDF's native esp_lcd.
//
// Forked from the ST7789 build. The SPI wiring is shared (MOSI 11 / SCLK 12 /
// CS 10 / DC 9 on SPI3) because the two modules are hooked up the same way apart
// from RESET, which the ST7789 board left unconnected and this one needs.
//
// This replaces the vendored TFT_eSPI. Only the transport (esp_lcd_panel_io_spi)
// comes from the IDF: the panel init sequence, the address window and the pixel
// push are done here, because the IDF's own ST7789 driver only speaks 16/18 bits
// per pixel and this port's default output is 12-bit RGB444 (see below) - the one
// thing that driver cannot express.
//
// The np2 framebuffer (verified clean) is 640x480 RGB565, pitch 640, of which
// 640x400 is the active PC-98 screen. By default we show it 2x-downscaled to
// 320x200, letterboxed inside 320x240; the 1:1 scaler mode pushes it unscaled.
//
// Byte order: SPI puts the high byte on the wire first, while RGB565 sits in
// memory little-endian, so every 16-bit pixel has to be swapped on the way out.
// TFT_eSPI did that in its push routines (setSwapBytes); here it happens while
// the output band is built, which costs nothing extra because the band is being
// written anyway. The RGB444 packer produces wire order directly.

#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <nvs.h>
#include <nvs_flash.h>

extern "C" {
#include <compiler.h>             // np2kai types (font.h needs them)
#include <i286c/cpumem.h>         // FONT_ADRS (FONTMEMORYBIND: fontrom = mem + FONT_ADRS)
#include <font/font.h>            // fontrom: the CGROM loaded from FONT.ROM
extern UINT8 mem[];               // np2kai main memory (cpumem.c)
}

extern "C" int ets_printf(const char *fmt, ...);

// ---- panel wiring (ST7789 240x320 on SPI2). Matches the README table. -------
// SPI3, not SPI2: Arduino's SD library owns SPI2 (FSPI) on this chip, and taking
// it here reconfigured the bus under the card - the symptom was an endless
// "sdCommand(): Card Failed!" and a panel that only ever showed the init wipe.
// TFT_eSPI was configured the same way (CONFIG_TFT_HSPI_PORT=y).
#define LCD_HOST        SPI3_HOST
#define PIN_MOSI        11
#define PIN_SCLK        12
#define PIN_CS          10
#define PIN_DC           9
// ST7796 modules bring RESET out and want a real pulse; the ST7789 board tied it
// off. Set to -1 if yours is strapped high.
#define PIN_RST         13
// RST and MISO are not wired on this module; BL goes straight to 3.3V.

// Source = the PSRAM framebuffer (platform_stubs.cpp: PC98_W x PC98_H).
#define SRC_W    640
#define SRC_H    480                 // framebuffer height (640x400 active + slack)
#define PC98_H   400                 // the active PC-98 screen inside it
// Destination size, per panel. 640x400 halves exactly onto a 320x240 panel; on
// the 480x320 one that would waste most of the glass, so it is scaled by 3/4
// instead and very nearly fills it. Both are computed at init into s_dst_*.
#define DST_W_MAX 480                // biggest destination width (buffer sizing)
#define DST_H_MAX 300
static int s_dst_w = 320, s_dst_h = 200;

// Native panel resolution, and the landscape size after MADCTL rotation.
// Panel table. Both modules share the SPI wiring; RESET is only brought out on
// the ST7796 one. Native portrait size - the landscape figures are these swapped.
#define PANEL_ST7789  0
#define PANEL_ST7796  1
#define PANEL_COUNT   2
static const struct {
    const char *name;
    int  w, h;          // native (portrait)
    bool has_rgb444;    // ST7796's COLMOD offers 65K/262K only - no 12-bit at all
} s_panels[PANEL_COUNT] = {
    { "ST7789", 240, 320, true  },
    { "ST7796", 320, 480, false },
};
static int s_panel = PANEL_ST7789;   // overridden from NVS in lcd_load_nvs()

// One SPI transfer per band rather than per row: fewer transactions is faster.
// The size is bounded by INTERNAL DMA memory, the scarcest resource on this
// board: the BLE controller takes ~44KB of it and leaves under 40KB for
// everything else (the SD driver's per-transfer bounce buffers included), so 32KB
// bands could not be allocated at all. 4KB x2 is 8 rows of RGB444 per transfer,
// ~25 transfers per frame, and that overhead is negligible next to the 96KB the
// frame itself takes on the wire. Two buffers, used alternately, because
// esp_lcd_panel_io_tx_color returns before the transfer has finished - the band
// being packed must never be the band on the wire.
#define BAND_BYTES      (4 * 1024)

static esp_lcd_panel_io_handle_t s_io = nullptr;
static uint16_t *s_img = nullptr;    // 320x200 downscaled frame (PSRAM, native order)
static uint8_t  *s_band[2] = { nullptr, nullptr };
static int       s_band_cur = 0;
static int s_pan_w = 320, s_pan_h = 240;   // landscape, set from the table at init

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
// Unlike TFT_eSPI, where the rate was a variable read at every transaction, here
// it is fixed when the panel IO is created - so a change tears the IO down and
// builds a new one. That is only ever done from the menu, with the emulator
// paused and no transfer in flight.
static const int s_spi_mhz[] = { 80, 40, 20 };
#define SPI_MHZ_COUNT   ((int)(sizeof(s_spi_mhz) / sizeof(s_spi_mhz[0])))
#define SPI_MHZ_DEFAULT 1                      // index of 40MHz

static int s_spi_idx = SPI_MHZ_DEFAULT;
static int s_panel12 = 0;                      // what COLMOD the panel is in now
static volatile int s_clear_req = 0;

extern "C" int  lcd_get_spi_idx(void)   { return s_spi_idx; }
extern "C" int  lcd_spi_idx_count(void) { return SPI_MHZ_COUNT; }
extern "C" int  lcd_spi_mhz(void)       { return s_spi_mhz[s_spi_idx]; }

static bool io_create(int mhz) {
    esp_lcd_panel_io_spi_config_t cfg = {};
    cfg.cs_gpio_num       = PIN_CS;
    cfg.dc_gpio_num       = PIN_DC;
    cfg.spi_mode          = 0;
    cfg.pclk_hz           = (unsigned)mhz * 1000000u;
    cfg.trans_queue_depth = 1;
    cfg.lcd_cmd_bits      = 8;
    cfg.lcd_param_bits    = 8;
    return esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &cfg, &s_io) == ESP_OK;
}

static inline void cmd(uint8_t c) { esp_lcd_panel_io_tx_param(s_io, c, NULL, 0); }
static inline void cmd1(uint8_t c, uint8_t a) { esp_lcd_panel_io_tx_param(s_io, c, &a, 1); }

// ST7789 bring-up. Rotation 1 (landscape): MADCTL MV|MX plus the BGR bit, which
// is what this module needs to show red as red. Inversion stays OFF - this is a
// non-inverted panel (the same choice TFT_eSPI was configured with here).
static void panel_init(void) {
    const bool is96 = (s_panel == PANEL_ST7796);
#if PIN_RST >= 0
    // Hardware reset first: some ST7796 modules ignore SWRESET until they have
    // seen a real RESET pulse after power-up.
    gpio_set_direction((gpio_num_t)PIN_RST, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level((gpio_num_t)PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(120));
#endif
    cmd(0x01);                            // SWRESET
    vTaskDelay(pdMS_TO_TICKS(150));
    cmd(0x11);                            // SLPOUT
    vTaskDelay(pdMS_TO_TICKS(120));
    // ST7796 keeps most of its registers behind a command-set lock. Plenty of
    // modules come up blank until it is opened, so unlock, then close it again.
    if (is96) {
        cmd1(0xf0, 0xc3);                 // CSCON: enable command set 2
        cmd1(0xf0, 0x96);
        cmd1(0xb4, 0x01);                 // inversion control: 1-dot inversion
        cmd1(0xf0, 0x3c);                 // CSCON: lock again
        cmd1(0xf0, 0x69);
    }
    // MADCTL MV|MX = 320x240 landscape. The BGR bit stays CLEAR: with it set, the
    // panel reads our RGB565 as BGR and red and blue trade places - visible as a
    // yellow menu cursor turning cyan, and on the emulator screen too (a DOS text
    // screen just does not show it). TFT_eSPI's CONFIG_TFT_BGR_ORDER=y is not the
    // same statement: that library also flips the order it writes.
    // BGR bit SET here, unlike the ST7789 build: with it clear the bring-up red
    // fill came out blue, i.e. the panel reads our RGB565 the other way round.
    // MX dropped: with it set the picture came out mirrored left to right. If a
    // future panel comes up upside-down instead, the other single-axis flip from
    // here is 0xE8 (MV|MX|MY|BGR).
    // Orientation and colour order differ per module: the ST7789 board here wants
    // MV|MX with RGB order, the ST7796 one MV with BGR (its red came out blue and
    // its picture mirrored until MX went away).
    cmd1(0x36, is96 ? 0xe8 : 0x60);       // MADCTL
    cmd1(0x3A, 0x55);                     // COLMOD: start in 16 bit
    s_panel12 = 0;
    // ST7796 panels are normally NOT inverted, unlike the ST7789 module this was
    // forked from. If the picture comes out as a negative, use 0x21 (INVON).
    // The ST7789 module is an inverted panel, the ST7796 one is not.
    cmd(is96 ? 0x20 : 0x21);              // INVOFF / INVON
    cmd(0x13);                            // NORON
    vTaskDelay(pdMS_TO_TICKS(10));
    cmd(0x29);                            // DISPON
    vTaskDelay(pdMS_TO_TICKS(120));
}

// Rebuild the transport at a new clock and re-run the init sequence, since a new
// IO handle means the panel has to be told its format again.
static void lcd_apply_spi_clock(void) {
    if (s_io) {
        esp_lcd_panel_io_del(s_io);
        s_io = nullptr;
    }
    if (!io_create(s_spi_mhz[s_spi_idx])) {
        ets_printf("lcd: panel IO re-create FAIL at %dMHz\n", s_spi_mhz[s_spi_idx]);
        return;
    }
    panel_init();
    s_clear_req = 1;
}

// Safe to call from the menu (core 1): the emulator is paused there, so the
// core-0 blit task is idle and no transaction is in flight.
extern "C" void lcd_set_spi_idx(int i) {
    if (i < 0 || i >= SPI_MHZ_COUNT) return;
    s_spi_idx = i;
    if (s_io) lcd_apply_spi_clock();       // before lcd_init() it is only a preset
}

// Power-on colour depth: 1 = 12-bit pixels (25% less SPI traffic, and lossless
// for the PC-98's 4-bit-per-channel palette), 0 = plain RGB565. This is only the
// value used when NVS holds no choice yet - the menu's setting is persisted and
// wins (some displays, notably an ST7789 *emulator* rather than the real
// controller, garble 12-bit pixels and must be able to stay on RGB565).
// ST7796 has no 12-bit pixel format at all - its COLMOD only offers 65K and 262K
// colours, unlike the ST7789 this was forked from. Asking for 0x53 left the panel
// showing nothing while 16-bit fills worked, which is what a blank emulator screen
// over a working red test fill looked like. Which panel is attached is a runtime
// choice, though, so that is a reason to refuse 12-bit on the ST7796 - not to
// build it out for both: s_panels[].has_rgb444 says which is which, and it is
// what lcd_load_nvs() and lcd_set_rgb444() below both go by.
#define RGB444_DEFAULT 1
static volatile int s_rgb444 = RGB444_DEFAULT;

extern "C" int  lcd_get_rgb444(void) { return s_rgb444; }

extern "C" int  lcd_get_panel(void)   { return s_panel; }
extern "C" int  lcd_panel_count(void) { return PANEL_COUNT; }
extern "C" const char *lcd_panel_name(void) { return s_panels[s_panel].name; }
// Takes effect on the next boot: the init sequence, the geometry and the buffers
// are all fixed at lcd_init() time. The menu row says so.
extern "C" void lcd_set_panel(int p) {
    if (p >= 0 && p < PANEL_COUNT) s_panel = p;
}

extern "C" void lcd_set_rgb444(int on) {
    if (!s_panels[s_panel].has_rgb444) {
        return;                            // ST7796: no 12-bit format to switch to
    }
    on = on ? 1 : 0;
    if (on == s_rgb444) return;
    s_rgb444 = on;
    s_clear_req = 1;                       // repaint everything in the new format
}

// Load the persisted clock BEFORE the panel is brought up, so even the init
// sequence goes out at the rate this display is known to tolerate. main.cpp's
// NVS block runs later (after the LCD is up), hence the local nvs_flash_init():
// it is idempotent, and if the partition needs erasing we just keep the default
// here and let main.cpp do the repair.
static void lcd_load_nvs(void) {
    if (nvs_flash_init() != ESP_OK) return;
    nvs_handle_t nh;
    if (nvs_open("pc98", NVS_READONLY, &nh) != ESP_OK) return;
    uint8_t v;
    if (nvs_get_u8(nh, "spiidx", &v) == ESP_OK && v < SPI_MHZ_COUNT) s_spi_idx = v;
    if (nvs_get_u8(nh, "panel", &v) == ESP_OK && v < PANEL_COUNT) s_panel = v;
    if (nvs_get_u8(nh, "rgb444", &v) == ESP_OK) s_rgb444 = v ? 1 : 0;
    // A panel without a 12-bit format shows nothing at all when asked for one,
    // and the menu that would let you change it back is drawn the same way - so
    // the stored choice cannot be honoured here.
    if (!s_panels[s_panel].has_rgb444) s_rgb444 = 0;
    nvs_close(nh);
}

// ---- address window + band push --------------------------------------------
static void set_window(int x, int y, int w, int h) {
    uint8_t p[4];
    p[0] = (uint8_t)(x >> 8); p[1] = (uint8_t)x;
    p[2] = (uint8_t)((x + w - 1) >> 8); p[3] = (uint8_t)(x + w - 1);
    esp_lcd_panel_io_tx_param(s_io, 0x2A, p, 4);          // CASET
    p[0] = (uint8_t)(y >> 8); p[1] = (uint8_t)y;
    p[2] = (uint8_t)((y + h - 1) >> 8); p[3] = (uint8_t)(y + h - 1);
    esp_lcd_panel_io_tx_param(s_io, 0x2B, p, 4);          // RASET
}

// first = true sends RAMWR (0x2C), which resets the write pointer to the window
// origin; the bands after it must use RAMWR_CONTINUE (0x3C) or each one would
// start over at the top left.
static void push_band(const uint8_t *data, size_t bytes, bool first) {
    esp_lcd_panel_io_tx_color(s_io, first ? 0x2C : 0x3C, data, bytes);
}

// Switch the panel's pixel format, only when it actually has to change.
static void set_colmod(int want12) {
    if (want12 == s_panel12) return;
    cmd1(0x3A, want12 ? 0x53 : 0x55);    // 65k RGB iface + 12 bit / 16 bit ctrl
    s_panel12 = want12;
}

// Pack a row of RGB565 into the panel's 12-bit format: two pixels per 3 bytes,
// RRRRGGGG BBBBRRRR GGGGBBBB. npx must be even (DST_W is 320).
//
// The panel is told to accept 12-bit pixels (COLMOD 0x53), so a frame is 1.5
// bytes/pixel instead of 2: 96,000 bytes instead of 128,000 for the 320x200
// image, i.e. 25% less time on the SPI bus. The colour cost is nil for real PC-98
// output: the 9801 analogue palette is 4 bits per channel (4096 colours), which
// is exactly RGB444 - only the intermediate shades the AVG/MID downscalers invent
// get quantised to 16 levels per channel.
static inline void pack444_row(const uint16_t *src, uint8_t *dst, int npx) {
    for (int i = 0; i < npx; i += 2) {
        uint16_t a = src[i], b = src[i + 1];
        dst[0] = (uint8_t)(((a >> 8) & 0xf0) | ((a >> 7) & 0x0f));   // Ra Ga
        dst[1] = (uint8_t)(((a << 3) & 0xf0) | ((b >> 12) & 0x0f));  // Ba Rb
        dst[2] = (uint8_t)(((b >> 3) & 0xf0) | ((b >> 1) & 0x0f));   // Gb Bb
        dst += 3;
    }
}

// Byte-swap a row of RGB565 into wire order.
static inline void pack565_row(const uint16_t *src, uint8_t *dst, int npx) {
    for (int i = 0; i < npx; i++) {
        dst[0] = (uint8_t)(src[i] >> 8);
        dst[1] = (uint8_t)(src[i]);
        dst += 2;
    }
}

// Push a w*h RGB565 image in the requested format, band by band. w must be even
// in 12-bit mode.
static bool push_rect(int x, int y, int w, int h, const uint16_t *img, int stride, int use12) {
    if (!s_io || !s_band[0] || w <= 0 || h <= 0) return false;
    if (use12 && (w & 1)) use12 = 0;                 // 12-bit packs pixel pairs
    const int rowbytes = use12 ? (w * 3 / 2) : (w * 2);
    if (rowbytes > BAND_BYTES) return false;
    int rows_per_band = BAND_BYTES / rowbytes;
    if (rows_per_band > h) rows_per_band = h;

    set_colmod(use12);
    set_window(x, y, w, h);
    bool first = true;
    for (int r0 = 0; r0 < h; r0 += rows_per_band) {
        int nrows = h - r0;
        if (nrows > rows_per_band) nrows = rows_per_band;
        uint8_t *band = s_band[s_band_cur];
        s_band_cur ^= 1;                     // the other one may still be in flight
        for (int r = 0; r < nrows; r++) {
            const uint16_t *srow = img + (size_t)(r0 + r) * stride;
            uint8_t *drow = band + (size_t)r * rowbytes;
            if (use12) pack444_row(srow, drow, w);
            else       pack565_row(srow, drow, w);
        }
        push_band(band, (size_t)nrows * rowbytes, first);
        first = false;
    }
    return true;
}

// Flat fill. Builds one band of the colour and repeats it, so no full-screen
// buffer is needed. Always 16-bit: fills are rare and not worth a format change.
static void fill_rect(int x, int y, int w, int h, uint16_t colour) {
    if (!s_io || !s_band[0] || w <= 0 || h <= 0) return;
    const int rowbytes = w * 2;
    if (rowbytes > BAND_BYTES) return;
    int rows_per_band = BAND_BYTES / rowbytes;
    if (rows_per_band > h) rows_per_band = h;
    uint8_t *band = s_band[0];
    for (int i = 0; i < rows_per_band * w; i++) {
        band[i * 2]     = (uint8_t)(colour >> 8);
        band[i * 2 + 1] = (uint8_t)colour;
    }
    set_colmod(0);
    set_window(x, y, w, h);
    bool first = true;
    for (int r0 = 0; r0 < h; r0 += rows_per_band) {
        int nrows = h - r0;
        if (nrows > rows_per_band) nrows = rows_per_band;
        // Re-using one buffer across bands is safe: trans_queue_depth is 1, so
        // the next call blocks until the previous transfer has been taken, and
        // the contents never change between bands anyway.
        push_band(band, (size_t)nrows * rowbytes, first);
        first = false;
    }
}

extern "C" bool lcd_init(void) {
    lcd_load_nvs();

    spi_bus_config_t bus = {};
    bus.mosi_io_num     = PIN_MOSI;
    bus.miso_io_num     = -1;
    bus.sclk_io_num     = PIN_SCLK;
    bus.quadwp_io_num   = -1;
    bus.quadhd_io_num   = -1;
    bus.max_transfer_sz = BAND_BYTES + 64;
    if (spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) {
        ets_printf("lcd: spi_bus_initialize FAIL\n");
        return false;
    }
    if (!io_create(s_spi_mhz[s_spi_idx])) {
        ets_printf("lcd: esp_lcd_new_panel_io_spi FAIL\n");
        return false;
    }

    // DMA-capable band buffers: internal RAM, since the SPI DMA reads them on
    // every band. 32KB x2, and they are plain allocations - nothing here needs a
    // large contiguous block the way the BLE controller does.
    // PSRAM is the fallback rather than a hard failure: the GDMA can reach it on
    // this chip, and a slower blit beats a black screen if the internal pool is
    // exhausted (which is exactly what a 32KB band ran into).
    for (int i = 0; i < 2; i++) {
        s_band[i] = (uint8_t *)heap_caps_malloc(BAND_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
        if (!s_band[i]) {
            s_band[i] = (uint8_t *)heap_caps_malloc(BAND_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (s_band[i]) ets_printf("lcd: band buffer %d in PSRAM (internal DMA pool full)\n", i);
        }
    }
    if (!s_band[0] || !s_band[1]) {
        ets_printf("lcd: band buffer alloc FAIL (%u bytes x2)\n", (unsigned)BAND_BYTES);
        return false;
    }

    panel_init();
    s_pan_w = s_panels[s_panel].h;           // landscape = native size swapped
    s_pan_h = s_panels[s_panel].w;
    // Largest whole-screen reduction that still fits, in quarters: 2:1 for a
    // 320x240 panel, 3:4 for a 480x320 one.
    if (s_pan_h >= 300 && s_pan_w >= 480) { s_dst_w = 480; s_dst_h = 300; }
    else                                  { s_dst_w = 320; s_dst_h = 200; }
    fill_rect(0, 0, s_pan_w, s_pan_h, 0x0000);

    s_img = (uint16_t *)heap_caps_malloc((size_t)DST_W_MAX * DST_H_MAX * 2,
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ets_printf("lcd: esp_lcd ST7789 init OK, size=%dx%d, spi=%dMHz, color=%s\n",
               s_pan_w, s_pan_h, s_spi_mhz[s_spi_idx], s_rgb444 ? "RGB444" : "RGB565");
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

static inline uint16_t avg2(uint16_t a, uint16_t b) {
    uint32_t r = (((a >> 11) & 31) + ((b >> 11) & 31)) >> 1;
    uint32_t g = (((a >> 5) & 63) + ((b >> 5) & 63)) >> 1;
    uint32_t u = ((a & 31) + (b & 31)) >> 1;
    return (uint16_t)((r << 11) | (g << 5) | u);
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

extern "C" int  lcd_get_scale_mode(void)   { return s_scale_mode; }
extern "C" int  lcd_scale_mode_count(void) { return SCALE_COUNT; }
extern "C" void lcd_set_scale_mode(int m) {
    if (m < 0 || m >= SCALE_COUNT) return;
    // The modes cover different areas of the panel, so the next blit wipes the
    // screen first or the old mode's pixels stay in the margins.
    if (m != s_scale_mode) s_clear_req = 1;
    s_scale_mode = m;
}

static void do_blit(const uint8_t *fb) {
    if (!fb) return;
    if (s_clear_req) {
        s_clear_req = 0;
        fill_rect(0, 0, s_pan_w, s_pan_h, 0x0000);
    }

    if (s_scale_mode == SCALE_1X) {
        // Clip to the panel: a display smaller than the PC-98 screen simply shows
        // its top-left corner. Rows go from PSRAM straight through the band
        // packer, so no scratch frame is involved.
        int w = (s_pan_w < SRC_W) ? s_pan_w : SRC_W;
        int h = (s_pan_h < SRC_H) ? s_pan_h : SRC_H;
        int x = (s_pan_w > SRC_W) ? (s_pan_w - SRC_W) / 2 : 0;
        int y = (s_pan_h > SRC_H) ? (s_pan_h - SRC_H) / 2 : 0;
        push_rect(x, y, w, h, (const uint16_t *)fb, SRC_W, s_rgb444);
        return;
    }

    if (!s_img) return;
    const uint16_t *src = (const uint16_t *)fb;
    const int mode = s_scale_mode;
    // Box filter over whatever ratio the panel asks for. The old code assumed a
    // fixed 2x2 block, which is why a 480x320 panel still got a 320x200 picture
    // in the middle of a lot of black: the ratio has to follow s_dst_*.
    for (int dy = 0; dy < s_dst_h; dy++) {
        const int sy0 = dy * PC98_H / s_dst_h;
        int sy1 = (dy + 1) * PC98_H / s_dst_h;
        if (sy1 <= sy0) sy1 = sy0 + 1;
        uint16_t *d = s_img + dy * s_dst_w;
        for (int dx = 0; dx < s_dst_w; dx++) {
            const int sx0 = dx * SRC_W / s_dst_w;
            int sx1 = (dx + 1) * SRC_W / s_dst_w;
            if (sx1 <= sx0) sx1 = sx0 + 1;
            uint32_t rs = 0, gs = 0, bs = 0, cnt = 0;
            uint16_t mn = 0xffff, mx = 0;
            for (int sy = sy0; sy < sy1; sy++) {
                const uint16_t *r = src + sy * SRC_W;
                for (int sx = sx0; sx < sx1; sx++) {
                    uint16_t p = r[sx];
                    rs += (p >> 11) & 31; gs += (p >> 5) & 63; bs += p & 31;
                    cnt++;
                    if (p < mn) mn = p;
                    if (p > mx) mx = p;
                }
            }
            uint16_t out;
            if (mode == SCALE_AVG) {
                out = (uint16_t)(((rs / cnt) << 11) | ((gs / cnt) << 5) | (bs / cnt));
            } else if (mode == SCALE_MID) {
                out = avg2(mn, mx);
            } else {
                out = mx;
            }
            d[dx] = out;
        }
    }
    const int x0 = (s_pan_w - s_dst_w) / 2, y0 = (s_pan_h - s_dst_h) / 2;
    push_rect(x0, y0, s_dst_w, s_dst_h, s_img, s_dst_w, s_rgb444);
}

// ---- disk-menu text UI (menu_disk.cpp). Drawn on core 1 while the emulator is
// paused; the async blit task is idle then (no lcd_blit calls), so touching the
// panel directly here is safe.
//
// TFT_eSPI brought its own fonts; the IDF has none, so the glyphs come from the
// PC-98 CGROM that FONT.ROM already provides (8x16 ANK at FONT_ANK16). The two
// scanlines of each glyph row are OR-ed together into an 8x8 cell, which keeps
// the 10-pixel row pitch menu_disk.cpp lays out - the old 6x8 font sat in the
// same pitch, so every row index and page size in the menu is unchanged. The cell
// is 8 wide rather than 6, so a line holds 40 characters instead of 53.
#define MENU_CH_W   8
#define MENU_CH_H   8
#define MENU_ROW_H  10
#define FONT_ANK16  0x80000        // 8x16 ANK block in the CGROM

extern "C" void lcd_menu_clear(void) {
    fill_rect(0, 0, s_pan_w, s_pan_h, 0x0000);
}

extern "C" void lcd_menu_line(int row, const char *s, uint16_t fg, uint16_t bg) {
    if (!s || row < 0 || (row + 1) * MENU_ROW_H > s_pan_h) return;
    if (!s_band[0] || !s_img) return;
    const int w = (s_pan_w > DST_W_MAX) ? DST_W_MAX : s_pan_w;
    const int cols = w / MENU_CH_W;
    // Render the whole row into the (idle) downscale buffer and push it in one
    // go: the emulator is paused, so s_img is free, and one transfer per row
    // beats ten single-line ones.
    uint16_t *cell = s_img;
    for (int gy = 0; gy < MENU_ROW_H; gy++) {
        uint16_t *line = cell + (size_t)gy * w;
        for (int i = 0; i < w; i++) line[i] = bg;
        if (gy >= MENU_CH_H) continue;       // the gap between rows stays background
        for (int c = 0; c < cols; c++) {
            uint8_t ch = (uint8_t)s[c];
            if (!ch) break;
            if (ch < 0x20 || ch > 0x7e) ch = ' ';
            const uint8_t *glyph = fontrom + FONT_ANK16 + (unsigned)ch * 16;
            uint8_t bits = (uint8_t)(glyph[gy * 2] | glyph[gy * 2 + 1]);
            for (int bx = 0; bx < MENU_CH_W; bx++)
                if (bits & (0x80 >> bx)) line[c * MENU_CH_W + bx] = fg;
        }
    }
    push_rect(0, row * MENU_ROW_H, w, MENU_ROW_H, cell, w, s_rgb444);
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
