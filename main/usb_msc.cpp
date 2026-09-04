// USB Mode — expose the SD card to a PC as a USB Mass Storage device.
//
// Chosen from the disk menu ("USB Mode after reboot"), which sets an NVS flag
// and reboots. app_main() reads that flag FIRST — before Bluetooth, before
// arduino, before the emulator — and if it is set, comes here and never
// returns. The flag is cleared the moment it is read, so anything that reboots
// the board (replugging USB, RESET, a crash) comes back up as the emulator.
// One-shot by construction: there is no way to get stuck in USB Mode.
//
// Why the whole emulator is skipped rather than "paused":
//   - The PC writes RAW SECTORS. If np2kai still held the FAT mounted, its
//     FATFS cache would go stale behind the host's back and the card would be
//     corrupted. Here the card is only probed for its geometry; no filesystem
//     is ever mounted on this side.
//   - The S3 has ONE USB PHY, and normal operation gives it to the USB HID
//     host (usb_kbd.cpp) — which is why this board does not enumerate at all
//     while the emulator runs. USB Mode hands the same PHY to the device stack
//     instead, so the keyboard is gone for the duration but a CDC console
//     appears, which normal mode does not have.
//   - The BLE controller's 44KB of internal DMA RAM is not spent, which leaves
//     the USB stack plenty of room.
//
// Speed: reads and writes both land at ~0.7 MB/s, measured against a 16MB
// file with a SHA256 check. That is the Full Speed USB ceiling (the S3 OTG
// core has no High Speed), not the card — the SD side still has headroom.
// Getting there needed two things, both kept below:
//   - multi-sector transfers. One sector per call (what SD.readRAW() and
//     SD_MMC.writeRAW() both do) costs a command plus the card's internal
//     program time per 512 bytes and measured 0.09 MB/s.
//   - a write-back cache handed to a separate task, so the card write overlaps
//     the next USB transfer instead of stalling it.

#include <string.h>
#include <stdio.h>
#include <stdarg.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include "nvs.h"
#include "nvs_flash.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_memory_utils.h"
#include "esp_private/usb_phy.h"
#include "esp_system.h"
#include "esp_log.h"
#include "tusb.h"

// The card is driven through arduino-esp32's SD back end, not the IDF sdspi
// driver, and that is not laziness — the IDF driver cannot talk to the card in
// this board at all. Its init sequence ends with CMD59 (CRC_ON_OFF) and treats
// a refusal as fatal:
//     sdspi_transaction: cmd=59, R1 response: command not supported
//     sdmmc_init_spi_crc: sdmmc_send_cmd_crc_on_off returned 0x106
//     sdmmc_card_init: ESP_ERR_NOT_SUPPORTED
// The card is otherwise fine (it answers CMD8 and reports itself as SDHC);
// it simply does not implement that optional command, and IDF offers no way
// to skip the step. arduino-esp32's sd_diskio never sends CMD59, which is why
// SD.begin() in main.cpp has always worked here.
//
// sdReadSectors()/sdWriteSectors() take a sector count, so multi-sector
// transfers — the thing that makes this fast — still work. They are not in
// sd_diskio.h, but they are not static either; declare them here.
#include <SPI.h>
#include "sd_diskio.h"
bool sdReadSectors(uint8_t pdrv, char *buffer, unsigned long long sector, int count);
bool sdWriteSectors(uint8_t pdrv, const char *buffer, unsigned long long sector, int count);
// Returns 0 on success (a FATFS DSTATUS, i.e. an unsigned char).
unsigned char ff_sd_initialize(unsigned char pdrv);

extern "C" {
#include <compiler.h>  // np2kai types
#include <i286c/cpumem.h>
#include <font/font.h>      // fontrom (mem + FONT_ADRS)
#include <font/fontdata.h>  // fontdata_8: the 8x8 ANK face built into np2kai
extern UINT8 mem[];         // np2kai main memory (cpumem.c) — a static array,
                            // so fontrom is usable without any emulator init
}

// SD wiring, mirroring main.cpp: SPI (FSPI = SPI2_HOST) at 20MHz. The panel is
// on SPI3, so the two buses are independent and the init order does not matter.
static const int SD_CS = 4, SD_SCK = 5, SD_MISO = 6, SD_MOSI = 7;
static const int SD_FREQ_HZ = 20000000;

