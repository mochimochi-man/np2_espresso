// Disk image swap menu (runtime FDD1/FDD2/HDD exchange).
//
// Modal text UI on the ST7789. Entered via the Pause/Break key (g_menu_req is
// set in usb_kbd.cpp); the emulator loop calls menu_disk_run() and pauses
// pccore_exec while it is active. Runs on the emulator task (core 1), so SD
// file operations happen in the same safe context as the boot-time mounts
// (fopen inside pccore_exec crashes — that is why the delayed setfdd path is
// not used).

#include <Arduino.h>
#include <SD.h>
#include <nvs.h>
#include <esp_system.h>   // esp_restart() for the RESET menu item
#include <dirent.h>       // POSIX opendir/readdir over the SD VFS ("/sd")

extern "C" {
#include <compiler.h>
#include <pccore.h>
#include <diskdrv.h>
#include <dosio.h>
#include <fdd/sxsi.h>
#include <fdd/newdisk.h>
int  ets_printf(const char *fmt, ...);
int  usb_kbd_pop(uint8_t *nkey, uint8_t *down);   // usb_kbd.cpp
}

extern volatile int g_speed_req;    // main.cpp applies the new CPU multiple

// LCD text helpers (lcd_st7789.cpp)
extern "C" void lcd_menu_clear(void);
extern "C" void lcd_menu_line(int row, const char *s, uint16_t fg, uint16_t bg);
extern "C" int  lcd_get_scale_mode(void);        // lcd_st7789.cpp
extern "C" void lcd_set_scale_mode(int m);
extern "C" int  lcd_scale_mode_count(void);
extern "C" int  lcd_get_panel(void);             // 0 = ST7789, 1 = ST7796
extern "C" int  lcd_panel_count(void);
extern "C" void lcd_set_panel(int p);            // applied on the next boot
extern "C" const char *lcd_panel_name(void);      // number of scaler modes to cycle
extern "C" int  lcd_get_spi_idx(void);           // LCD SPI clock (index into 80/40/20MHz)
extern "C" void lcd_set_spi_idx(int i);
extern "C" int  lcd_spi_idx_count(void);
extern "C" int  lcd_spi_mhz(void);               // current clock, for the menu label
extern "C" int  lcd_get_rgb444(void);            // 12-bit pixel output (persisted)
extern "C" void lcd_set_rgb444(int on);

// nkey codes used for navigation. The numeric keypad doubles as arrows here:
// keypad 8/2 = up/down and keypad 4/6 = left/right (keypad Enter already maps
// to RETURN).
enum { NK_ESC = 0x00, NK_RET = 0x1c, NK_UP = 0x3a, NK_DOWN = 0x3d,
       NK_LEFT = 0x3b, NK_RIGHT = 0x3c, NK_KP8 = 0x43, NK_KP2 = 0x4b,
       NK_KP4 = 0x46, NK_KP6 = 0x48 };

// Treat main-row arrows and the keypad the same for menu navigation.
static inline bool key_is_up(uint8_t nk)    { return nk == NK_UP    || nk == NK_KP8; }
static inline bool key_is_down(uint8_t nk)  { return nk == NK_DOWN  || nk == NK_KP2; }
static inline bool key_is_left(uint8_t nk)  { return nk == NK_LEFT  || nk == NK_KP4; }
static inline bool key_is_right(uint8_t nk) { return nk == NK_RIGHT || nk == NK_KP6; }

// RGB565 colors (same values as TFT_eSPI's TFT_* macros)
#define COL_WHITE  0xFFFF
#define COL_BLACK  0x0000
#define COL_YELLOW 0xFFE0

// Shown on whichever value row is selected, so the arrows are discoverable
// without a row of instructions above the menu.
#define VALUE_HINT "   (LEFT/RIGHT or RET)"

