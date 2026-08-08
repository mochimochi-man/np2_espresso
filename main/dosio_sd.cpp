// dosio for ESP32 / SD — POSIX fd I/O version.
//
// IMPORTANT: np2's FDD reader opens/seeks/reads the disk image from inside
// pccore_exec(). newlib *stdio* (fopen/fseek/fread on FILE*, which Arduino's
// fs::File uses) CRASHES when called from that context. POSIX open/read/lseek
// (raw fds, no FILE/reent) work, so this layer uses them directly against the
// SD VFS mountpoint ("/sd"). SD.begin() is still used (to mount) by main.

#include <Arduino.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
// Pre-include the C++ standard headers compiler_base.h pulls in, so their include
// guards prevent re-inclusion (with C linkage) inside the extern "C" block below.
#include <cstdio>
#include <cstdlib>
#include <cstddef>
#include <cstring>
#include <cmath>
#include <climits>
#include <csetjmp>
#include <cstdarg>
#include <cinttypes>
#include <string>
#include <memory>

extern "C" {
#include <compiler.h>
#include "oemtext.h"
#include <dosio.h>
}

extern "C" int ets_printf(const char *fmt, ...);

// Concrete handle: dosio.h did `typedef RFILE * FILEH;`
struct RFILE { int fd; };

#ifndef OEMPATHDIVC
#define OEMPATHDIVC '/'
#endif

#define SD_MOUNT "/sd"     // Arduino SD default VFS mountpoint

static OEMCHAR curpath[MAX_PATH] = { '/', 0 };
static OEMCHAR *curfilep = curpath + 1;

// Map a core path ("/FD.NFD") to the SD VFS path ("/sd/FD.NFD").
static void map_path(char *out, size_t outsz, const OEMCHAR *path) {
    if (path && path[0] == '/')
        snprintf(out, outsz, "%s%s", SD_MOUNT, path);
    else
        snprintf(out, outsz, "%s/%s", SD_MOUNT, path ? path : "");
}

// ============================================================================
//  I/O worker task.
//
//  np2's FDD reader calls file_open/seek/read from *inside* pccore_exec(), deep
//  in the emulated-CPU call stack. Calling the SD/FAT VFS syscalls from there
//  faults (LoadProhibited, NULL ctx in the FAT handler), even though the very
//  same calls work at setup time (shallow stack). To be safe regardless of the
//  exact cause, every real ::open/::lseek/::read/... runs on a dedicated worker
//  task with a shallow internal-RAM stack; the emulator hands off a request and
//  blocks until the worker finishes. One requester (the emulator) at a time.
// ============================================================================
enum {
    IOP_OPEN, IOP_SEEK, IOP_READ, IOP_WRITE, IOP_CLOSE,
    IOP_FSIZE, IOP_ATTR, IOP_UNLINK, IOP_RENAME, IOP_MKDIR, IOP_RMDIR
};
struct IoReq {
    int op, fd, flags, whence;
    const char *path, *path2;
    void *buf; size_t len; off_t off;
    long result;
};
// 0 = run SD syscalls directly on the calling task (faster; safe now that the
// screen-draw overrun is fixed). Set to 1 to route them through a shallow worker
// task again (kept as a safety fallback).
#define USE_IO_WORKER 0

static IoReq            s_io;
static SemaphoreHandle_t s_io_req, s_io_done, s_io_mtx;

