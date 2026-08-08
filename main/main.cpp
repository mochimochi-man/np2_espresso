// PC-98 (np2kai i386c) on ESP32-S3 — first integration / boot test (M2c).
#include <Arduino.h>
#include <SD.h>
#include "esp_heap_caps.h"
#include "driver/gpio.h"
#include <nvs.h>
#include <nvs_flash.h>

extern "C" {
#include <compiler.h>
#include <pccore.h>
#include <diskdrv.h>
#include <keystat.h>
#include <dosio.h>
#include <io/iocore.h>
#include <vram/dispsync.h>
#include <statsave.h>
#include <fdd/sxsi.h>
#include <sound/sound.h>
#include <sound/pcm86.h>
#include <sound/beep.h>
#include <nevent.h>
#include <cbus/mpu98ii.h>
#include <cbus/smpu98.h>
#include <io/serial.h>
#include <io/gdc.h>
void mouseif_changeclock(void);   // io/mouseif.h has a C/C++ typedef clash; declared here instead
#include <cpucore.h>              // CPU_CLOCK/BASECLOCK/REMCLOCK (emulated cycle counter)
#include <font/font.h>               // fontrom (CGROM) for the boot-phase text renderer
#include <common/milstr.h>
BOOL fdd_diskready(REG8 drv);      // diskimage/fddfile.h
uint8_t *pc98_framebuffer(void);   // from platform_stubs.cpp
bool pc98_scrnmng_init(void);      // pre-allocate the framebuffer (PSRAM) before pccore_init
extern int g_pc98_draw_enabled;    // pccore.c: screen-render gate (off during boot)
bool lcd_init(void);               // lcd_st7789.cpp: ST7789 on HSPI
void lcd_blit(const uint8_t *fb);  // blit framebuffer -> panel (async, core 0)
bool lcd_blit_busy(void);            // lcd_st7789.cpp: core-0 blit still reading fb
void lcd_set_scale_mode(int m);      // lcd_st7789.cpp: 0=MAX 1=AVG 2=MID 3=1:1
int  lcd_scale_mode_count(void);     // lcd_st7789.cpp: valid modes are 0..count-1
void usb_kbd_init(void);           // usb_kbd.cpp: USB HID keyboard host
int  usb_kbd_pop(uint8_t *nkey, uint8_t *down);  // drain one key event
extern volatile int g_menu_req;      // usb_kbd.cpp: Pause/Break -> disk swap menu
extern volatile int g_speed_req;     // menu_disk.cpp: CPU clock row -> new multiple (1..5)
void menu_disk_run(void);            // menu_disk.cpp: modal disk UI (pauses emulation)
extern uint8_t mem[];              // core main RAM (i386c cpumem.c); text VRAM at 0xA0000
bool audio_init(int rate, int maxframes);        // audio_i2s.cpp: I2S DAC output
void audio_write_s32(const int32_t *pcm, int frames);
extern volatile int g_audio_peak;  // audio_i2s.cpp: max |sample| (>32767 = clipping)
extern volatile int g_audio_drops; // audio_i2s.cpp: frames dropped (ring overflow)
extern volatile int g_audio_under; // audio_i2s.cpp: ring-empty events (DAC underrun)
extern volatile int g_np2_snd_gen;   // np2 sound.c: samples rendered (diag)
extern volatile int g_np2_snd_skip;  // np2 sound.c: samples discarded, buffer full (diag)
extern volatile int g_np2_snd_locks; // np2 sound.c: sound_pcmlock calls (diag)
extern volatile long long g_prof_loop, g_prof_cb, g_prof_snd, g_prof_draw; // pccore.c phases (diag)
extern int g_sound_enable;         // platform_stubs.cpp: gates soundmng_create
}