// The rows that hold a value rather than perform an action - CPU clock, DISP,
// LCD SPI and LCD color - all behave the same way:
//
//   LEFT / RIGHT   step down or up, and STOP at the ends
//   RET            step up, and wrap round to the bottom at the top
//
// RET wraps because that is how it has always worked here and it is the only
// way through the list with one key; the arrows stop, because when the value
// you want is one step back, wrapping all the way round is not an answer.
static void adjust_row(int sel, int dir, bool wrap) {
    switch (sel) {
    case 4: {                       // CPU clock: x1..x5
        int m = (int)np2cfg.multiple + dir;
        if (m > 5) m = wrap ? 1 : 5;
        if (m < 1) m = wrap ? 5 : 1;
        np2cfg.multiple = (UINT)m;  // the row follows immediately
        g_speed_req = m;            // the emulator loop applies it
        break;
    }
    case 5: {                       // panel x scaler, in one cycle
        // Panel and scaler share a row: which panel is attached is set once and
        // never again, so it does not deserve a row of its own. The panel part
        // only takes effect after RESET.
        const int n = lcd_scale_mode_count();
        const int total = lcd_panel_count() * n;
        int idx = lcd_get_panel() * n + lcd_get_scale_mode() + dir;
        if (wrap) idx = (idx % total + total) % total;
        else if (idx < 0) idx = 0;
        else if (idx >= total) idx = total - 1;
        lcd_set_panel(idx / n);
        lcd_set_scale_mode(idx % n);
        break;
    }
    case 6: {                       // LCD SPI clock
        // The table runs 80/40/20MHz, so a higher index is a LOWER clock. What
        // the row shows is the megahertz, and RIGHT has to raise the number on
        // screen - so the index moves against the direction, not with it.
        const int n = lcd_spi_idx_count();
        int idx = lcd_get_spi_idx() - dir;
        if (wrap) idx = (idx % n + n) % n;
        else if (idx < 0) idx = 0;
        else if (idx >= n) idx = n - 1;
        lcd_set_spi_idx(idx);
        break;
    }
    case 7:                         // LCD colour depth: RGB565 <-> RGB444
        // Two states, so there is no far end to stop at and no direction to
        // tell apart - either arrow, and RET, all do the same thing.
        lcd_set_rgb444(!lcd_get_rgb444());
        break;
    default:
        break;
    }
}

#define MAX_ENTRIES 24
#define NAME_LEN    64
#define VISIBLE     20               // rows below the title

static char s_names[MAX_ENTRIES][NAME_LEN];
static int  s_count;

#define ROMNAME_LEN 40             // main.cpp sizes the buffers to match
extern "C" char g_bios_file[ROMNAME_LEN];   // "" = the built-in default
extern "C" char g_font_file[ROMNAME_LEN];
static void save_settings(void);   // defined below; used by the HDD-eject reboot

static const char *drv_label(int d) {
    return d == 0 ? "FDD1" : d == 1 ? "FDD2" : "HDD ";
}

static const char *drv_current(int d) {
    if (d == 2) return np2cfg.sasihdd[0][0] ? (const char *)np2cfg.sasihdd[0] : "(empty)";
    return np2cfg.fddfile[d][0] ? (const char *)np2cfg.fddfile[d] : "(empty)";
}

static int ext_match(const char *name, int drive) {
    const char *dot = strrchr(name, '.');
    if (!dot) return 0;
    char e[8]; int i = 0;
    for (const char *p = dot + 1; *p && i < 7; p++) e[i++] = tolower((uint8_t)*p);
    e[i] = 0;
    if (drive == 2)
        return !strcmp(e, "hdi") || !strcmp(e, "nhd") || !strcmp(e, "thd") || !strcmp(e, "vhd");
    return !strcmp(e, "nfd") || !strcmp(e, "d88") || !strcmp(e, "fdi") || !strcmp(e, "fdd") || !strcmp(e, "hdm");
}