#if USE_IO_WORKER
static void io_task(void *) {
    for (;;) {
        xSemaphoreTake(s_io_req, portMAX_DELAY);
        IoReq &q = s_io;
        switch (q.op) {
        case IOP_OPEN:   q.result = ::open(q.path, q.flags, 0666); break;
        case IOP_SEEK:   q.result = (long)::lseek(q.fd, q.off, q.whence); break;
        case IOP_READ:   q.result = (long)::read(q.fd, q.buf, q.len); break;
        case IOP_WRITE:  q.result = (long)::write(q.fd, q.buf, q.len); break;
        case IOP_CLOSE:  q.result = ::close(q.fd); break;
        case IOP_FSIZE:  { struct stat st; q.result = (::fstat(q.fd, &st) == 0) ? (long)st.st_size : -1; } break;
        case IOP_ATTR:   { struct stat st; q.result = (::stat(q.path, &st) != 0) ? -1 : (S_ISDIR(st.st_mode) ? FILEATTR_DIRECTORY : FILEATTR_ARCHIVE); } break;
        case IOP_UNLINK: q.result = ::unlink(q.path); break;
        case IOP_RENAME: q.result = ::rename(q.path, q.path2); break;
        case IOP_MKDIR:  q.result = ::mkdir(q.path, 0777); break;
        case IOP_RMDIR:  q.result = ::rmdir(q.path); break;
        default:         q.result = -1; break;
        }
        xSemaphoreGive(s_io_done);
    }
}
#endif // USE_IO_WORKER
// Perform one syscall. The io worker exists only as a fallback: the real root
// cause of the earlier exec-context crashes was a screen-draw buffer overrun
// (fixed by gating drawscreen), not the VFS itself, so direct calls from the
// exec stack are safe — and avoid a per-sector cross-core context switch, which
// noticeably speeds up disk-heavy phases (DOS loading COMMAND.COM etc.).
static long io_call(const IoReq &r) {
    if (!USE_IO_WORKER || !s_io_mtx) {   // direct (default)
        const IoReq &q = r;
        switch (q.op) {
        case IOP_OPEN:   return ::open(q.path, q.flags, 0666);
        case IOP_SEEK:   return (long)::lseek(q.fd, q.off, q.whence);
        case IOP_READ:   return (long)::read(q.fd, q.buf, q.len);
        case IOP_WRITE:  return (long)::write(q.fd, q.buf, q.len);
        case IOP_CLOSE:  return ::close(q.fd);
        case IOP_FSIZE:  { struct stat st; return (::fstat(q.fd, &st) == 0) ? (long)st.st_size : -1; }
        case IOP_ATTR:   { struct stat st; return (::stat(q.path, &st) != 0) ? -1 : (S_ISDIR(st.st_mode) ? FILEATTR_DIRECTORY : FILEATTR_ARCHIVE); }
        case IOP_UNLINK: return ::unlink(q.path);
        case IOP_RENAME: return ::rename(q.path, q.path2);
        case IOP_MKDIR:  return ::mkdir(q.path, 0777);
        case IOP_RMDIR:  return ::rmdir(q.path);
        default:         return -1;
        }
    }
    xSemaphoreTake(s_io_mtx, portMAX_DELAY);
    s_io = r;
    xSemaphoreGive(s_io_req);
    xSemaphoreTake(s_io_done, portMAX_DELAY);
    long res = s_io.result;
    xSemaphoreGive(s_io_mtx);
    return res;
}

// ============================================================================
//  Persistent handle cache.
//
//  Reads/writes reuse one fd per image (opened once), so ::open never runs from
//  the exec context and per-sector open/close overhead disappears.
// ============================================================================
#define OPEN_CACHE_SLOTS 4
struct CacheSlot { char path[MAX_PATH + 8]; int fd; };
static CacheSlot s_cache[OPEN_CACHE_SLOTS];

// Return an fd for `full`, opening (once) O_RDWR with O_RDONLY fallback so the
// same fd serves both reads and writes. Cached across calls; -1 on failure.
static int cache_open(const char *full) {
    for (int i = 0; i < OPEN_CACHE_SLOTS; i++)
        if (s_cache[i].fd >= 0 && strcmp(s_cache[i].path, full) == 0)
            return s_cache[i].fd;
    IoReq r{}; r.op = IOP_OPEN; r.path = full; r.flags = O_RDWR;
    int fd = (int)io_call(r);
    if (fd < 0) { r.flags = O_RDONLY; fd = (int)io_call(r); }
    if (fd < 0) return -1;
    for (int i = 0; i < OPEN_CACHE_SLOTS; i++) {
        if (s_cache[i].fd < 0) {
            snprintf(s_cache[i].path, sizeof(s_cache[i].path), "%s", full);
            s_cache[i].fd = fd;
            return fd;
        }
    }
    // Cache full: usable but won't be reused (shouldn't happen for <=4 images).
    return fd;
}
// True if `fd` belongs to the cache (→ file_close must not really close it).
static bool cache_owns(int fd) {
    for (int i = 0; i < OPEN_CACHE_SLOTS; i++)
        if (s_cache[i].fd == fd) return true;
    return false;
}