extern "C" int ets_printf(const char *fmt, ...);

// This board's console is UART0 (43/44), which needs an adapter nobody has
// wired up, and in USB Mode the PHY belongs to the device stack anyway. So the
// startup log is kept here and pushed out of the CDC port.
//
// Deliberately a LINEAR buffer that stops when full, not a ring: the whole
// point is the startup log, and it is REPLAYED from the beginning every time a
// host opens the port (see tud_cdc_line_state_cb). A ring that is consumed on
// the first drain loses exactly the lines worth reading — Windows probes a new
// CDC as it enumerates, so by the time a terminal attaches the interesting
// part is already gone. That cost one debugging round on this board.
//
// Sized for a startup log, not a session history: this is .bss, i.e. internal
// RAM the write-back cache would otherwise have.
//
// The buffer is malloc'd on entry to USB Mode rather than declared as .bss:
// this file is linked into the emulator too, and 2-3KB of internal RAM that
// only USB Mode ever touches is 2-3KB the FM mixer and the SD driver do not
// have. The emulator runs with about 20KB of internal DMA heap free, so a
// static buffer here is not free at all.
#define LOGBUF_SIZE 2048
static char *s_logbuf = nullptr;
static volatile uint32_t s_log_len = 0;   // bytes stored (stops at LOGBUF_SIZE)
static volatile uint32_t s_log_sent = 0;  // how much of it the host has had
static portMUX_TYPE s_log_mux = portMUX_INITIALIZER_UNLOCKED;

static void msc_logf(const char *fmt, ...) {
    char tmp[160];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n <= 0) {
        return;
    }
    if (n > (int)sizeof(tmp) - 1) {
        n = (int)sizeof(tmp) - 1;
    }
    ets_printf("%s", tmp);  // still goes to UART0 for anyone who has it wired
    if (!s_logbuf) {
        return;
    }
    taskENTER_CRITICAL(&s_log_mux);
    for (int i = 0; i < n && s_log_len < LOGBUF_SIZE; i++) {
        s_logbuf[s_log_len++] = tmp[i];
    }
    taskEXIT_CRITICAL(&s_log_mux);
}

// Pull ESP_LOGx output into the same buffer. The SD driver explains its own
// failures far better than a returned esp_err_t can ("card doesn't support
// ...", "send_cmd returned ...") and on this board there is no console to read
// them on, so route them to the CDC alongside our own lines.
static int msc_log_vprintf(const char *fmt, va_list ap) {
    char tmp[160];
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    if (n <= 0) {
        return 0;
    }
    if (n > (int)sizeof(tmp) - 1) {
        n = (int)sizeof(tmp) - 1;
    }
    ets_printf("%s", tmp);
    if (!s_logbuf) {
        return n;
    }
    taskENTER_CRITICAL(&s_log_mux);
    for (int i = 0; i < n && s_log_len < LOGBUF_SIZE; i++) {
        s_logbuf[s_log_len++] = tmp[i];
    }
    taskEXIT_CRITICAL(&s_log_mux);
    return n;
}
#define printf msc_logf

// Provided by lcd_rgb.cpp / menu_disk.cpp's renderer.
extern "C" bool lcd_init(void);
extern "C" void lcd_menu_clear(void);
extern "C" void lcd_menu_line(int row, const char *s, uint16_t fg, uint16_t bg);

#define COL_WHITE 0xFFFF
#define COL_BLACK 0x0000
#define COL_CYAN 0x07FF

// ---- boot flag -----------------------------------------------------------
// Shares the "pc98" namespace with the rest of the settings (menu_disk.cpp).
#define NVS_NS "pc98"
#define NVS_KEY_USBMSC "usbmsc"

extern "C" void usb_msc_request(void) {
    nvs_handle_t nh;
    if (nvs_open(NVS_NS, NVS_READWRITE, &nh) == ESP_OK) {
        nvs_set_u8(nh, NVS_KEY_USBMSC, 1);
        nvs_commit(nh);
        nvs_close(nh);
    }
}