static void draw_drives(int sel) {
    lcd_menu_clear();
    for (int i = 0; i < 3; i++) {
        char line[96];
        snprintf(line, sizeof(line), "%s %s: %.30s", i == sel ? ">" : " ",
                 drv_label(i), drv_current(i));
        lcd_menu_line(2 + i, line, i == sel ? COL_BLACK : COL_WHITE,
                      i == sel ? COL_YELLOW : COL_BLACK);
    }
    {
        char line[64];
        snprintf(line, sizeof(line), "%s Create New Disk", sel == 3 ? ">" : " ");
        lcd_menu_line(5, line, sel == 3 ? COL_BLACK : COL_WHITE,
                      sel == 3 ? COL_YELLOW : COL_BLACK);
        // Rows 4-7 hold a value rather than perform an action, so they all take
        // LEFT/RIGHT as well as RET - see adjust_row(). The hint is shown only
        // on the selected row, because four copies of it is four too many.
        snprintf(line, sizeof(line), "%s CPU clock: x%u%s",
                 sel == 4 ? ">" : " ", (unsigned)np2cfg.multiple,
                 sel == 4 ? VALUE_HINT : "");
        lcd_menu_line(6, line, sel == 4 ? COL_BLACK : COL_WHITE,
                      sel == 4 ? COL_YELLOW : COL_BLACK);
        // Index = lcd_st7789.cpp scaler mode. 1:1 crops to the panel's top-left
        // corner on the 320x240 LCD; it is for a 640x480 panel (whole screen).
        static const char *const scnames[4] = { "MAX(bright)", "AVG(mix4)",
                                                "MID(min+max)", "1:1(no scale)" };
        snprintf(line, sizeof(line), "%s DISP: %s/%s%s", sel == 5 ? ">" : " ",
                 lcd_panel_name(), scnames[lcd_get_scale_mode()],
                 sel == 5 ? VALUE_HINT : "");
        lcd_menu_line(7, line, sel == 5 ? COL_BLACK : COL_WHITE,
                      sel == 5 ? COL_YELLOW : COL_BLACK);
        // LCD SPI clock. 80MHz is fastest but out of spec for ST7789 and unstable
        // on some panels/wiring (noise, dropped pixels, blank screen); the change
        // shows up on the next redraw, i.e. on this very menu.
        snprintf(line, sizeof(line), "%s LCD SPI: %dMHz%s",
                 sel == 6 ? ">" : " ", lcd_spi_mhz(), sel == 6 ? VALUE_HINT : "");
        lcd_menu_line(8, line, sel == 6 ? COL_BLACK : COL_WHITE,
                      sel == 6 ? COL_YELLOW : COL_BLACK);
        // 12-bit pixels: 25% less SPI traffic, exact for the PC-98 4-bit palette.
        // Saved to NVS with the rest: a display whose 12-bit decoding is wrong
        // (an ST7789 emulator, say) has to be able to stay on RGB565.
        snprintf(line, sizeof(line), "%s LCD color: %s%s",
                 sel == 7 ? ">" : " ", lcd_get_rgb444() ? "RGB444" : "RGB565",
                 sel == 7 ? VALUE_HINT : "");
        lcd_menu_line(9, line, sel == 7 ? COL_BLACK : COL_WHITE,
                      sel == 7 ? COL_YELLOW : COL_BLACK);
        // Which BIOS to boot with. The ROM is read once at pccore_init, so the
        // change only takes effect on the next boot - hence the RESET note.
        // The ROM in use, chosen from whatever BIOS*.ROM / FONT*.ROM sit in the SD
        // root. Both are read once at pccore_init, so a change needs a reboot.
        snprintf(line, sizeof(line), "%s BIOS: %.24s", sel == 8 ? ">" : " ",
                 g_bios_file[0] ? g_bios_file : "/BIOS.ROM (default)");
        lcd_menu_line(10, line, sel == 8 ? COL_BLACK : COL_WHITE,
                      sel == 8 ? COL_YELLOW : COL_BLACK);
        snprintf(line, sizeof(line), "%s FONT: %.24s", sel == 9 ? ">" : " ",
                 g_font_file[0] ? g_font_file : "/FONT.ROM (default)");
        lcd_menu_line(11, line, sel == 9 ? COL_BLACK : COL_WHITE,
                      sel == 9 ? COL_YELLOW : COL_BLACK);
        snprintf(line, sizeof(line), "%s RESET (save & reboot)", sel == 10 ? ">" : " ");
        lcd_menu_line(12, line, sel == 10 ? COL_BLACK : COL_WHITE,
                      sel == 10 ? COL_YELLOW : COL_BLACK);
        // No USB mass storage mode on this board. The ESP32-S3 has one USB
        // peripheral, and the keyboard is already using it as a host - so a card
        // reader can only exist by unplugging the keyboard for the duration,
        // which is not a feature anyone would use twice.
    }
}

