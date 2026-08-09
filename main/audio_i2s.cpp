// I2S audio output for the PC-98 emulator's FM sound (milestone ④), dual-core.
//
// The new i2s_std driver produced a constant "gritty" noise on this exact board
// (MAX98357A + 8ohm), while the Anemoia-ESP32 reference is clean on the SAME
// hardware. Anemoia uses the LEGACY i2s driver with a specific config, so we now
// replicate that config verbatim (mode/format/dma/use_apll/auto_clear) to match
// the known-good setup. Pins BCLK=38, WS=39, DOUT=40.
//
// Threading: np2 (core 1) produces PCM into a lock-free stream buffer; a core-0
// task drains it into I2S with a BLOCKING i2s_write, so core 1 never stalls and
// the DAC is fed continuously.

#include <Arduino.h>
#include <math.h>
#include <string.h>
#include "driver/i2s.h"                 // legacy driver (matches Anemoia)
#include "freertos/stream_buffer.h"
#include "pie_simd.h"                   // ESP32-S3 PIE SIMD helpers (np2kai shim)

extern "C" int ets_printf(const char *fmt, ...);

// DIAG: 1 => ignore FM, output a pure test sine straight to the DAC (path check).
#define AUDIO_TESTTONE 0

#define AUD_BCLK  38
#define AUD_WS    39
#define AUD_DOUT  40

static bool                s_ok   = false;
static int16_t            *s_buf  = nullptr;   // conversion scratch (stereo int16)
static int                 s_bufframes = 0;
static StreamBufferHandle_t s_ring = nullptr;  // core1 -> core0 PCM stream (bytes)
static int                 s_rate = 22050;
#if NP2_SIMD_SELFCHECK
static int16_t            *s_ref  = nullptr;   // C-reference scratch for the PIE dual-run check
static int                 s_checks = 0, s_fails = 0, s_pie_calls = 0;
#endif

volatile int g_audio_peak  = 0;   // max |sample| pre-clip (>32767 = clipping)
volatile int g_audio_drops = 0;   // stereo frames the ring couldn't accept (overflow)
volatile int g_audio_under = 0;   // ring-empty events (starvation: DAC underruns = crackle)

// ---- core 0 task: drain the ring into the DAC, blocking so nothing is dropped ----
static void audio_task(void *arg) {
    (void)arg;
    static int16_t rx[512 * 2];
#if AUDIO_TESTTONE
    double ph = 0; const double k = 2*M_PI*440/s_rate;
    for (;;) {
        for (int i = 0; i < 512; i++) { int v=(int)(8000*sin(ph)); ph+=k; rx[i*2]=rx[i*2+1]=(int16_t)v; }
        size_t wr = 0; i2s_write(I2S_NUM_0, rx, sizeof(rx), &wr, portMAX_DELAY);
    }
#else
    for (;;) {
        // 100ms timeout: expiring means the producer (core 1, slower than real time
        // under load) starved the DAC -> the DMA auto-clear outputs zeros = crackle.
        size_t n = xStreamBufferReceive(s_ring, rx, sizeof(rx), pdMS_TO_TICKS(100));
        if (n) { size_t wr = 0; i2s_write(I2S_NUM_0, rx, n, &wr, portMAX_DELAY); }
        else g_audio_under++;
    }
#endif
}