// Read the flag AND clear it in the same breath. Clearing here (rather than on
// the way out of USB Mode) is what makes the mode survive nothing: a replug, a
// RESET or a panic all land back in the emulator.
extern "C" bool usb_msc_boot_flag_take(void) {
    nvs_handle_t nh;
    uint8_t want = 0;
    if (nvs_open(NVS_NS, NVS_READWRITE, &nh) != ESP_OK) {
        return false;
    }
    if (nvs_get_u8(nh, NVS_KEY_USBMSC, &want) != ESP_OK) {
        want = 0;
    }
    if (want) {
        nvs_erase_key(nh, NVS_KEY_USBMSC);
        nvs_commit(nh);
    }
    nvs_close(nh);
    return want != 0;
}

// ---- card ----------------------------------------------------------------
static const uint32_t SECTOR_SIZE = 512;
// Sectors per write-back buffer, settled at startup from whatever internal DMA
// memory the RGB panel left behind (see usb_msc_run). 64 = 32KB: the card
// writes that in ~8ms while USB (Full Speed) needs ~29ms to deliver the next
// one, so the card time hides completely behind it. Smaller still works, just
// with less margin.
static uint32_t s_cache_sectors = 0;
static const int CACHE_BUFS = 2;
static const uint32_t FLUSH_IDLE_MS = 100;

static SPIClass s_sdspi(FSPI);   // same bus main.cpp uses for SD.begin()
static uint8_t s_pdrv = 0xFF;    // arduino-esp32 SD drive number, 0xFF = no card
static uint32_t s_sectors = 0;
static uint32_t s_sector_size = 0;

static uint8_t *s_cache[CACHE_BUFS] = {nullptr, nullptr};
static int s_fill = 0;
static bool s_have_buf = false;
static uint32_t s_lba = 0;
static uint32_t s_count = 0;
static volatile uint32_t s_last_write_ms = 0;
static volatile bool s_failed = false;

typedef struct {
    int idx;
    uint32_t lba;
    uint32_t count;
} wrjob_t;

static QueueHandle_t s_wr_queue = nullptr;
static SemaphoreHandle_t s_free_bufs = nullptr;
static SemaphoreHandle_t s_mux = nullptr;

static uint8_t *s_bounce = nullptr;  // for transfers TinyUSB hands us unaligned

// sdReadSectors()/sdWriteSectors() assume the caller has already opened an SPI
// transaction at the card's clock — that is all AcquireSPI does inside
// ff_sd_read()/ff_sd_write(), and those are the only callers upstream. Called
// bare they run at whatever SPIClass::begin() left behind, i.e. 1MHz, and the
// card reader measured 0.05 MB/s instead of 0.7. Bracket every transfer.
struct SdBus {
    SdBus() {
        s_sdspi.beginTransaction(SPISettings(SD_FREQ_HZ, MSBFIRST, SPI_MODE0));
    }
    ~SdBus() {
        s_sdspi.endTransaction();
    }
};

// The IDF SD-over-SPI host, not Arduino's SD library: USB Mode must not mount a
// filesystem (the host writes raw sectors). Same bus and pins main.cpp uses for
// SD.begin(), so the card behaves identically; only the driver differs.
// Everything above card_init() is shared with the SDMMC boards unchanged;
// only the two transfer calls and this probe differ.
// This board has no console anyone can reach without wiring up UART0 (43/44),
// so every step that can fail records why, and draw_screen() puts it on the
// panel. Guessing at an SD or USB failure with no output is not debugging.
static char s_diag[64] = "";
#define DIAG(...) snprintf(s_diag, sizeof(s_diag), __VA_ARGS__)