// Collect matching SD images into s_names. Returns entry count.
static int scan_images(int drive) {
    s_count = 0;
    // Inject the currently-mounted image first (belt-and-suspenders; de-duped
    // below) so a drive never shows "No Image" while an image is mounted.
    const char *mnt = (drive == 2) ? (const char *)np2cfg.sasihdd[0]
                                   : (const char *)np2cfg.fddfile[drive];
    if (mnt && mnt[0]) {
        const char *mb = strrchr(mnt, '/'); mb = mb ? mb + 1 : mnt;
        if (ext_match(mb, drive)) {
            snprintf(s_names[s_count], NAME_LEN, "/%s", mb);
            s_count++;
        }
    }
    // List the SD root with POSIX readdir. Unlike Arduino SD's openNextFile()
    // (which open()s each entry and stops at the first file already held open by
    // the emulator, e.g. FONT.ROM/HDD.NHD), readdir only reads directory entries,
    // so every image is listed whether or not it is currently mounted.
    DIR *dir = opendir("/sd");        // Arduino SD VFS mountpoint (see dosio_sd.cpp)
    if (!dir) return s_count;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL && s_count < MAX_ENTRIES) {
        const char *base = ent->d_name;
        if (!ext_match(base, drive)) continue;
        int dup = 0;                        // s_names entries are "/BASE"
        for (int i = 0; i < s_count; i++)
            if (!strcmp(s_names[i] + 1, base)) { dup = 1; break; }
        if (!dup) {
            snprintf(s_names[s_count], NAME_LEN, "/%s", base);
            s_count++;
        }
    }
    closedir(dir);
    return s_count;
}

// ROM chooser. The SD root is listed for BIOS*.ROM / FONT*.ROM (case-insensitive),
// which is how a compatible ROM built for this port and a ROM dumped from real
// hardware end up side by side: BIOS.ROM and BIOS_ESP.ROM both match, and the
// choice is a filename rather than a flag, so any number of them can coexist.
static int scan_roms(const char *prefix) {
    s_count = 0;
    DIR *dir = opendir("/sd");
    if (!dir) return 0;
    const size_t pl = strlen(prefix);
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL && s_count < MAX_ENTRIES) {
        const char *b = ent->d_name;
        if (strncasecmp(b, prefix, pl) != 0) continue;
        const char *dot = strrchr(b, '.');
        if (!dot || strcasecmp(dot, ".rom") != 0) continue;
        snprintf(s_names[s_count], NAME_LEN, "/%s", b);
        s_count++;
    }
    closedir(dir);
    return s_count;
}