// ---- FM sound (milestone ④, PC-9801-26K / YM2203) ----
// Default OFF: the emulator runs well below real time (~8-27% RT), so FM audio
// would be pitch-shifted / underrunning, and FM synthesis further slows the
// already CPU-bound emulation. The whole path (26K board + I2S out on 38/39/40)
// is implemented and boots when enabled — flip this to 1 to try it. Audio output
// correctness is unverified (no way to listen; DAC hardware on S3 unconfirmed).
#define ENABLE_FM_SOUND   1
#define FM_RATE           22050
#define FM_DELAY_MS       20
static int g_fm_frames = 0;        // stereo frames per sound_pcmlock block

// ---- periodic perf/diag serial log (every 300 frames) ----
// ets_printf writes to the UART synchronously from the emulator core, so this
// steady output costs latency. Keep OFF for normal builds; flip to 1 to measure.
#define DEBUG_PERF_LOG  0

// ---- real-time pacing (throttle) ----
// The i286c core can run FASTER than a real VM21 (BIOS phase hit ~10x), which
// makes VSYNC-timed software run too fast and FM audio play at the wrong tempo.
// Pace the loop so emulated time advances at real time: after each frame, sleep
// until wall-clock catches up to the emulated cycles executed (cycles/realclock).
// Heavy frames that can't keep up simply run flat-out (no negative sleep); a debt
// clamp stops them from banking "fast time" credit afterwards.
#define ENABLE_REALTIME   1

// Output via ets_printf: writes straight to the UART ROM routine, bypassing the
// stdio/VFS layer (which crashed after the emulator ran). No %f support → integers.
extern "C" int ets_printf(const char *fmt, ...);
#define printf ets_printf

// Boot-phase text renderer. The BIOS_IO_EMULATION BIOS writes the text VRAM
// directly without programming the GDC, so np2's drawscreen composites nothing
// until DOS/a game sets a video mode — the memory count and the EPSON [SHIFT]
// message never reach the LCD. While drawcount is still 0 (no real composite
// has ever happened), render the 80x25 text VRAM into the framebuffer with the
// CGROM font (8x16 ANK cells) so the boot sequence shows like on real hardware.
static void render_boot_text(uint8_t *fb) {
    uint16_t *d = (uint16_t *)fb;
    for (int cy = 0; cy < 25; cy++) {
        for (int cx = 0; cx < 80; cx++) {
            uint8_t code = mem[0xA0000 + (cy * 80 + cx) * 2];
            const uint8_t *glyph = fontrom + (unsigned)code * 16;
            for (int r = 0; r < 16; r++) {
                uint8_t bits = glyph[r];
                uint16_t *px = d + (cy * 16 + r) * 640 + cx * 8;
                for (int b = 0; b < 8; b++)
                    px[b] = (bits & (0x80 >> b)) ? 0xFFFF : 0x0000;
            }
        }
    }
}

// Dump PC-98 text VRAM (80x25, 2 bytes/char at 0xA0000) as ASCII — the reliable
// "did DOS boot?" signal (independent of the LCD/scrnmng path).
static void dump_text_vram(void) {
    printf("---- text VRAM (80x25) ----\n");
    for (int row = 0; row < 25; row++) {
        char line[81];
        int nonblank = 0;
        for (int col = 0; col < 80; col++) {
            uint8_t c = mem[0xA0000 + (row * 80 + col) * 2];
            if (c >= 0x20 && c < 0x7f) { line[col] = (char)c; nonblank = 1; }
            else line[col] = ' ';
        }
        line[80] = 0;
        if (nonblank) printf("|%s|\n", line);
    }
    printf("---------------------------\n");
}

// SD wiring — Anemoia config.h (ESP32-S3, FSPI)
static const int SD_CS = 4, SD_SCK = 5, SD_MISO = 6, SD_MOSI = 7;
static SPIClass sdspi(FSPI);

static void banner(const char *s) { printf("\n==== %s ====\n", s); }

// Disk image paths on the SD card.
#define IMG_FDD1  "/FD.NFD"
#define IMG_FDD2  "/FD2.NFD"
#define IMG_HDD   "/HDD.NHD"

