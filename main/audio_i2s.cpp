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
#include "driver/i2s.h"                 // legacy driver (matches Anemoia)
#include "freertos/stream_buffer.h"

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
    s_buf = (int16_t *)heap_caps_malloc((size_t)maxframes * 2 * sizeof(int16_t),
                                        MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    size_t ringbytes = (size_t)maxframes * 2 * sizeof(int16_t) * 6;   // ~120ms: absorb load jitter
    s_ring = xStreamBufferCreate(ringbytes, 1);
    if (!s_buf || !s_ring) { ets_printf("audio: alloc FAIL\n"); return false; }
    s_ok = true;
    xTaskCreatePinnedToCore(audio_task, "audio", 3072, nullptr, 6, nullptr, 0);
    ets_printf("audio: I2S(legacy) ready rate=%d pins bclk=%d ws=%d dout=%d\n",
               rate, AUD_BCLK, AUD_WS, AUD_DOUT);
    return true;
}

static inline int16_t clip16(int32_t v) {
    if (v >  32767) return  32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}

// Convert `frames` interleaved stereo SINT32 -> int16 and enqueue for core 0.
extern "C" void audio_write_s32(const int32_t *pcm, int frames) {
    if (!s_ok || !s_ring || !s_buf || !pcm || frames <= 0) return;
    if (frames > s_bufframes) frames = s_bufframes;
    int peak = g_audio_peak;
    for (int i = 0; i < frames * 2; i++) {
        int32_t v = pcm[i];
        int32_t av = v < 0 ? -v : v;
        if (av > peak) peak = av;
        s_buf[i] = clip16(v);
    }
    g_audio_peak = peak;
    size_t bytes = (size_t)frames * 2 * sizeof(int16_t);
    size_t sent  = xStreamBufferSend(s_ring, s_buf, bytes, 0);
    if (sent < bytes) g_audio_drops += (int)((bytes - sent) / (2 * sizeof(int16_t)));
}