// which: 0 = BIOS, 1 = FONT. Writes the chosen name into g_bios_file/g_font_file.
static void browse_rom(int which) {
    const char *prefix = which ? "FONT" : "BIOS";
    char *dst = which ? g_font_file : g_bios_file;
    scan_roms(prefix);
    // The ROM built into the firmware is offered alongside whatever is on the
    // card, as the first entry. It is not a file, so scan_roms() cannot find it,
    // and without a row here there is no way to ask for it once a card carrying
    // a ROM is in the slot - which is every card that has ever worked.
    //
    // dosio_sd.cpp answers these two paths out of flash; everything between here
    // and there treats them as ordinary filenames.
    const char *const builtin = which ? ":builtin/FONT.ROM" : ":builtin/BIOS.ROM";
    const int total = s_count + 1;
    int sel = 0, top = 0;
    for (;;) {
        lcd_menu_clear();
        char hdr[64];
        snprintf(hdr, sizeof(hdr), "  select %s ROM (needs RESET)", prefix);
        lcd_menu_line(0, hdr, COL_YELLOW, COL_BLACK);
        for (int r = 0; r < VISIBLE; r++) {
            const int idx = top + r;
            if (idx >= total) break;
            char line[80];
            snprintf(line, sizeof(line), "%s %.38s", idx == sel ? ">" : " ",
                     idx == 0 ? (which ? "(built-in font)" : "(built-in compatible BIOS)")
                              : s_names[idx - 1]);
            lcd_menu_line(2 + r, line, idx == sel ? COL_BLACK : COL_WHITE,
                          idx == sel ? COL_YELLOW : COL_BLACK);
        }
        uint8_t nk, dn;
        for (;;) {
            if (usb_kbd_pop(&nk, &dn) && dn) break;
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        if (nk == NK_ESC) return;
        if (key_is_up(nk)   && sel > 0)            sel--;
        if (key_is_down(nk) && sel < total - 1)    sel++;
        if (sel < top) top = sel;
        if (sel >= top + VISIBLE) top = sel - VISIBLE + 1;
        if (nk == NK_RET) {
            snprintf(dst, ROMNAME_LEN, "%s", sel == 0 ? builtin : s_names[sel - 1]);
            ets_printf("menu: %s ROM <- %s\n", prefix, dst);
            return;
        }
    }
}

static void draw_files(int drive, int sel, int top) {
    lcd_menu_clear();
    // The row at index s_count is the drive's eject entry. FDD ejects live; HDD
    // (the running system disk) ejects by persisting the empty state and
    // rebooting, so the machine comes back up in N88-BASIC / a floppy instead of
    // having the booted disk yanked out from under the OS.
    const char *ejlabel = (drive == 2) ? "(eject HDD & reboot)" : "(eject / empty)";
    for (int r = 0; r < VISIBLE; r++) {
        int idx = top + r;
        if (idx > s_count) break;
        char line[80];
        const char *nm = (idx == s_count) ? ejlabel : s_names[idx];
        snprintf(line, sizeof(line), "%s %.38s", idx == sel ? ">" : " ", nm);
        lcd_menu_line(2 + r, line, idx == sel ? COL_BLACK : COL_WHITE,
                      idx == sel ? COL_YELLOW : COL_BLACK);
    }
}

static void apply_image(int drive, const char *path) {
    if (drive == 2) {
        diskdrv_setsxsi(0x00, (const OEMCHAR *)path);
        diskdrv_hddbind();
    } else if (path) {
        diskdrv_readyfddex((REG8)drive, (const OEMCHAR *)path, FTYPE_NONE, 0);
    } else {
        diskdrv_setfddex((REG8)drive, NULL, FTYPE_NONE, 0);   // eject
    }
    ets_printf("menu: %s <- %s\n", drv_label(drive), path ? path : "(eject)");
}

// File browser for one drive; returns when an image was applied or ESC.
static void browse(int drive) {
    scan_images(drive);
    int total = s_count + 1;                       // +1: eject entry (FDD live, HDD eject+reboot)
    lcd_menu_clear();                             // leave the drive list cleanly (no overlap)
    if (total == 0) {
        lcd_menu_line(2, "  no images on SD", COL_WHITE, COL_BLACK);
        vTaskDelay(pdMS_TO_TICKS(1200));
        return;
    }
    int sel = 0, top = 0;
    draw_files(drive, sel, top);
    for (;;) {
        uint8_t nk, dn;
        if (!usb_kbd_pop(&nk, &dn)) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }
        if (!dn) continue;
        if (nk == NK_ESC) return;
        if (key_is_up(nk)   && sel > 0)          sel--;
        if (key_is_down(nk) && sel < total - 1)  sel++;
        if (sel < top) top = sel;
        if (sel >= top + VISIBLE) top = sel - VISIBLE + 1;
        if (nk == NK_RET) {
            if (sel == s_count) {                 // eject entry
                if (drive == 2) {
                    // Eject the running HDD: persist the now-empty HDD and reboot,
                    // so the machine comes up in N88-BASIC / a floppy instead of
                    // live-unmounting the disk the OS is booted from.
                    np2cfg.sasihdd[0][0] = '\0';
                    lcd_menu_clear();
                    lcd_menu_line(2, "  HDD ejected - rebooting...", COL_WHITE, COL_BLACK);
                    save_settings();
                    vTaskDelay(pdMS_TO_TICKS(800));
                    esp_restart();
                }
                apply_image(drive, NULL);         // FDD: live eject
            } else {
                apply_image(drive, s_names[sel]);
            }
            vTaskDelay(pdMS_TO_TICKS(600));
            return;
        }
        draw_files(drive, sel, top);
    }
}