// Boot media comes from NVS (the disk menu persists FDD1/FDD2/HDD on ESC/RESET).
// With no saved disk data, nothing is mounted and the BIOS drops to N88-BASIC
// (BIOS.ROM). Otherwise every saved image that exists on the SD is mounted, and
// the PC-98 BIOS boots the first of FDD1 -> FDD2 -> HDD that carries bootable
// media (BIOS scans the FDDs before the HDD).

// True if a file exists on the SD (via the np2 dosio layer, same paths as mounts).
static bool sd_file_exists(const char *path) {
    FILEH h = file_open_rb((const OEMCHAR *)path);
    if (h == FILEH_INVALID) return false;
    file_close(h);
    return true;
}

static void emu_task(void *arg);

// Emulator task stack lives in internal .bss (placed at link time), not the
// heap: moving the hot CPU data (szpflag_w etc.) into internal DRAM fragmented
// the heap so a 64KB contiguous heap block was no longer available. A static
// stack sidesteps heap fragmentation entirely.
#define EMU_STACK_WORDS (56 * 1024 / sizeof(StackType_t))
static StackType_t  emu_stack[EMU_STACK_WORDS];
static StaticTask_t emu_tcb;

extern "C" void app_main(void) {
    // Silence the power-on/reboot speaker noise: the I2S pins float from reset
    // until audio_init() runs (seconds later). Drive BCK/WS/DOUT low at once.
    gpio_config_t io = {};
    io.pin_bit_mask = (1ULL << 38) | (1ULL << 39) | (1ULL << 40);
    io.mode = GPIO_MODE_OUTPUT;
    gpio_config(&io);
    gpio_set_level((gpio_num_t)38, 0);
    gpio_set_level((gpio_num_t)39, 0);
    gpio_set_level((gpio_num_t)40, 0);
    initArduino();
    vTaskDelay(pdMS_TO_TICKS(1500));
    // Run the emulator on a dedicated task (internal static stack, core 1).
    xTaskCreateStaticPinnedToCore(emu_task, "emu", EMU_STACK_WORDS, nullptr, 5,
                                  emu_stack, &emu_tcb, 1);
    vTaskDelete(nullptr);
}