extern "C" {

void dosio_init(void) {
    for (int i = 0; i < OPEN_CACHE_SLOTS; i++) s_cache[i].fd = -1;
#if USE_IO_WORKER
    // Optional fallback worker (shallow internal-RAM stack, other core).
    s_io_req  = xSemaphoreCreateBinary();
    s_io_done = xSemaphoreCreateBinary();
    s_io_mtx  = xSemaphoreCreateMutex();
    xTaskCreatePinnedToCore(io_task, "sdio", 16 * 1024, nullptr, 10, nullptr, 0);
#endif
}
void dosio_term(void) {}

// ---- open / create ----
// Reads/writes of existing files go through the persistent cache (see above).
static FILEH open_cached(const OEMCHAR *path) {
    char full[MAX_PATH + 8];
    map_path(full, sizeof(full), path);
    int fd = cache_open(full);
    if (fd < 0) return FILEH_INVALID;
    RFILE *h = new RFILE();
    h->fd = fd;
    return (FILEH)h;
}
FILEH file_open(const OEMCHAR *path)    { return open_cached(path); }
FILEH file_open_rb(const OEMCHAR *path) { return open_cached(path); }
// Create bypasses the cache (fresh O_CREAT|O_TRUNC fd, closed normally).
FILEH file_create(const OEMCHAR *path)  {
    char full[MAX_PATH + 8];
    map_path(full, sizeof(full), path);
    IoReq r{}; r.op = IOP_OPEN; r.path = full; r.flags = O_RDWR | O_CREAT | O_TRUNC;
    int fd = (int)io_call(r);
    if (fd < 0) return FILEH_INVALID;
    RFILE *h = new RFILE();
    h->fd = fd;
    return (FILEH)h;
}

// ---- seek / io / close (all real syscalls go through the io worker) ----
FILEPOS file_seek(FILEH handle, FILEPOS pointer, int method) {
    RFILE *h = (RFILE *)handle;
    if (!h) return 0;
    int w = (method == FSEEK_CUR) ? SEEK_CUR : (method == FSEEK_END) ? SEEK_END : SEEK_SET;
    IoReq r{}; r.op = IOP_SEEK; r.fd = h->fd; r.off = pointer; r.whence = w;
    long res = io_call(r);
    return (FILEPOS)(res < 0 ? 0 : res);
}
UINT file_read(FILEH handle, void *data, UINT length) {
    RFILE *h = (RFILE *)handle;
    if (!h) return 0;
    UINT done = 0;
    while (done < length) {
        IoReq r{}; r.op = IOP_READ; r.fd = h->fd;
        r.buf = (uint8_t *)data + done; r.len = length - done;
        long n = io_call(r);
        if (n <= 0) break;
        done += (UINT)n;
    }
    return done;
}
UINT file_write(FILEH handle, const void *data, UINT length) {
    RFILE *h = (RFILE *)handle;
    if (!h) return 0;
    IoReq r{}; r.op = IOP_WRITE; r.fd = h->fd;
    r.buf = (void *)data; r.len = length;
    long n = io_call(r);
    return (n < 0) ? 0 : (UINT)n;
}
short file_close(FILEH handle) {
    RFILE *h = (RFILE *)handle;
    if (!h) return -1;
    if (!cache_owns(h->fd)) {           // keep cached fds open
        IoReq r{}; r.op = IOP_CLOSE; r.fd = h->fd; io_call(r);
    }
    delete h;
    return 0;
}
FILELEN file_getsize(FILEH handle) {
    RFILE *h = (RFILE *)handle;
    if (!h) return 0;
    IoReq r{}; r.op = IOP_FSIZE; r.fd = h->fd;
    long sz = io_call(r);
    return (FILELEN)(sz < 0 ? 0 : sz);
}
short file_getdatetime(FILEH handle, DOSDATE *dosdate, DOSTIME *dostime) {
    (void)handle;
    if (dosdate) { dosdate->year = 2020; dosdate->month = 1; dosdate->day = 1; }
    if (dostime) { dostime->hour = 0; dostime->minute = 0; dostime->second = 0; }
    return 0;
}

// ---- path-based ops (via io worker) ----
short file_delete(const OEMCHAR *path) {
    char full[MAX_PATH + 8]; map_path(full, sizeof(full), path);
    IoReq r{}; r.op = IOP_UNLINK; r.path = full;
    return (io_call(r) == 0) ? 0 : -1;
}
short file_attr(const OEMCHAR *path) {
    char full[MAX_PATH + 8]; map_path(full, sizeof(full), path);
    IoReq r{}; r.op = IOP_ATTR; r.path = full;
    return (short)io_call(r);
}
short file_rename(const OEMCHAR *e, const OEMCHAR *n) {
    char fe[MAX_PATH + 8], fn[MAX_PATH + 8];
    map_path(fe, sizeof(fe), e); map_path(fn, sizeof(fn), n);
    IoReq r{}; r.op = IOP_RENAME; r.path = fe; r.path2 = fn;
    return (io_call(r) == 0) ? 0 : -1;
}
short file_dircreate(const OEMCHAR *path) {
    char full[MAX_PATH + 8]; map_path(full, sizeof(full), path);
    IoReq r{}; r.op = IOP_MKDIR; r.path = full;
    return (io_call(r) == 0) ? 0 : -1;
}
short file_dirdelete(const OEMCHAR *path) {
    char full[MAX_PATH + 8]; map_path(full, sizeof(full), path);
    IoReq r{}; r.op = IOP_RMDIR; r.path = full;
    return (io_call(r) == 0) ? 0 : -1;
}

// ================= current-directory helpers =================================
void file_setcd(const OEMCHAR *exepath) {
    file_cpyname(curpath, exepath, sizeof(curpath));
    curfilep = file_getname(curpath);
    *curfilep = '\0';
}
OEMCHAR *file_getcd(const OEMCHAR *path) {
    file_cpyname(curfilep, path, NELEMENTS(curpath) - (UINT)(curfilep - curpath));
    return curpath;
}
FILEH file_open_c(const OEMCHAR *path)    { return file_open(file_getcd(path)); }
FILEH file_open_rb_c(const OEMCHAR *path) { return file_open_rb(file_getcd(path)); }
FILEH file_create_c(const OEMCHAR *path)  { return file_create(file_getcd(path)); }
short file_delete_c(const OEMCHAR *path)  { return file_delete(file_getcd(path)); }
short file_attr_c(const OEMCHAR *path)    { return file_attr(file_getcd(path)); }

// ================= directory enumeration (unused for boot → minimal) ==========
FLISTH file_list1st(const OEMCHAR *dir, FLINFO *fli) { (void)dir; (void)fli; return FLISTH_INVALID; }
BRESULT file_listnext(FLISTH hdl, FLINFO *fli) { (void)hdl; (void)fli; return FAILURE; }
void file_listclose(FLISTH hdl) { (void)hdl; }

// ================= path string helpers (portable, from sdl/dosio.c) ==========
void file_catname(OEMCHAR *path, const OEMCHAR *name, int maxlen) {
    int csize;
    while (maxlen > 0) { if (*path == '\0') break; path++; maxlen--; }
    file_cpyname(path, name, maxlen);
    while ((csize = milstr_charsize(path)) != 0) {
        if ((csize == 1) && (*path == OEMPATHDIVC)) *path = OEMPATHDIVC;
        path += csize;
    }
}
OEMCHAR *file_getname(const OEMCHAR *path) {
    const OEMCHAR *ret = path; int csize;
    while ((csize = milstr_charsize(path)) != 0) {
        if ((csize == 1) && (*path == OEMPATHDIVC)) ret = path + 1;
        path += csize;
    }
    return (OEMCHAR *)ret;
}
void file_cutname(OEMCHAR *path) { OEMCHAR *p = file_getname(path); *p = '\0'; }
OEMCHAR *file_getext(const OEMCHAR *path) {
    const OEMCHAR *p = file_getname(path); const OEMCHAR *q = NULL;
    while (*p != '\0') { if (*p == '.') q = p + 1; p++; }
    if (q == NULL) q = p;
    return (OEMCHAR *)q;
}
void file_cutext(OEMCHAR *path) {
    OEMCHAR *p = file_getname(path); OEMCHAR *q = NULL;
    while (*p != '\0') { if (*p == '.') q = p; p++; }
    if (q != NULL) *q = '\0';
}
void file_cutseparator(OEMCHAR *path) {
    int pos = (int)strlen(path) - 1;
    if ((pos > 0) && (path[pos] == OEMPATHDIVC) && ((pos != 1) || (path[0] != '.')))
        path[pos] = '\0';
}
void file_setseparator(OEMCHAR *path, int maxlen) {
    int pos = (int)OEMSTRNLEN(path, maxlen);
    if ((pos) && (path[pos - 1] != OEMPATHDIVC) && ((pos + 2) < maxlen)) {
        path[pos++] = OEMPATHDIVC; path[pos] = '\0';
    }
}

} // extern "C"