// nkey -> ASCII for filename entry (uppercase letters, digits, '-')
static char nkey_ascii(uint8_t nk) {
    if (nk >= 0x01 && nk <= 0x09) return (char)('1' + nk - 1);
    if (nk == 0x0a) return '0';
    if (nk == 0x0b) return '-';
    static const char letters[0x30] = {
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,                  // 0x00-0x0f
        'q','w','e','r','t','y','u','i','o','p',0,0,0,'a','s','d', // 0x10-0x1f
        'f','g','h','j','k','l',0,0,0,'z','x','c','v','b','n','m'  // 0x20-0x2f
    };
    if (nk < 0x30 && letters[nk]) return (char)(letters[nk] - 'a' + 'A');
    switch (nk) {                        // numeric keypad digits KP0..KP9
        case 0x4e: return '0'; case 0x4a: return '1'; case 0x4b: return '2';
        case 0x4c: return '3'; case 0x46: return '4'; case 0x47: return '5';
        case 0x48: return '6'; case 0x42: return '7'; case 0x43: return '8';
        case 0x44: return '9';
    }
    return 0;
}

static uint8_t wait_key(void) {
    uint8_t nk, dn;
    for (;;) {
        if (!usb_kbd_pop(&nk, &dn)) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }
        if (dn) return nk;
    }
}

// Create a blank FD/HDD image on the SD card (name typed on USB keyboard).
static void newdisk_flow(void) {
    static const struct { const char *label; int mb; } types[] = {
        { "FD 1.44MB (2HD .HDM)", 0 },
        { "HDD 20MB (.NHD)", 20 },
        { "HDD 40MB (.NHD)", 40 },
        { "HDD 80MB (.NHD)", 80 },
    };
    int sel = 0;
    for (;;) {
        lcd_menu_clear();
        lcd_menu_line(1, "  New blank disk: type", COL_WHITE, COL_BLACK);
        for (int i = 0; i < 4; i++) {
            char line[64];
            snprintf(line, sizeof(line), "%s %s", i == sel ? ">" : " ", types[i].label);
            lcd_menu_line(3 + i, line, i == sel ? COL_BLACK : COL_WHITE,
                          i == sel ? COL_YELLOW : COL_BLACK);
        }
        uint8_t nk = wait_key();
        if (nk == NK_ESC) return;
        if (key_is_up(nk)   && sel > 0) sel--;
        if (key_is_down(nk) && sel < 3) sel++;
        if (nk == NK_RET) break;
    }
    int mb = types[sel].mb;

    // base name entry: up to 8 chars (8.3 style), RET to confirm
    char name[9]; int len = 0; name[0] = 0;
    for (;;) {
        lcd_menu_clear();
        lcd_menu_line(1, "  File name (A-Z 0-9 -)", COL_WHITE, COL_BLACK);
        char line[32];
        snprintf(line, sizeof(line), "  %s_", name);
        lcd_menu_line(3, line, COL_WHITE, COL_BLACK);
        lcd_menu_line(5, "  RET:create BS:del ESC:cancel", COL_WHITE, COL_BLACK);
        uint8_t nk = wait_key();
        if (nk == NK_ESC) return;
        if (nk == NK_RET) { if (len > 0) break; continue; }
        if (nk == 0x0e) { if (len > 0) name[--len] = 0; continue; }  // Backspace
        char c = nkey_ascii(nk);
        if (c && len < 8) { name[len++] = c; name[len] = 0; }
    }

    char path[24];
    snprintf(path, sizeof(path), "/%s.%s", name, mb ? "NHD" : "HDM");
    lcd_menu_clear();
    lcd_menu_line(1, "  New blank disk:", COL_WHITE, COL_BLACK);
    lcd_menu_line(2, path, COL_WHITE, COL_BLACK);

    if (SD.exists(path)) {
        lcd_menu_line(4, "  exists! aborted", COL_WHITE, COL_BLACK);
        vTaskDelay(pdMS_TO_TICKS(1500));
        return;
    }
    lcd_menu_line(4, "  creating...", COL_WHITE, COL_BLACK);
    if (mb == 0) {
        newdisk_144mb_fdd((const OEMCHAR *)path);
    } else {
        int prog = 0, cancel = 0;
        newdisk_nhd_ex((const OEMCHAR *)path, (UINT)mb, 1, &prog, &cancel);
    }
    lcd_menu_line(4, "  done (format in DOS)", COL_WHITE, COL_BLACK);
    vTaskDelay(pdMS_TO_TICKS(1500));
}