// Two steps, and the second is easy to miss: sdcard_init() only reserves the
// drive slot and remembers the pins, while the card is actually probed —
// CMD0/CMD8/ACMD41, then the capacity read that fills in sdcard_num_sectors()
// — by ff_sd_initialize(). FATFS normally calls that at mount time, and we
// never mount (the host owns the card; a stale FATFS cache on this side would
// corrupt it), so call it here. Skipping it leaves a card of 0 sectors.
static bool card_init(void) {
    s_sdspi.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
    s_pdrv = sdcard_init(SD_CS, &s_sdspi, SD_FREQ_HZ);
    if (s_pdrv == 0xFF) {
        printf("usb_msc: sdcard_init failed\n");
        DIAG("sdcard_init failed");
        return false;
    }
    unsigned char st = ff_sd_initialize(s_pdrv);
    if (st != 0) {
        printf("usb_msc: ff_sd_initialize: 0x%02x\n", st);
        DIAG("card probe: 0x%02x", st);
        sdcard_uninit(s_pdrv);
        s_pdrv = 0xFF;
        return false;
    }
    s_sectors = sdcard_num_sectors(s_pdrv);
    s_sector_size = sdcard_sector_size(s_pdrv);
    if (!s_sectors || s_sector_size != SECTOR_SIZE) {
        printf("usb_msc: bad geometry: %u sectors of %u\n", (unsigned)s_sectors, (unsigned)s_sector_size);
        DIAG("geometry %u/%u", (unsigned)s_sectors, (unsigned)s_sector_size);
        sdcard_uninit(s_pdrv);
        s_pdrv = 0xFF;
        return false;
    }
    return true;
}

// The only place that actually writes the card. Separated from the USB
// callback so a card write and the next USB transfer run at the same time.
static void writer_task(void *arg) {
    (void)arg;
    wrjob_t job;
    for (;;) {
        xQueueReceive(s_wr_queue, &job, portMAX_DELAY);
        bool wok;
        {
            SdBus bus;
            wok = sdWriteSectors(s_pdrv, (const char *)s_cache[job.idx], job.lba, (int)job.count);
        }
        if (!wok) {
            printf("usb_msc: write %u failed\n", (unsigned)job.lba);
            s_failed = true;
        }
        xSemaphoreGive(s_free_bufs);
    }
}

// Hand the filled buffer to the writer. Caller holds s_mux.
static void submit_locked(void) {
    if (s_count == 0) {
        return;
    }
    wrjob_t job = {s_fill, s_lba, s_count};
    xQueueSend(s_wr_queue, &job, portMAX_DELAY);
    s_fill = (s_fill + 1) % CACHE_BUFS;  // FIFO queue + single writer => round robin
    s_count = 0;
    s_have_buf = false;
}

// Wait until everything submitted has reached the card. Caller holds s_mux.
static void barrier_locked(void) {
    submit_locked();
    for (int i = 0; i < CACHE_BUFS; i++) {
        xSemaphoreTake(s_free_bufs, portMAX_DELAY);
    }
    for (int i = 0; i < CACHE_BUFS; i++) {
        xSemaphoreGive(s_free_bufs);
    }
    s_have_buf = false;
}

static void flush_cache(void) {
    xSemaphoreTake(s_mux, portMAX_DELAY);
    barrier_locked();
    xSemaphoreGive(s_mux);
}

// ---- TinyUSB MSC callbacks ----------------------------------------------
extern "C" void tud_msc_inquiry_cb(uint8_t lun, uint8_t vendor_id[8], uint8_t product_id[16], uint8_t product_rev[4]) {
    (void)lun;
    memcpy(vendor_id, "PC-98   ", 8);
    memcpy(product_id, "SD Card Reader  ", 16);
    memcpy(product_rev, "1.0 ", 4);
}

extern "C" bool tud_msc_test_unit_ready_cb(uint8_t lun) {
    (void)lun;
    return s_pdrv != 0xFF && !s_failed;
}

extern "C" void tud_msc_capacity_cb(uint8_t lun, uint32_t *block_count, uint16_t *block_size) {
    (void)lun;
    *block_count = s_sectors;
    *block_size = (uint16_t)(s_sector_size ? s_sector_size : SECTOR_SIZE);
}

extern "C" bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power_condition, bool start, bool load_eject) {
    (void)lun;
    (void)power_condition;
    (void)start;
    (void)load_eject;
    // "Safely remove hardware" on the host lands here.
    flush_cache();
    return true;
}