static void emu_task(void *arg) {
    (void)arg;
    banner("np2 espresso (np2kai i286c)");
    printf("PSRAM: total=%u free=%u\n", (unsigned)ESP.getPsramSize(), (unsigned)ESP.getFreePsram());
    printf("internal heap free=%u\n", (unsigned)ESP.getFreeHeap());

    // --- SD ---
    sdspi.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
    if (!SD.begin(SD_CS, sdspi, 20000000)) {
        printf("SD init FAILED\n"); vTaskDelete(nullptr); return;
    }
    printf("SD init OK\n");

    // --- LCD (ST7789 on HSPI) ---
    printf("LCD init: %s\n", lcd_init() ? "OK" : "FAIL");

    // --- USB HID keyboard (USB-OTG host) ---
    usb_kbd_init();

    // --- np2 config ---
    pccore_setdefault();
    milstr_ncpy(np2cfg.model, "VX", sizeof(np2cfg.model));   // PC-9801VX (80286: XMS/HIMEM capable)
    // Emulated V30 @ ~4.92MHz = base 2.4576MHz x2. x4 (authentic VM21 9.83MHz)
    // left the i286c core at ~17ms/frame with ZERO graphics — borderline for
    // real time, so games dipped below it and FM audio (22050 samples/s tied
    // to emulated time) crackled from chronic supply deficit. x3 (~12ms) was
    // enough at rest but marginal under load; x2 (~8ms) gives a wide margin.
    np2cfg.multiple = 3;
    np2cfg.EXTMEM = 1;                                       // 1MB extended (XMS) — _MALLOC'd from PSRAM at init
#if ENABLE_FM_SOUND
    // PC-9801-26K (YM2203: 3 FM + SSG) — the classic VM21-era FM board. sound_init()
    // (inside pccore_init) reads these; g_sound_enable makes soundmng_create return
    // a real buffer so sound_create() succeeds.
    g_sound_enable   = 1;
    np2cfg.SOUND_SW  = SOUNDID_PC_9801_26K;
    // fmgen is compiled out entirely (SUPPORT_FMGEN removed): np2's native
    // opngen always does the synthesis — lighter and plenty for a YM2203.
    np2cfg.samplingrate = FM_RATE;
    np2cfg.delayms   = FM_DELAY_MS;
    // Headroom: 3 FM + 3 SSG channels summed at full volume can exceed int16 and
    // clip (buzz on loud/bass passages). 70% master leaves room for the mix.
    np2cfg.vol_master = 70;
    np2cfg.vol_fm    = 100;
    np2cfg.vol_ssg   = 100;
    g_fm_frames      = (FM_RATE * FM_DELAY_MS) / 1000;
#else
    np2cfg.SOUND_SW = SOUNDID_NONE;                          // FM off (see ENABLE_FM_SOUND)
#endif
    milstr_ncpy(np2cfg.biospath, "/", sizeof(np2cfg.biospath));
    file_setcd(np2cfg.biospath);
    milstr_ncpy(np2cfg.fontfile, "/FONT.ROM", sizeof(np2cfg.fontfile));  // real EPSON T98-Next font on SD
    // np2cfg.usebios (real /bios.rom vs np2's built-in emulated BIOS) is decided
    // below, once we know whether any disk is mounted — see the have_media block.

    // Equip 2 floppy drives (VM21 has two). fdc.equip is loaded from this in the
    // FDC reset (io/fdc.c), and diskdrv_readyfddex() refuses a drive whose equip
    // bit is clear — so drive 1 (FDD2) needs bit 1 set here.
    np2cfg.fddequip |= 0x03;

    // --- HDD (SASI/IDE fixed drive 0). hddbind() inside pccore_reset() opens
    //     sasihdd[0], so the image must be set BEFORE then. The path is chosen
    //     from NVS below (after dosio_init, so file existence can be checked);
    //     empty here means "no HDD". sxsihdd auto-detects NHD/HDI/THD/VHD. ---
#if defined(SUPPORT_IDEIO)
    np2cfg.idetype[0] = SXSIDEV_HDD;
#endif
    np2cfg.sasihdd[0][0] = '\0';        // no HDD unless NVS names one that exists

    // --- persisted disk state (NVS "pc98"): CPU multiple, LCD scaler, and the
    //     FDD1/FDD2/HDD image paths (written by the disk menu on ESC/RESET). Read
    //     the paths into buffers here; the actual mounts happen after dosio_init
    //     (so file existence can be checked). If NO disk keys are stored,
    //     have_nvs_disks stays false and nothing is mounted -> N88-BASIC. ---
    char saved_hdd [sizeof(np2cfg.sasihdd[0])] = {0};
    char saved_fdd0[sizeof(np2cfg.fddfile[0])] = {0};
    char saved_fdd1[sizeof(np2cfg.fddfile[1])] = {0};
    bool have_nvs_disks = false;
    {
        esp_err_t nerr = nvs_flash_init();
        if (nerr == ESP_ERR_NVS_NO_FREE_PAGES || nerr == ESP_ERR_NVS_NEW_VERSION_FOUND) {
            nvs_flash_erase();
            nerr = nvs_flash_init();
        }
        nvs_handle_t nh;
        if (nerr == ESP_OK && nvs_open("pc98", NVS_READONLY, &nh) == ESP_OK) {
            uint8_t v;
            if (nvs_get_u8(nh, "multiple", &v) == ESP_OK && v >= 1 && v <= 5)
                np2cfg.multiple = v;
            if (nvs_get_u8(nh, "scaler", &v) == ESP_OK && v < lcd_scale_mode_count())
                lcd_set_scale_mode(v);
            size_t len;
            len = sizeof(saved_hdd);
            if (nvs_get_str(nh, "hdd",  saved_hdd,  &len) == ESP_OK) have_nvs_disks = true;
            len = sizeof(saved_fdd0);
            if (nvs_get_str(nh, "fdd0", saved_fdd0, &len) == ESP_OK) have_nvs_disks = true;
            len = sizeof(saved_fdd1);
            if (nvs_get_str(nh, "fdd1", saved_fdd1, &len) == ESP_OK) have_nvs_disks = true;
            nvs_close(nh);
        }
    }

    dosio_init();
    keystat_initialize();

    // HDD mount decision (before pccore_reset()'s hddbind opens sasihdd[0]):
    // mount only when NVS names an HDD image that actually exists on the SD.
    if (have_nvs_disks && saved_hdd[0] && sd_file_exists(saved_hdd))
        file_cpyname(np2cfg.sasihdd[0], (const OEMCHAR *)saved_hdd,
                     NELEMENTS(np2cfg.sasihdd[0]));
    // Which BIOS to run. With a disk mounted we want the real /bios.rom, since
    // that is what boots real media. With nothing mounted the real ROM falls
    // through to its N88-BASIC banner screen, so we deliberately keep np2's
    // built-in BIOS there instead: it stops at its own "insert the system disk"
    // prompt, which is the correct thing to show and carries no NEC copyright
    // notice. FDD mounts happen after pccore_reset(), so test the saved paths
    // rather than np2cfg.fddfile — but this must be settled before pccore_init(),
    // which is where bios_initialize() reads the flag.
    const bool have_media = np2cfg.sasihdd[0][0] ||
                            (have_nvs_disks && saved_fdd0[0] && sd_file_exists(saved_fdd0)) ||
                            (have_nvs_disks && saved_fdd1[0] && sd_file_exists(saved_fdd1));
    np2cfg.usebios = have_media ? 1 : 0;
    printf("BIOS: media=%d usebios=%d  /bios.rom on SD=%d\n",
           (int)have_media, (int)np2cfg.usebios, (int)sd_file_exists("/bios.rom"));
#if defined(CPUCORE_IA32)
    SetCpuTypeIndex(0);            // i386c only: pick the IA-32 CPU model
#endif
    printf("framebuffer alloc: %s\n", pc98_scrnmng_init() ? "OK (640x480 PSRAM)" : "FAIL");
    banner("pccore_init");
    printf("free before pccore_init: internal=%u psram=%u\n",
           (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getFreePsram());
    pccore_init();
    pccore_reset();
    printf("BIOS ROM: %s\n", (pccore.rom & PCROM_BIOS) ? "real /bios.rom loaded" : "emulated (dummy BASIC)");

    // --- mount floppies (after pccore_reset). Load NOW (setup context) via
    //     readyfddex instead of the delayed setfdd: the delayed load runs inside
    //     pccore_exec where fopen()/SD crash. Opening here keeps the handle open so
    //     the FDC streams sectors via read()/lseek during emulation.
    //     Mount each saved floppy whose file exists; with no NVS disk data both
    //     drives stay empty and the BIOS drops to N88-BASIC. ---
    if (have_nvs_disks) {
        if (saved_fdd0[0] && sd_file_exists(saved_fdd0))
            diskdrv_readyfddex(0, (const OEMCHAR *)saved_fdd0, FTYPE_NONE, 0);
        if (saved_fdd1[0] && sd_file_exists(saved_fdd1))
            diskdrv_readyfddex(1, (const OEMCHAR *)saved_fdd1, FTYPE_NONE, 0);
    }
    pccore_cfgupdate();
    printf("disks: nvs=%d fdd1=[%s] fdd2=[%s] hdd=[%s]\n", (int)have_nvs_disks,
           np2cfg.fddfile[0], np2cfg.fddfile[1], np2cfg.sasihdd[0]);
    printf("fddequip=%02X  fdd_diskready(0)=%d  fdd_diskready(1)=%d\n",
           np2cfg.fddequip, (int)fdd_diskready(0), (int)fdd_diskready(1));

    banner("running");
    printf("free after reset: internal=%u psram=%u largest_psram=%u\n",
           (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getFreePsram(),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
    // HDD open status (hddbind ran inside pccore_reset): flag has FILEOPENED|READY
    // when the HDD image opened OK; also report the detected geometry. An empty
    // sasihdd[0] means "no HDD" was requested (BASIC / floppy boot), not an error.
    { SXSIDEV hd = sxsi_getptr(0x00);
      if (hd && (hd->flag & SXSIFLAG_FILEOPENED)) {
        printf("HDD: open OK [%s] flag=%02X  C/H/S=%u/%u/%u  totals=%u\n",
               np2cfg.sasihdd[0], hd->flag, hd->cylinders, hd->surfaces, hd->sectors,
               (unsigned)hd->totals);
      } else if (np2cfg.sasihdd[0][0]) {
        printf("HDD: [%s] NOT open (flag=%02X) — check the image on SD\n",
               np2cfg.sasihdd[0], hd ? hd->flag : 0xff);
      } else {
        printf("HDD: none mounted (boot to N88-BASIC or floppy)\n");
      }
    }

#if ENABLE_FM_SOUND
    printf("FM: SOUND_SW=%02X rate=%d frames/blk=%d  audio_init=%s\n",
           np2cfg.SOUND_SW, FM_RATE, g_fm_frames,
           audio_init(FM_RATE, g_fm_frames) ? "OK" : "FAIL");
#endif

    // --- run the machine continuously; render + push to the LCD once booted ---
    uint8_t *fb = pc98_framebuffer();
    int64_t t0 = esp_timer_get_time();
    int total = 0;
    // Rendering (drawscreen -> maketext/composite) is the biggest per-frame cost
    // after the PSRAM-bound CPU, and the LCD only changes when we blit. So run
    // most frames with draw=FALSE (pccore_exec skips drawscreen) and only render
    // + blit 1 in RENDER_EVERY frames. Big speedup with a still-usable ~emu/N LCD.
    // drawscreen() self-skips when nothing changed (np2 dirty tracking: only ~13
    // composites per 300 idle frames), so we let it run every frame — it's ~free
    // when static — and only pay the 25ms LCD blit on frames that actually drew.
    const int RENDER_EVERY = 2;   // drawscreen every 2nd frame: frees core 1 so emu hits real time (audio needs 22050 samples/s)
    // perf breakdown accumulators (us) — answer "is the blit/downscale the cost?"
    int64_t acc_exec = 0, acc_draw = 0, acc_blit = 0, acc_snd = 0;
    int n_exec = 0, n_draw = 0;
    UINT dc_prev = drawcount;      // DIAG: how many frames actually composite (scrndraw_draw)
    // Emulated-time bookkeeping, shared by the FM drain and the real-time throttle:
    // dcyc = emulated CPU cycles executed per frame; both audio sample count and the
    // wall-clock target derive from it (via pccore.realclock).
    uint32_t rt_hz = pccore.realclock ? pccore.realclock : 9984000; // re-read each frame (F11 changes it)
    uint32_t rt_prevcyc = (uint32_t)(CPU_CLOCK + CPU_BASECLOCK - CPU_REMCLOCK);
    int64_t  rt_t0 = esp_timer_get_time();     // wall baseline
    int64_t  rt_emu_us = 0;                     // accumulated emulated time (us)
    int64_t  acc_wait = 0;                      // perf: time spent throttled
    int64_t  snd_owed = 0;                      // FM: audio samples owed (rate-matched drain)
    printf("realtime pacing ON: realclock=%u Hz\n", (unsigned)rt_hz);
    for (;;) {
        // Disk swap menu (Pause/Break): modal — pccore_exec pauses inside.
        if (g_menu_req) {
            g_menu_req = 0;
            while (lcd_blit_busy()) vTaskDelay(1);  // in-flight blit must finish before the menu draws
            menu_disk_run();
            if (fb) lcd_blit(fb);   // menu drew straight to the LCD: restore the emulated screen
            // Time stopped while the menu was open: rebase the pacing clocks so
            // the throttle/audio drain don't try to "catch up" the menu time.
            rt_t0 = esp_timer_get_time();
            rt_emu_us = 0;
            rt_prevcyc = (uint32_t)(CPU_CLOCK + CPU_BASECLOCK - CPU_REMCLOCK);
        }

        // Disk-menu CPU row: change the emulated clock (x1..x5) at runtime.
        // Replicates np2's own asynccpu clock-change path so every clocked
        // subsystem (sound/beep/kbd/mouse/GDC/nevent) follows.
        if (g_speed_req) {
            UINT oldm = pccore.multiple;
            UINT newm = (UINT)g_speed_req;
            g_speed_req = 0;
            if (newm < 1) newm = 1;
            if (newm > 5) newm = 5;
            pccore.multiple = pccore.maxmultiple = np2cfg.multiple = newm;
            pccore.realclock = pccore.baseclock * newm;
            pcm86_changeclock(oldm);
            nevent_changeclock(oldm, newm);
            sound_changeclock();
            beep_changeclock();
            mpu98ii_changeclock();
#if defined(SUPPORT_SMPU98)
            smpu98_changeclock();
#endif
            keyboard_changeclock();
            mouseif_changeclock();
            gdc_updateclock();
            rt_emu_us = esp_timer_get_time() - rt_t0;   // rate changed: don't bank false debt
            printf("cpu multiple -> %u (%u Hz)\n", (unsigned)newm, (unsigned)pccore.realclock);
        }
        rt_hz = pccore.realclock ? pccore.realclock : 9984000;

        // Feed queued USB-keyboard events into the emulated keyboard.
        uint8_t nk, dn;
        while (usb_kbd_pop(&nk, &dn)) {
            if (dn) keystat_keydown(nk); else keystat_keyup(nk);
        }

        // Regular-cadence render scheduling. Bursty catch-up renders look like
        // stutter even at the same average fps, so the LCD cadence itself is
        // load-adaptive: light load -> every 2nd frame (~28fps), heavy load ->
        // every 4th frame (~14fps, but a STEADY 14). Rendering is only skipped
        // outright when emulated time is >100ms behind wall clock — audio is
        // tied to emulated time, and that guard keeps the FM stream fed.
        int64_t debt_us = rt_emu_us - (esp_timer_get_time() - rt_t0);   // <0 = behind
        int every = (debt_us > -30000) ? RENDER_EVERY : 4;
        int render = g_pc98_draw_enabled && ((total % every) == 0) && (debt_us > -100000);
        // Tear-free: don't let this frame's drawscreen rewrite fb while the
        // core-0 blit is still reading it. A full non-render frame has passed
        // since the enqueue, so the blit is normally long done (zero wait).
        if (render) while (lcd_blit_busy()) vTaskDelay(1);
        UINT dc0 = drawcount;
        int64_t a = esp_timer_get_time();
        pccore_exec(render ? TRUE : FALSE);
        int64_t b = esp_timer_get_time();
        // Only push to the LCD when drawscreen actually re-composited the frame
        // (drawcount advanced) — skips the 25ms blit while the screen is static.
        if (render && fb) {
            if (drawcount != dc0) {
                lcd_blit(fb);
            } else if (drawcount == 0 && (total % 8) == 0) {
                render_boot_text(fb);   // boot phase only: GDC not programmed yet
                lcd_blit(fb);
            }
        }
        int64_t c = esp_timer_get_time();
        if (render) { acc_draw += (b - a); acc_blit += (c - b); n_draw++; }
        else        { acc_exec += (b - a); n_exec++; }

        total++;

        // Emulated CPU cycles executed this frame (unsigned delta is wrap-safe;
        // clamp resets/garbage to ~one frame). Shared by FM drain + throttle.
        uint32_t cyc  = (uint32_t)(CPU_CLOCK + CPU_BASECLOCK - CPU_REMCLOCK);
        uint32_t dcyc = cyc - rt_prevcyc;
        rt_prevcyc = cyc;
        if (dcyc == 0 || dcyc > rt_hz / 20) dcyc = rt_hz / 60;  // sane: <1/20s

#if ENABLE_FM_SOUND
        // Rate-matched drain: emit audio in proportion to emulated time (dcyc), so
        // tempo is correct at any frame rate. (A fixed block/frame ran ~12% fast.)
        {
            int64_t s0 = esp_timer_get_time();
            snd_owed += (int64_t)dcyc * FM_RATE / rt_hz;
            for (int guard = 0; snd_owed >= g_fm_frames && guard < 8; guard++) {
                const SINT32 *pcm = sound_pcmlock();
                if (!pcm) break;
                audio_write_s32((const int32_t *)pcm, g_fm_frames);
                sound_pcmunlock(pcm);
                snd_owed -= g_fm_frames;
            }
            acc_snd += esp_timer_get_time() - s0;
        }
#endif

#if ENABLE_REALTIME
        rt_emu_us += (int64_t)dcyc * 1000000 / rt_hz;
        {
            int64_t now   = esp_timer_get_time() - rt_t0;
            int64_t ahead = rt_emu_us - now;
            if (ahead > 1000) {                 // ahead of real time -> wait it out
                acc_wait += ahead;
                vTaskDelay(pdMS_TO_TICKS(ahead / 1000));
            } else {
                if (ahead < -200000) rt_emu_us = now;   // >200ms behind -> forgive debt
                if ((total & 31) == 0) vTaskDelay(1);   // yield so IDLE/WDT runs when not sleeping
            }
        }
#else
        if ((total & 7) == 0) vTaskDelay(1);   // feed watchdog / let IDLE run
#endif

#if DEBUG_PERF_LOG
        if ((total % 300) == 0) {
            int ms = (int)((esp_timer_get_time() - t0) / 1000);
            int fps10 = ms ? (int)((long long)total * 10000 / ms) : 0;
            printf("[%d frames] %d.%d fps in %dms\n", total, fps10 / 10, fps10 % 10, ms);
            // per-frame averages: exec=CPU-only(no draw), draw=CPU+drawscreen, blit=downscale+SPI
            int nall = n_exec + n_draw;
            printf("  perf/frame: exec=%dus (n=%d)  draw=%dus  blit=%dus (n=%d)  snd=%dus  composites=%u\n",
                   n_exec ? (int)(acc_exec / n_exec) : 0, n_exec,
                   n_draw ? (int)(acc_draw / n_draw) : 0,
                   n_draw ? (int)(acc_blit / n_draw) : 0, n_draw,
                   nall ? (int)(acc_snd / nall) : 0,
                   (unsigned)(drawcount - dc_prev));
            dc_prev = drawcount;
            acc_exec = acc_draw = acc_blit = acc_snd = 0; n_exec = n_draw = 0;
#if ENABLE_REALTIME
            // wait/frame high => plenty of real-time headroom; ~0 => can't keep up
            printf("  realtime: wait=%dus/frame (headroom)\n", (int)(acc_wait / 300));
            acc_wait = 0;
#endif
#if ENABLE_FM_SOUND
            // audio peak: >32767 => the mix clips (buzz). Reset each window.
            printf("  audio: peak=%d drops=%d under=%d | gen=%d skip=%d locks=%d\n",
                   g_audio_peak, g_audio_drops, g_audio_under,
                   g_np2_snd_gen, g_np2_snd_skip, g_np2_snd_locks);
            g_audio_peak = 0; g_audio_drops = 0; g_audio_under = 0;
            g_np2_snd_gen = 0; g_np2_snd_skip = 0; g_np2_snd_locks = 0;
            int nfr = 300;
            printf("  phases/frame: loop=%dus cb=%dus sndsync=%dus draw=%dus\n",
                   (int)(g_prof_loop / nfr), (int)(g_prof_cb / nfr),
                   (int)(g_prof_snd / nfr), (int)(g_prof_draw / nfr));
            g_prof_loop = 0; g_prof_cb = 0; g_prof_snd = 0; g_prof_draw = 0;
#endif
            if (total <= 900) dump_text_vram();
        }
#endif
    }
}