// Persist the 5 menu items — CPU multiple, scaler mode, and the FDD1/FDD2/HDD
// mounts — called on menu ESC exit and on RESET. main.cpp reloads these at boot
// and restores the disks. NVS commit only writes flash when a value actually
// changed, so this is wear-safe.
static void save_settings(void) {
    nvs_handle_t nh;
    if (nvs_open("pc98", NVS_READWRITE, &nh) == ESP_OK) {
        nvs_set_u8(nh, "multiple", (uint8_t)np2cfg.multiple);
        nvs_set_u8(nh, "scaler", (uint8_t)lcd_get_scale_mode());
        nvs_set_u8(nh, "spiidx", (uint8_t)lcd_get_spi_idx());
        nvs_set_u8(nh, "rgb444", (uint8_t)lcd_get_rgb444());
        nvs_set_u8(nh, "panel", (uint8_t)lcd_get_panel());
        nvs_set_str(nh, "biosfile", g_bios_file);
        nvs_set_str(nh, "fontfile", g_font_file);
        // Current mounts (empty drive => empty string; restored as empty at boot).
        nvs_set_str(nh, "fdd0", (const char *)np2cfg.fddfile[0]);
        nvs_set_str(nh, "fdd1", (const char *)np2cfg.fddfile[1]);
        nvs_set_str(nh, "hdd",  (const char *)np2cfg.sasihdd[0]);
        nvs_commit(nh);
        nvs_close(nh);
    }
}

extern "C" void menu_disk_run(void) {
    int sel = 0;
    draw_drives(sel);
    for (;;) {
        uint8_t nk, dn;
        if (!usb_kbd_pop(&nk, &dn)) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }
        if (!dn) continue;
        if (nk == NK_ESC) break;
        if (key_is_up(nk)   && sel > 0) sel--;
        if (key_is_down(nk) && sel < 10) sel++;
        // The value rows take the arrows too, stopping at the ends.
        if (sel >= 4 && sel <= 7) {
            const int dir = key_is_right(nk) ? 1 : (key_is_left(nk) ? -1 : 0);
            if (dir) adjust_row(sel, dir, false);
        }
        if (nk == NK_RET) {
            if (sel == 3) {                     // create blank disk image
                newdisk_flow();
            } else if (sel >= 4 && sel <= 7) {  // a value row: step up, wrapping
                adjust_row(sel, 1, true);
            } else if (sel == 8) {              // BIOS ROM: pick a file from the SD
                browse_rom(0);
            } else if (sel == 9) {              // FONT ROM: pick a file from the SD
                browse_rom(1);
            } else if (sel == 10) {             // RESET: persist the settings, reboot
                save_settings();
                esp_restart();
            } else {
                browse(sel);                    // sel 0/1/2 = FDD1/FDD2/HDD
            }
        }
        draw_drives(sel);
    }
    save_settings();                        // ESC exit: persist multiple + scaler
}