extern "C" int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize) {
    (void)lun;
    if (s_pdrv == 0xFF || (offset % SECTOR_SIZE) != 0 || (bufsize % SECTOR_SIZE) != 0) {
        return -1;
    }
    const uint32_t start = lba + offset / SECTOR_SIZE;
    const uint32_t count = bufsize / SECTOR_SIZE;

    xSemaphoreTake(s_mux, portMAX_DELAY);
    // Settle every outstanding write first. Tracking which sectors are in
    // flight would be faster, but reads barely interleave with a bulk copy and
    // getting it wrong hands the host stale data.
    barrier_locked();

    // Read through the aligned bounce buffer: the SPI back end is happier
    // with a DMA-capable buffer than with whatever TinyUSB hands over, and a
    // 4KB memcpy is nothing next to the transfer itself.
    bool ok;
    {
        SdBus bus;
        if (esp_ptr_dma_capable(buffer) && ((uintptr_t)buffer & 63) == 0) {
            ok = sdReadSectors(s_pdrv, (char *)buffer, start, (int)count);
        } else if (bufsize <= CFG_TUD_MSC_EP_BUFSIZE) {
            ok = sdReadSectors(s_pdrv, (char *)s_bounce, start, (int)count);
            if (ok) {
                memcpy(buffer, s_bounce, bufsize);
            }
        } else {
            ok = false;
        }
    }
    xSemaphoreGive(s_mux);

    if (!ok) {
        printf("usb_msc: read %u failed\n", (unsigned)start);
        return -1;
    }
    return (int32_t)bufsize;
}

extern "C" int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t bufsize) {
    (void)lun;
    if (s_pdrv == 0xFF || s_failed || (offset % SECTOR_SIZE) != 0 || (bufsize % SECTOR_SIZE) != 0) {
        return -1;
    }
    const uint32_t start = lba + offset / SECTOR_SIZE;
    uint32_t count = bufsize / SECTOR_SIZE;
    const uint8_t *src = buffer;

    xSemaphoreTake(s_mux, portMAX_DELAY);
    if (s_count && start != s_lba + s_count) {  // no longer contiguous
        submit_locked();
    }
    if (s_count == 0) {
        s_lba = start;
    }
    while (count > 0) {
        if (!s_have_buf) {
            xSemaphoreTake(s_free_bufs, portMAX_DELAY);  // waits only if the card fell behind
            s_have_buf = true;
        }
        const uint32_t room = s_cache_sectors - s_count;
        const uint32_t n = (count < room) ? count : room;
        memcpy(s_cache[s_fill] + s_count * SECTOR_SIZE, src, n * SECTOR_SIZE);
        s_count += n;
        src += n * SECTOR_SIZE;
        count -= n;
        if (s_count == s_cache_sectors) {
            const uint32_t next = s_lba + s_count;
            submit_locked();
            s_lba = next;
        }
    }
    s_last_write_ms = (uint32_t)(esp_timer_get_time() / 1000);
    xSemaphoreGive(s_mux);
    return (int32_t)bufsize;
}

extern "C" int32_t tud_msc_scsi_cb(uint8_t lun, uint8_t const scsi_cmd[16], void *buffer, uint16_t bufsize) {
    (void)lun;
    (void)buffer;
    (void)bufsize;
    switch (scsi_cmd[0]) {
        case 0x35:  // SYNCHRONIZE CACHE (10) - no SCSI_CMD_ enum for it
            flush_cache();
            return 0;
        default:
            tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x20, 0x00);
            return -1;
    }
}

// ---- USB descriptors -----------------------------------------------------
#define USB_VID 0x303A  // Espressif
#define USB_PID 0x4003  // TinyUSB convention: bit0 = CDC, bit1 = MSC

static tusb_desc_device_t const s_desc_device = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    // Composite (CDC + MSC), so the device declares the IAD class triple and
    // each function's class lives in its interface association.
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = USB_VID,
    .idProduct = USB_PID,
    .bcdDevice = 0x0100,
    .iManufacturer = 0x01,
    .iProduct = 0x02,
    .iSerialNumber = 0x03,
    .bNumConfigurations = 0x01,
};

enum { ITF_NUM_CDC = 0, ITF_NUM_CDC_DATA, ITF_NUM_MSC, ITF_NUM_TOTAL };
#define EPNUM_CDC_NOTIF 0x81
#define EPNUM_CDC_OUT 0x02
#define EPNUM_CDC_IN 0x82
#define EPNUM_MSC_OUT 0x03
#define EPNUM_MSC_IN 0x83
#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + TUD_MSC_DESC_LEN)