// Legacy driver config copied from Anemoia-ESP32 (ESP32-S3 branch), which is clean
// on this hardware. rate = sample rate, maxframes = biggest block audio_write gets.
extern "C" bool audio_init(int rate, int maxframes) {
    s_rate = rate;
    i2s_config_t cfg = {};
    cfg.mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
    cfg.sample_rate          = rate;
    cfg.bits_per_sample      = I2S_BITS_PER_SAMPLE_16BIT;
    cfg.channel_format       = I2S_CHANNEL_FMT_RIGHT_LEFT;   // stereo
    cfg.communication_format = I2S_COMM_FORMAT_I2S_MSB;      // == Anemoia
    cfg.intr_alloc_flags     = 0;
    cfg.dma_buf_count        = 8;
    cfg.dma_buf_len          = 256;
    cfg.use_apll             = false;
    cfg.tx_desc_auto_clear   = true;
    if (i2s_driver_install(I2S_NUM_0, &cfg, 0, NULL) != ESP_OK) {
        ets_printf("audio: i2s_driver_install FAIL\n"); return false;
    }
    i2s_pin_config_t pins = {};
    pins.mck_io_num   = I2S_PIN_NO_CHANGE;
    pins.bck_io_num   = AUD_BCLK;
    pins.ws_io_num    = AUD_WS;
    pins.data_out_num = AUD_DOUT;
    pins.data_in_num  = I2S_PIN_NO_CHANGE;
    if (i2s_set_pin(I2S_NUM_0, &pins) != ESP_OK) {
        ets_printf("audio: i2s_set_pin FAIL\n"); return false;
    }
    s_bufframes = maxframes;
    // 16-byte align the scratch: the PIE fast path requires it (EE.VST.128).
    // Never freed (lives for the whole session), so the raw pointer is dropped.
    size_t scratch = (size_t)maxframes * 2 * sizeof(int16_t) + 16;
    void *raw_buf = heap_caps_malloc(scratch, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    if (!raw_buf) { ets_printf("audio: alloc FAIL\n"); return false; }
    s_buf = (int16_t *)(((uintptr_t)raw_buf + 15) & ~(uintptr_t)15);
#if NP2_SIMD_SELFCHECK
    void *raw_ref = heap_caps_malloc(scratch, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    if (!raw_ref) { ets_printf("audio: alloc FAIL\n"); return false; }
    s_ref = (int16_t *)(((uintptr_t)raw_ref + 15) & ~(uintptr_t)15);
#endif
    size_t ringbytes = (size_t)maxframes * 2 * sizeof(int16_t) * 6;   // ~120ms: absorb load jitter
    s_ring = xStreamBufferCreate(ringbytes, 1);
#if NP2_SIMD_SELFCHECK
    if (!s_buf || !s_ring || !s_ref) { ets_printf("audio: alloc FAIL\n"); return false; }
#else
    if (!s_buf || !s_ring) { ets_printf("audio: alloc FAIL\n"); return false; }
#endif
    s_ok = true;
    xTaskCreatePinnedToCore(audio_task, "audio", 3072, nullptr, 6, nullptr, 0);
    ets_printf("audio: I2S(legacy) ready rate=%d pins bclk=%d ws=%d dout=%d\n",
               rate, AUD_BCLK, AUD_WS, AUD_DOUT);
    return true;
}

// Convert `frames` interleaved stereo SINT32 -> int16 and enqueue for core 0.
extern "C" void audio_write_s32(const int32_t *pcm, int frames) {
    if (!s_ok || !s_ring || !s_buf || !pcm || frames <= 0) return;
    if (frames > s_bufframes) frames = s_bufframes;
    const int n = frames * 2;
    size_t bytes = (size_t)n * sizeof(int16_t);
#if NP2_SIMD_SELFCHECK
    // Dual-run: C reference vs PIE dispatcher on the same input, byte-compare.
    // Also reports alignment, since the PIE path only engages on 16B-aligned
    // src/dst (s_buf is aligned; sndstream.buffer comes from the core's _MALLOC).
    uint32_t t0 = esp_cpu_get_cycle_count();
    int peak_ref = np2simd_s32_to_s16_c(pcm, s_ref, n, g_audio_peak);
    uint32_t t1 = esp_cpu_get_cycle_count();
    int peak_pie = np2simd_s32_to_s16(pcm, s_buf, n, g_audio_peak);
    uint32_t t2 = esp_cpu_get_cycle_count();
    const bool pie_used = (n >= 8 && np2simd_aligned16(pcm) && np2simd_aligned16(s_buf));
    s_checks++; s_pie_calls += pie_used ? 1 : 0;
    bool fail = (peak_ref != peak_pie) || memcmp(s_ref, s_buf, bytes) != 0;
    if (fail) {
        s_fails++;
        ets_printf("SELFCHECK audio FAIL #%d peak ref=%d pie=%d n=%d\n",
                   s_fails, peak_ref, peak_pie, n);
        for (int i = 0; i < n; i++)
            if (s_ref[i] != s_buf[i])
                ets_printf("  first diff i=%d ref=%d pie=%d\n", i, s_ref[i], s_buf[i]), i = n;
    }
    if (fail || (s_checks & 0x3F) == 1)
        ets_printf("SELFCHECK audio: %s C=%lu PIE=%lu cyc fails=%d/%d pie_calls=%d src_al=%d dst_al=%d\n",
                   fail ? "FAIL" : "PASS", (unsigned long)(t1 - t0), (unsigned long)(t2 - t1),
                   s_fails, s_checks, s_pie_calls, (int)(((uintptr_t)pcm) & 15), (int)(((uintptr_t)s_buf) & 15));
    g_audio_peak = peak_pie;
#else
    g_audio_peak = np2simd_s32_to_s16(pcm, s_buf, n, g_audio_peak);
#endif
    size_t sent  = xStreamBufferSend(s_ring, s_buf, bytes, 0);
    if (sent < bytes) g_audio_drops += (int)((bytes - sent) / (2 * sizeof(int16_t)));
}