static uint8_t const s_desc_config[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x00, 100),
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, 4, EPNUM_CDC_NOTIF, 8, EPNUM_CDC_OUT, EPNUM_CDC_IN, 64),
    TUD_MSC_DESCRIPTOR(ITF_NUM_MSC, 5, EPNUM_MSC_OUT, EPNUM_MSC_IN, 64),
};

static char const *s_desc_strings[] = {
    (const char[]){0x09, 0x04},  // 0: en-US
    "np2 espresso",              // 1: manufacturer
    "PC-98 SD Card Reader",      // 2: product
    "123456",                    // 3: serial
    "USB Mode console",          // 4: CDC interface
    "SD Card Reader",            // 5: MSC interface
};

// ---- CDC: console + the reset that makes reflashing possible --------------
static volatile uint32_t s_cdc_baud = 0;

extern "C" void tud_cdc_line_coding_cb(uint8_t itf, cdc_line_coding_t const *coding) {
    (void)itf;
    s_cdc_baud = coding->bit_rate;
}

// The 1200bps touch. Opening the port at 1200 baud and dropping DTR is how
// esptool (and the Arduino IDE) ask a native-USB board to reboot for flashing.
// Here it just restarts: the USB Mode flag has already been consumed, so the
// board comes back as the emulator. On this board that means back to USB host
// mode with no port at all, so flashing still needs BOOT held + RESET tapped —
// this is a clean way to LEAVE USB Mode, not a way into the bootloader.
//
// Do NOT be tempted to set RTC_CNTL_FORCE_DOWNLOAD_BOOT here to land straight
// in the ROM loader. That bit is in the RTC domain and survives a reset, so
// every subsequent boot goes to the ROM loader too and the application never
// runs again to clear it — only pulling the USB cable (a real power-on reset)
// gets the board back. Tried on hardware; it strands the board.
extern "C" void tud_cdc_line_state_cb(uint8_t itf, bool dtr, bool rts) {
    (void)itf;
    (void)rts;
    if (dtr) {
        s_log_sent = 0;  // a terminal just attached: replay the startup log
        return;
    }
    if (s_cdc_baud == 1200) {
        flush_cache();  // do not strand a half-written cache in RAM
        esp_restart();
    }
}

// Hand the log to the host, from wherever it has got to.
static void drain_log_to_cdc(void) {
    if (!tud_cdc_connected() || !s_logbuf) {
        return;
    }
    while (s_log_sent < s_log_len) {
        if (tud_cdc_write_available() == 0) {
            break;
        }
        tud_cdc_write_char(s_logbuf[s_log_sent++]);
    }
    tud_cdc_write_flush();
}

extern "C" uint8_t const *tud_descriptor_device_cb(void) {
    return (uint8_t const *)&s_desc_device;
}

extern "C" uint8_t const *tud_descriptor_configuration_cb(uint8_t index) {
    (void)index;
    return s_desc_config;
}

extern "C" uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void)langid;
    static uint16_t desc[32];
    uint8_t chr_count;

    if (index >= sizeof(s_desc_strings) / sizeof(s_desc_strings[0])) {
        return NULL;
    }
    if (index == 0) {
        memcpy(&desc[1], s_desc_strings[0], 2);
        chr_count = 1;
    } else {
        const char *str = s_desc_strings[index];
        chr_count = (uint8_t)strlen(str);
        if (chr_count > 31) {
            chr_count = 31;
        }
        for (uint8_t i = 0; i < chr_count; i++) {
            desc[1 + i] = str[i];
        }
    }
    desc[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * chr_count + 2));
    return desc;
}

// ---- screen --------------------------------------------------------------
// lcd_menu_line() draws from np2kai's CGROM at fontrom+0x80000. In USB Mode
// nothing has loaded FONT.ROM (and the card belongs to the host anyway), so
// fill that area from fontdata_8 — the 8x8 ANK face compiled into np2kai —
// doubling each row to 8x16. This is exactly what font_load() does before it
// overlays anything read from a file (font.c).
static void load_builtin_font(void) {
    const UINT8 *p = fontdata_8;
    UINT8 *q = fontrom + 0x80000;
    for (int i = 0; i < 256; i++) {
        for (int j = 0; j < 8; j++) {
            q[0] = p[0];
            q[1] = p[0];
            p += 1;
            q += 2;
        }
    }
}

// Lines are kept inside 30 characters: on this panel lcd_menu_line renders
// 8-pixel cells across a 240-wide screen, so anything longer is simply cut.
static void draw_screen(void) {
    lcd_menu_clear();
    lcd_menu_line(4, "  USB MASS STORAGE MODE", COL_CYAN, COL_BLACK);
    if (s_pdrv != 0xFF) {
        const uint64_t mb = ((uint64_t)s_sectors * s_sector_size) / (1024ULL * 1024ULL);
        char line[40];
        snprintf(line, sizeof(line), "  SD card: %u MB", (unsigned)mb);
        lcd_menu_line(6, line, COL_WHITE, COL_BLACK);
        lcd_menu_line(8, "  Now a drive on the PC.", COL_WHITE, COL_BLACK);
    } else {
        lcd_menu_line(6, "  NO SD CARD", COL_WHITE, COL_BLACK);
    }
    if (s_diag[0]) {
        char line[40];
        snprintf(line, sizeof(line), "  %.27s", s_diag);
        lcd_menu_line(9, line, COL_CYAN, COL_BLACK);
    }
    lcd_menu_line(11, "  Eject, then replug USB", COL_WHITE, COL_BLACK);
    lcd_menu_line(12, "  or RESET, to return.", COL_WHITE, COL_BLACK);
}

// draw_screen() runs before the USB stack is up, so the panel needs a second
// pass once we know whether the PHY and TinyUSB actually came up.
static void draw_usb_status(const char *s) {
    char line[40];
    snprintf(line, sizeof(line), "  %.27s", s);
    lcd_menu_line(14, line, COL_CYAN, COL_BLACK);
}

// ---- entry ---------------------------------------------------------------
static void tusb_task(void *arg) {
    (void)arg;
    for (;;) {
        tud_task();
    }
}

// Never returns.
extern "C" void usb_msc_run(void) {
    // Nothing above this point has logged anything yet, so the buffer can be
    // claimed here — out of the emulator's way for the whole time it runs.
    s_logbuf = (char *)malloc(LOGBUF_SIZE);
    printf("\n=== USB Mode (SD card reader) ===\n");

    // Panel first, so the mode is on screen before anything can fail. It is on
    // SPI3 and the card is on SPI2, so unlike the RGB boards there is no shared
    // pin forcing this order.
    printf("usb_msc: LCD init: %s\n", lcd_init() ? "OK" : "FAIL");
    load_builtin_font();

    printf("usb_msc: internal DMA heap after LCD: free=%u largest=%u\n", (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));

    s_mux = xSemaphoreCreateMutex();
    s_wr_queue = xQueueCreate(CACHE_BUFS, sizeof(wrjob_t));
    s_free_bufs = xSemaphoreCreateCounting(CACHE_BUFS, CACHE_BUFS);
    s_bounce = (uint8_t *)heap_caps_aligned_alloc(64, CFG_TUD_MSC_EP_BUFSIZE, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!s_mux || !s_wr_queue || !s_free_bufs || !s_bounce) {
        printf("usb_msc: cannot allocate the basics\n");
        lcd_menu_clear();
        lcd_menu_line(8, "        USB MODE: OUT OF MEMORY", COL_WHITE, COL_BLACK);
        for (;;) {
            vTaskDelay(portMAX_DELAY);
        }
    }

    // The write-back cache is a speed optimisation, not a requirement, so take
    // whatever internal DMA memory is going rather than refusing to run. What
    // is left after the RGB panel's bounce buffers varies with the panel
    // config, and a fixed demand turned a slower card reader into no card
    // reader at all. 64 sectors (32KB) is the point past which the card write
    // is already fully hidden behind the USB transfer; 8 (4KB, one USB
    // transfer) still pipelines, just with no margin.
    for (uint32_t want = 64; want >= 8; want /= 2) {
        for (int i = 0; i < CACHE_BUFS; i++) {
            s_cache[i] = (uint8_t *)heap_caps_aligned_alloc(64, want * SECTOR_SIZE, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        }
        if (s_cache[0] && s_cache[1]) {
            s_cache_sectors = want;
            break;
        }
        for (int i = 0; i < CACHE_BUFS; i++) {
            free(s_cache[i]);
            s_cache[i] = nullptr;
        }
    }
    if (!s_cache_sectors) {
        printf("usb_msc: not even 2x4KB of internal DMA memory left\n");
        lcd_menu_clear();
        lcd_menu_line(8, "        USB MODE: OUT OF MEMORY", COL_WHITE, COL_BLACK);
        for (;;) {
            vTaskDelay(portMAX_DELAY);
        }
    }
    printf("usb_msc: write cache %u KB x%d\n", (unsigned)(s_cache_sectors * SECTOR_SIZE / 1024), CACHE_BUFS);

    // Let the SD driver talk while it probes the card. Only its own tags are
    // turned up, so the buffer holds the card probe rather than everything
    // else's startup chatter. DEBUG lines only appear if the build was made
    // with CONFIG_LOG_MAXIMUM_LEVEL_DEBUG; ERROR and WARN come through either
    // way, and those are the ones that name the failing step.
    esp_log_set_vprintf(msc_log_vprintf);
    static const char *const sd_tags[] = {"sdmmc_cmd", "sdmmc_common", "sdmmc_init", "sdmmc_sd", "sdmmc_io", "sdspi_host", "sdspi_transaction"};
    for (unsigned i = 0; i < sizeof(sd_tags) / sizeof(sd_tags[0]); i++) {
        esp_log_level_set(sd_tags[i], ESP_LOG_DEBUG);
    }
    bool card_ok = card_init();
    for (unsigned i = 0; i < sizeof(sd_tags) / sizeof(sd_tags[0]); i++) {
        esp_log_level_set(sd_tags[i], ESP_LOG_INFO);
    }

    if (!card_ok) {
        printf("usb_msc: no SD card\n");
    } else {
        const uint64_t mb = ((uint64_t)s_sectors * s_sector_size) / (1024ULL * 1024ULL);
        printf("usb_msc: SD %u MB, %u sectors of %u bytes\n", (unsigned)mb, (unsigned)s_sectors, (unsigned)s_sector_size);
        xTaskCreate(writer_task, "sdwriter", 3072, nullptr, 5, nullptr);
    }

    draw_screen();

    // Take the PHY for the device stack. This is what costs the USB-Serial-JTAG
    // console: one PHY, and it is now OTG's.
    usb_phy_config_t phy_conf = {};
    phy_conf.controller = USB_PHY_CTRL_OTG;
    phy_conf.target = USB_PHY_TARGET_INT;
    phy_conf.otg_mode = USB_OTG_MODE_DEVICE;
    phy_conf.otg_speed = USB_PHY_SPEED_UNDEFINED;
    usb_phy_handle_t phy_hdl;
    esp_err_t perr = usb_new_phy(&phy_conf, &phy_hdl);
    if (perr != ESP_OK) {
        printf("usb_msc: usb_new_phy: %s\n", esp_err_to_name(perr));
        draw_usb_status(esp_err_to_name(perr));
    }
    // tusb_init() has a zero-argument form only when the legacy
    // CFG_TUSB_RHPORT0_MODE is defined. tusb_config.h uses the current
    // CFG_TUD_ENABLED form, so go straight to the function it wraps.
    const tusb_rhport_init_t rh_init = {
        .role = TUSB_ROLE_DEVICE,
        .speed = TUSB_SPEED_FULL,
    };
    if (!tusb_rhport_init(0, &rh_init)) {
        printf("usb_msc: tusb_rhport_init failed\n");
        draw_usb_status("tusb_rhport_init failed");
    } else if (perr == ESP_OK) {
        draw_usb_status("USB up, waiting for host");
    }
    xTaskCreate(tusb_task, "tusb", 4096, nullptr, 5, nullptr);

    printf("usb_msc: ready\n");
    for (;;) {
        // Settle the write-back cache once the host goes quiet, so an
        // unexpected unplug loses at most FLUSH_IDLE_MS of data.
        if (s_count && ((uint32_t)(esp_timer_get_time() / 1000) - s_last_write_ms) > FLUSH_IDLE_MS) {
            flush_cache();
        }
        drain_log_to_cdc();
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
