#include "image_store.h"
#include "systime.h"
#include "ff.h"
#include "sd_sdio.h"
#include "debug.h"
#include <string.h>
#include <stdlib.h>

static FATFS fs;
static FIL   fil;
static DWORD clmt[2 * IMG_MAX_EXTENTS + 2];      /* FatFs cluster link map: [size, (n, start)..., 0] */

img_entry_t img_list[IMG_MAX];
uint32_t    img_count;
img_mount_t img_cur;
bool        img_fs_ok;
char        img_fs_type[8] = "none";

static char    last_name[IMG_NAME_MAX];
static uint8_t last_subdir;
static bool    last_rw, have_last;

/* ---- helpers ----------------------------------------------------------- */
static int lower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

static int ci_cmp(const char *a, const char *b)
{
    while (*a && lower(*a) == lower(*b)) { a++; b++; }
    return lower(*a) - lower(*b);
}

static img_kind_t kind_of(const char *name)
{
    size_t n = strlen(name);
    if (n >= 6 && name[n - 5] == '.' && ci_cmp(name + n - 4, "vmdk") == 0)
    {
        /* the data file of a flat VMDK is listed through its descriptor, not on its own */
        if (n >= 10 && ci_cmp(name + n - 10, "-flat.vmdk") == 0)
            return IMG_KIND_NONE;
        return IMG_KIND_DISK;
    }
    if (n < 5 || name[n - 4] != '.')
        return IMG_KIND_NONE;
    if (ci_cmp(name + n - 3, "iso") == 0)
        return IMG_KIND_ISO;
    if (ci_cmp(name + n - 3, "img") == 0)
        return IMG_KIND_DISK;
    if (ci_cmp(name + n - 3, "vhd") == 0)
        return IMG_KIND_DISK;
    return IMG_KIND_NONE;
}

/* ---- table sector cache + file-sector mapping for the container formats ---- */
static uint32_t be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static uint32_t le32(const uint8_t *p) { return ((uint32_t)p[3] << 24) | ((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | p[0]; }
static uint64_t be64(const uint8_t *p) { return ((uint64_t)be32(p) << 32) | be32(p + 4); }
static uint64_t le64(const uint8_t *p) { return ((uint64_t)le32(p + 4) << 32) | le32(p); }

/* map a sector of the data FILE to the card (the extent table built at mount) */
static uint32_t file_map(uint32_t fsec, uint32_t count, uint32_t *sd_sector)
{
    uint32_t lo, hi, run;
    const img_extent_t *x;
    if (count == 0 || img_cur.n_extents == 0 || fsec >= img_cur.file_sectors)
        return 0;
    if (fsec + count > img_cur.file_sectors)
        count = img_cur.file_sectors - fsec;
    lo = 0; hi = img_cur.n_extents - 1;
    while (lo < hi)                          /* last extent whose file_sector <= fsec */
    {
        uint32_t mid = (lo + hi + 1u) / 2u;
        if (img_cur.ext[mid].file_sector <= fsec) lo = mid; else hi = mid - 1u;
    }
    x = &img_cur.ext[lo];
    if (fsec >= x->file_sector + x->sectors)
        return 0;
    run = x->file_sector + x->sectors - fsec;
    if (run > count)
        run = count;
    *sd_sector = x->sd_sector + (fsec - x->file_sector);
    return run;
}

/* Direct-mapped cache of file sectors holding allocation tables (VHD BAT, VMDK
 * grain directory / tables) and headers: 64 x 512 B.  Invalidated on mount. */
#define TCACHE_SLOTS 64u
static __attribute__((aligned(16))) uint8_t tcache[TCACHE_SLOTS][512];
static uint32_t tcache_tag[TCACHE_SLOTS];      /* file sector + 1, 0 = empty */

static void tcache_clear(void) { memset(tcache_tag, 0, sizeof tcache_tag); }

static const uint8_t *tcache_get(uint32_t fsec)
{
    unsigned slot = fsec % TCACHE_SLOTS;
    uint32_t sd;
    if (tcache_tag[slot] == fsec + 1u)
        return tcache[slot];
    if (file_map(fsec, 1, &sd) != 1 || sd_read_blocks(tcache[slot], sd, 1) != SD_OK)
    {
        tcache_tag[slot] = 0;
        return NULL;
    }
    tcache_tag[slot] = fsec + 1u;
    return tcache[slot];
}

const char *img_fmt_name(void)
{
    switch (img_cur.fmt)
    {
        case IMG_FMT_VHD_FIXED:   return "VHD fixed";
        case IMG_FMT_VHD_DYN:     return "VHD dynamic";
        case IMG_FMT_VMDK_FLAT:   return "VMDK flat";
        case IMG_FMT_VMDK_SPARSE: return "VMDK sparse";
        default:                  return "raw";
    }
}

const char *img_fmt_tag(void)
{
    switch (img_cur.fmt)
    {
        case IMG_FMT_VHD_FIXED:   return "VHD";
        case IMG_FMT_VHD_DYN:     return "VHDd";
        case IMG_FMT_VMDK_FLAT:   return "VMDK";
        case IMG_FMT_VMDK_SPARSE: return "VMDKs";
        default:                  return "disk";
    }
}

static void path_of(char *out, size_t n, const char *name, uint8_t sub)
{
    if (sub)
        snprintf(out, n, "/%s/%s", IMG_SUBDIR, name);
    else
        snprintf(out, n, "/%s", name);
}

static const char *fs_type_name(BYTE t)
{
    switch (t)
    {
        case FS_FAT12: return "FAT12";
        case FS_FAT16: return "FAT16";
        case FS_FAT32: return "FAT32";
        case FS_EXFAT: return "exFAT";
        default:       return "none";
    }
}

/* ---- scanning ---------------------------------------------------------- */
static void scan_dir(const char *path, uint8_t sub)
{
    DIR dir;
    FILINFO fno;
    if (f_opendir(&dir, path) != FR_OK)
        return;
    while (img_count < IMG_MAX && f_readdir(&dir, &fno) == FR_OK && fno.fname[0])
    {
        img_entry_t *e;
        img_kind_t kind;
        if (fno.fattrib & (AM_DIR | AM_HID | AM_SYS))
            continue;
        if (fno.fname[0] == '.')            /* macOS ._ resource forks etc. */
            continue;
        kind = kind_of(fno.fname);
        if (kind == IMG_KIND_NONE)
            continue;
        if (fno.fsize < 2048)
        {
            /* too small to be an image -- unless it is a VMDK descriptor (a few hundred bytes of text) */
            size_t ln = strlen(fno.fname);
            if (!(ln >= 6 && ci_cmp(fno.fname + ln - 5, ".vmdk") == 0))
                continue;
        }
        e = &img_list[img_count++];
        strncpy(e->name, fno.fname, IMG_NAME_MAX - 1);
        e->name[IMG_NAME_MAX - 1] = 0;
        e->size = fno.fsize;
        e->kind = kind;
        e->in_subdir = sub;
    }
    f_closedir(&dir);
}

int img_scan(void)
{
    FRESULT r;
    img_count = 0;
    f_unmount("");
    r = f_mount(&fs, "", 1);
    img_fs_ok = (r == FR_OK);
    if (!img_fs_ok)
    {
        strcpy(img_fs_type, "none");
        printf("[img] no usable filesystem on the card (FatFs error %d)\r\n", (int)r);
        return -1;
    }
    strcpy(img_fs_type, fs_type_name(fs.fs_type));
    scan_dir("/", 0);
    scan_dir("/" IMG_SUBDIR, 1);
    /* stable, case-insensitive sort so list numbers are predictable */
    for (uint32_t i = 1; i < img_count; i++)
    {
        img_entry_t t = img_list[i];
        uint32_t j = i;
        while (j > 0 && ci_cmp(img_list[j - 1].name, t.name) > 0)
        {
            img_list[j] = img_list[j - 1];
            j--;
        }
        img_list[j] = t;
    }
    return (int)img_count;
}

/* ---- mounting ---------------------------------------------------------- */
void img_unmount(void)
{
    tcache_clear();
    if (!img_cur.mounted)
        return;
    printf("[img] unmounted %s\r\n", img_cur.name);
    img_cur.mounted = false;
    img_cur.kind = IMG_KIND_NONE;
    img_cur.n_extents = 0;
    img_cur.blocks = 0;
    img_cur.generation++;
}

/* Build img_cur.ext[] for the file at `path`; *file_sectors gets its length. */
static bool build_extents(const char *path, uint64_t size, uint32_t *file_sectors)
{
    FRESULT r;
    uint32_t total, fsec = 0, n = 0;

    r = f_open(&fil, path, FA_READ);
    if (r != FR_OK)
    {
        printf("[img] open %s failed (FatFs error %d)\r\n", path, (int)r);
        return false;
    }
    if (size == 0)
        size = f_size(&fil);
    fil.cltbl = clmt;
    clmt[0] = sizeof(clmt) / sizeof(clmt[0]);
    r = f_lseek(&fil, CREATE_LINKMAP);
    if (r != FR_OK)
    {
        printf("[img] %s: %s\r\n", path,
               r == FR_NOT_ENOUGH_CORE ? "too fragmented (more than IMG_MAX_EXTENTS runs) - copy it onto a freshly formatted card"
                                       : "could not build the cluster map");
        f_close(&fil);
        return false;
    }
    total = (uint32_t)((size + 511u) / 512u);
    for (const DWORD *p = clmt + 1; p[0] != 0 && fsec < total; p += 2)
    {
        uint32_t cnt = p[0] * fs.csize;
        uint32_t sec = (uint32_t)fs.database + (p[1] - 2u) * fs.csize;
        if (fsec + cnt > total)
            cnt = total - fsec;
        if (n && img_cur.ext[n - 1].sd_sector + img_cur.ext[n - 1].sectors == sec)
            img_cur.ext[n - 1].sectors += cnt;          /* adjacent clusters: one run */
        else
        {
            if (n >= IMG_MAX_EXTENTS)
            {
                printf("[img] %s: too fragmented\r\n", path);
                f_close(&fil);
                return false;
            }
            img_cur.ext[n].file_sector = fsec;
            img_cur.ext[n].sd_sector = sec;
            img_cur.ext[n].sectors = cnt;
            n++;
        }
        fsec += cnt;
    }
    f_close(&fil);
    if (fsec < total)
    {
        printf("[img] %s: cluster chain shorter than the file (%lu of %lu sectors)\r\n", path,
               (unsigned long)fsec, (unsigned long)total);
        return false;
    }
    img_cur.n_extents = n;
    *file_sectors = total;
    return true;
}

/* Recognise the container of a disk image from its headers (the extent map of
 * the file must already be in img_cur).  Sets fmt / data_offset / table
 * fields and the virtual size in sectors; returns false with a message when
 * the format is not one we can serve. */
static bool detect_format(const img_entry_t *e, uint32_t *vsectors)
{
    size_t n = strlen(e->name);
    const uint8_t *p;

    img_cur.fmt = IMG_FMT_RAW;
    img_cur.data_offset = 0;
    *vsectors = img_cur.file_sectors;
    if (n >= 4 && ci_cmp(e->name + n - 3, "vhd") == 0)
    {
        uint64_t data_off;
        uint32_t type;
        p = tcache_get(img_cur.file_sectors - 1u);          /* the footer is the last sector */
        if (!p || memcmp(p, "conectix", 8) != 0)
        {
            printf("[img] %s: no VHD footer\r\n", e->name);
            return false;
        }
        type = be32(p + 60);
        data_off = be64(p + 16);
        *vsectors = (uint32_t)(be64(p + 48) / 512u);        /* current size */
        if (type == 2)                                      /* fixed: raw data, footer behind it */
        {
            img_cur.fmt = IMG_FMT_VHD_FIXED;
            if (*vsectors + 1u > img_cur.file_sectors)
                *vsectors = img_cur.file_sectors - 1u;
            return true;
        }
        if (type != 3)
        {
            printf("[img] %s: VHD type %lu (only fixed and dynamic are supported)\r\n", e->name, (unsigned long)type);
            return false;
        }
        p = tcache_get((uint32_t)(data_off / 512u));         /* dynamic disk header */
        if (!p || memcmp(p, "cxsparse", 8) != 0)
        {
            printf("[img] %s: dynamic VHD header not found\r\n", e->name);
            return false;
        }
        img_cur.fmt            = IMG_FMT_VHD_DYN;
        img_cur.tbl_sector     = (uint32_t)(be64(p + 16) / 512u);
        img_cur.tbl_entries    = be32(p + 28);
        img_cur.unit_sectors   = be32(p + 32) / 512u;
        img_cur.bitmap_sectors = ((img_cur.unit_sectors + 7u) / 8u + 511u) / 512u;
        if (img_cur.unit_sectors == 0 || (img_cur.unit_sectors & (img_cur.unit_sectors - 1u)))
        {
            printf("[img] %s: odd VHD block size\r\n", e->name);
            return false;
        }
        return true;
    }
    if (n >= 5 && ci_cmp(e->name + n - 4, "vmdk") == 0)
    {
        p = tcache_get(0);
        if (!p)
            return false;
        if (le32(p) == 0x564D444Bu)                         /* 'KDMV': a sparse extent */
        {
            uint64_t gd = le64(p + 56);
            if (p[77] | (p[78] << 8))                          /* compressAlgorithm (offset 77, LE16) */
            {
                printf("[img] %s: compressed (streamOptimized) VMDK is not supported\r\n", e->name);
                return false;
            }
            if (gd == 0xFFFFFFFFFFFFFFFFull || le64(p + 20) == 0 || le32(p + 44) == 0)
            {
                printf("[img] %s: VMDK sparse header not usable\r\n", e->name);
                return false;
            }
            img_cur.fmt          = IMG_FMT_VMDK_SPARSE;
            *vsectors            = (uint32_t)le64(p + 12);   /* capacity */
            img_cur.unit_sectors = (uint32_t)le64(p + 20);   /* grain size */
            img_cur.tbl_entries  = le32(p + 44);             /* GTEs per GT */
            img_cur.tbl_sector   = (uint32_t)gd;             /* grain directory */
            return true;
        }
        else                                                /* a text descriptor: look for a FLAT extent */
        {
            char txt[1025], *line, *save;
            const uint8_t *q = tcache_get(1);
            memcpy(txt, p, 512);
            if (q) memcpy(txt + 512, q, 512); else memset(txt + 512, 0, 512);
            txt[1024] = 0;
            for (line = strtok_r(txt, "\r\n", &save); line; line = strtok_r(NULL, "\r\n", &save))
            {
                char *w = line, *fname, *fend;
                unsigned long sz, off = 0;
                if (strncmp(w, "RW ", 3) != 0 && strncmp(w, "RDONLY ", 7) != 0)
                    continue;
                w = strchr(w, ' ') + 1;
                sz = strtoul(w, &w, 10);
                while (*w == ' ') w++;
                if (strncmp(w, "SPARSE", 6) == 0)
                {
                    printf("[img] %s: split/sparse extents in a separate file are not supported\r\n", e->name);
                    return false;
                }
                if (strncmp(w, "FLAT", 4) != 0)
                    continue;
                fname = strchr(w, '"');
                if (!fname) return false;
                fend = strchr(++fname, '"');
                if (!fend) return false;
                *fend = 0;
                off = strtoul(fend + 1, NULL, 10);
                {
                    char path[IMG_NAME_MAX + 8];
                    uint32_t fsecs;
                    path_of(path, sizeof(path), fname, e->in_subdir);
                    tcache_clear();
                    if (!build_extents(path, 0, &fsecs))      /* the data lives in the companion file */
                        return false;
                    img_cur.file_sectors = fsecs;
                    img_cur.fmt          = IMG_FMT_VMDK_FLAT;
                    img_cur.data_offset  = (uint32_t)off;
                    *vsectors            = (uint32_t)sz;
                    if (*vsectors + img_cur.data_offset > fsecs)
                        *vsectors = fsecs - img_cur.data_offset;
                    return true;
                }
            }
            printf("[img] %s: no FLAT extent in the VMDK descriptor\r\n", e->name);
            return false;
        }
    }
    return true;                                            /* .img: raw */
}

static bool mount_entry(const img_entry_t *e, bool want_rw)
{
    char path[IMG_NAME_MAX + 8];
    uint32_t total, vsectors;
    bool raw_like;

    if (!img_fs_ok)
        return false;
    img_unmount();
    path_of(path, sizeof(path), e->name, e->in_subdir);
    if (!build_extents(path, e->size, &total))
        return false;
    img_cur.file_sectors = total;
    tcache_clear();
    if (e->kind == IMG_KIND_ISO)
    {
        img_cur.fmt = IMG_FMT_RAW;
        img_cur.data_offset = 0;
        vsectors = total;
    }
    else if (!detect_format(e, &vsectors))
    {
        img_cur.n_extents = 0;
        return false;
    }
    raw_like = img_cur.fmt == IMG_FMT_RAW || img_cur.fmt == IMG_FMT_VHD_FIXED || img_cur.fmt == IMG_FMT_VMDK_FLAT;

    img_cur.mounted    = true;
    img_cur.kind       = e->kind;
    img_cur.writable   = (e->kind == IMG_KIND_DISK) && want_rw && raw_like;
    strcpy(img_cur.name, e->name);
    img_cur.in_subdir  = e->in_subdir;
    img_cur.block_size = (e->kind == IMG_KIND_ISO) ? 2048u : 512u;
    img_cur.blocks     = (e->kind == IMG_KIND_ISO) ? (uint32_t)(e->size / 2048u) : vsectors;
    img_cur.size       = (uint64_t)img_cur.blocks * img_cur.block_size;
    img_cur.generation++;

    strcpy(last_name, e->name);
    last_subdir = e->in_subdir;
    last_rw = want_rw;
    have_last = true;

    printf("[img] mounted %s: %lu MiB as %s (%s%s), %lu blocks x %lu, %lu extent%s\r\n", e->name,
           (unsigned long)(img_cur.size >> 20), e->kind == IMG_KIND_ISO ? "CD/DVD" : img_fmt_name(),
           img_cur.writable ? "read-write" : "read-only",
           (want_rw && !img_cur.writable && e->kind == IMG_KIND_DISK) ? ", rw refused for this format" : "",
           (unsigned long)img_cur.blocks, (unsigned long)img_cur.block_size,
           (unsigned long)img_cur.n_extents, img_cur.n_extents == 1 ? "" : "s");
    return true;
}

bool img_mount_index(uint32_t index, bool want_rw)
{
    if (index >= img_count)
    {
        printf("[img] no image #%lu\r\n", (unsigned long)(index + 1));
        return false;
    }
    return mount_entry(&img_list[index], want_rw);
}

bool img_mount_name(const char *name, bool want_rw)
{
    uint8_t sub = 0xFF;                                 /* any folder */
    size_t pl = strlen(IMG_SUBDIR), i;
    for (i = 0; i < pl && name[i] && lower(name[i]) == lower(IMG_SUBDIR[i]); i++)
        ;
    if (i == pl && (name[pl] == '/' || name[pl] == '\\'))
    {
        sub = 1;
        name += pl + 1;
    }
    for (i = 0; i < img_count; i++)
        if ((sub == 0xFF || img_list[i].in_subdir == sub) && ci_cmp(img_list[i].name, name) == 0)
            return mount_entry(&img_list[i], want_rw);
    return false;
}

/* ---- ISOMOUNT.TXT as a fixed 512-byte record --------------------------------
 * The key's choice is remembered on the card while a host may have the card's
 * volume mounted (an image swap no longer re-plugs USB).  Writing a file the
 * normal way changes the directory, the FAT and the allocation bitmap under a
 * host that caches all three, so the record has a fixed size and the key path
 * only rewrites its first sector in place: one aligned data-sector write, no
 * metadata change (FatFs writes whole aligned sectors straight to the card and
 * FA_MODIFIED is cleared before the close so no directory entry is updated).
 * The file is created, or re-padded to the record size after a host edited it,
 * only where the host cannot see the card yet: at boot / card init. */
#define IMG_CONFIG_REC   512u
#ifndef FA_MODIFIED
#define FA_MODIFIED      0x40
#endif
static char cfg_rec[IMG_CONFIG_REC];

static void build_record(const char *name_line, bool rw)
{
    static const char note[] = "# written by the board: fixed 512-byte record, updated in place. First line = image (none = nothing), second line rw = writable\r\n";
    size_t n;
    memset(cfg_rec, ' ', sizeof cfg_rec);
    n = (size_t)snprintf(cfg_rec, sizeof cfg_rec, "%s\r\n%s%s", name_line, rw ? "rw\r\n" : "", note);
    if (n < sizeof cfg_rec)
        cfg_rec[n] = ' ';                            /* undo snprintf's terminator: pad with spaces */
    cfg_rec[sizeof cfg_rec - 2] = '\r';
    cfg_rec[sizeof cfg_rec - 1] = '\n';
}

static bool write_record_new(void)
{
    UINT bw = 0;
    FRESULT r = f_open(&fil, "/" IMG_CONFIG_FILE, FA_WRITE | FA_CREATE_ALWAYS);
    if (r == FR_OK)
    {
        r = f_write(&fil, cfg_rec, IMG_CONFIG_REC, &bw);
        f_close(&fil);
    }
    return r == FR_OK && bw == IMG_CONFIG_REC;
}

static bool read_config(char *name, size_t n, bool *rw);

/* boot / card init: make sure the record exists and has the full size */
static void ensure_config_file(void)
{
    char name[IMG_NAME_MAX];
    bool rw = false, have;
    FILINFO fi;
    FRESULT r;
    if (!img_fs_ok)
        return;
    r = f_stat("/" IMG_CONFIG_FILE, &fi);
    if (r == FR_OK && fi.fsize >= IMG_CONFIG_REC)
        return;
    have = r == FR_OK && read_config(name, sizeof name, &rw);
    build_record(have ? name : "none", have && rw);
    if (write_record_new())
        printf("[img] %s %s as a %u-byte record (%s)\r\n", IMG_CONFIG_FILE, r == FR_OK ? "re-padded" : "created",
               (unsigned)IMG_CONFIG_REC, have ? name : "none");
    else
        printf("[img] could not write %s\r\n", IMG_CONFIG_FILE);
}

/* key selection: rewrite the record's first sector in place (see above) */
static void save_choice(const img_entry_t *e)
{
    FRESULT r;
    UINT bw = 0;
    char line[IMG_NAME_MAX + 16];
    if (!img_fs_ok)
        return;
    if (e)
        snprintf(line, sizeof line, "%s%s", e->in_subdir ? IMG_SUBDIR "/" : "", e->name);
    else
        strcpy(line, "none");
    build_record(line, false);
    r = f_open(&fil, "/" IMG_CONFIG_FILE, FA_READ | FA_WRITE);
    if (r != FR_OK)
    {
        printf("[key] %s is missing: choice not saved (the next boot recreates it)\r\n", IMG_CONFIG_FILE);
        return;
    }
    if (f_size(&fil) < IMG_CONFIG_REC)
    {
        f_close(&fil);
        printf("[key] %s was shortened on the host: choice not saved (the next boot re-pads it)\r\n", IMG_CONFIG_FILE);
        return;
    }
    r = f_write(&fil, cfg_rec, IMG_CONFIG_REC, &bw);   /* aligned whole sector: written directly */
    fil.flag &= (BYTE)~FA_MODIFIED;                    /* no directory-entry update on close */
    f_close(&fil);
    if (r == FR_OK && bw == IMG_CONFIG_REC)
        printf("[key] saved to %s in place: %s\r\n", IMG_CONFIG_FILE, line);
    else
        printf("[key] could not write %s (FatFs error %d)\r\n", IMG_CONFIG_FILE, (int)r);
}

bool img_remount_last(void)
{
    if (img_cur.mounted || !have_last || !sd_card.present)
        return false;
    if (img_scan() < 0)
        return false;
    return img_mount_name(last_name, last_rw);
}

/* ISOMOUNT.TXT: first non-comment line = image name, optional second line "rw" */
static bool read_config(char *name, size_t n, bool *rw)
{
    char line[IMG_NAME_MAX + 8];
    int field = 0;
    *rw = false;
    name[0] = 0;
    if (f_open(&fil, "/" IMG_CONFIG_FILE, FA_READ) != FR_OK)
        return false;
    while (f_gets(line, sizeof(line), &fil))
    {
        char *s = line, *e;
        while (*s == ' ' || *s == '\t') s++;
        e = s + strlen(s);
        while (e > s && (e[-1] == '\r' || e[-1] == '\n' || e[-1] == ' ' || e[-1] == '\t')) *--e = 0;
        if (*s == 0 || *s == '#')
            continue;
        if (field == 0)
        {
            strncpy(name, s, n - 1);
            name[n - 1] = 0;
        }
        else if (field == 1)
            *rw = ci_cmp(s, "rw") == 0;
        field++;
    }
    f_close(&fil);
    if (ci_cmp(name, "none") == 0)
        name[0] = 0;                                /* the key's "nothing mounted" */
    return name[0] != 0;
}

bool img_auto_mount(void)
{
    char cfg[IMG_NAME_MAX];
    bool rw;

    if (!sd_card.present)
        return false;
    if (img_scan() < 0)
        return false;
    ensure_config_file();                           /* the host has no view of the card yet: metadata writes are safe */
    if (read_config(cfg, sizeof(cfg), &rw))
    {
        if (img_mount_name(cfg, rw))
            return true;
        printf("[img] %s names \"%s\", which is not on the card\r\n", IMG_CONFIG_FILE, cfg);
    }
    if (have_last && img_mount_name(last_name, last_rw))
        return true;
    if (img_count == 1)
        return img_mount_index(0, false);
    img_print_list();
    if (img_count == 0)
        printf("[img] put .iso / .img files in the card root or in /%s, or name one in %s\r\n", IMG_SUBDIR, IMG_CONFIG_FILE);
    else
        printf("[img] several images and no %s: pick one on the console (l = list, 1-9 = mount)\r\n", IMG_CONFIG_FILE);
    return false;
}

/* ---- mapping ----------------------------------------------------------- */
uint32_t img_map(uint32_t fsec, uint32_t count, uint32_t *sd_sector)
{
    uint32_t total, unit, off, left;
    const uint8_t *p;

    if (!img_cur.mounted || count == 0 || img_cur.n_extents == 0)
        return 0;
    total = img_cur.blocks * (img_cur.block_size / 512u);
    if (fsec >= total)
        return 0;
    if (fsec + count > total)
        count = total - fsec;
    switch (img_cur.fmt)
    {
        case IMG_FMT_VHD_DYN:
        {
            uint32_t blk, ent;
            unit = img_cur.unit_sectors;
            blk = fsec / unit; off = fsec % unit; left = unit - off;
            if (count > left) count = left;
            if (blk >= img_cur.tbl_entries) return 0;
            p = tcache_get(img_cur.tbl_sector + blk / 128u);
            if (!p) return 0;
            ent = be32(p + (blk % 128u) * 4u);
            if (ent == 0xFFFFFFFFu) { *sd_sector = IMG_ZERO_RUN; return count; }
            return file_map(ent + img_cur.bitmap_sectors + off, count, sd_sector);
        }
        case IMG_FMT_VMDK_SPARSE:
        {
            uint32_t g, gt, gte, gtsec, ent;
            unit = img_cur.unit_sectors;
            g = fsec / unit; off = fsec % unit; left = unit - off;
            if (count > left) count = left;
            gt = g / img_cur.tbl_entries; gte = g % img_cur.tbl_entries;
            p = tcache_get(img_cur.tbl_sector + gt / 128u);          /* grain directory */
            if (!p) return 0;
            gtsec = le32(p + (gt % 128u) * 4u);
            if (gtsec == 0) { *sd_sector = IMG_ZERO_RUN; return count; }
            p = tcache_get(gtsec + gte / 128u);                       /* grain table */
            if (!p) return 0;
            ent = le32(p + (gte % 128u) * 4u);
            if (ent <= 1u) { *sd_sector = IMG_ZERO_RUN; return count; }   /* 0 = unallocated, 1 = zero grain */
            return file_map(ent + off, count, sd_sector);
        }
        default:                                                     /* raw, fixed VHD, flat VMDK */
            return file_map(img_cur.data_offset + fsec, count, sd_sector);
    }
}

/* ---- console output ---------------------------------------------------- */
void img_print_list(void)
{
    printf("[img] %lu image%s on the card (%s):\r\n", (unsigned long)img_count, img_count == 1 ? "" : "s", img_fs_type);
    for (uint32_t i = 0; i < img_count; i++)
        printf("[img]  %2lu: %s%s  (%lu MiB, %s)%s\r\n", (unsigned long)(i + 1),
               img_list[i].in_subdir ? IMG_SUBDIR "/" : "", img_list[i].name,
               (unsigned long)(img_list[i].size >> 20), img_list[i].kind == IMG_KIND_ISO ? "iso" : "img",
               (img_cur.mounted && ci_cmp(img_cur.name, img_list[i].name) == 0) ? "  <- mounted" : "");
}

void img_print_status(void)
{
    if (img_cur.mounted)
        printf("[img] mounted: %s, %lu MiB, %s, %s, %lu blocks x %lu, %lu extent%s\r\n", img_cur.name,
               (unsigned long)(img_cur.size >> 20), img_cur.kind == IMG_KIND_ISO ? "CD/DVD" : "disk",
               img_cur.writable ? "read-write" : "read-only", (unsigned long)img_cur.blocks,
               (unsigned long)img_cur.block_size, (unsigned long)img_cur.n_extents, img_cur.n_extents == 1 ? "" : "s");
    else
        printf("[img] no image mounted (filesystem: %s, %lu image%s found)\r\n", img_fs_type,
               (unsigned long)img_count, img_count == 1 ? "" : "s");
}

/* ---- repair aid: delete a directory tree on the card -------------------- */
/* macOS refuses to mount an exFAT volume whose Spotlight index directory is
 * damaged (duplicate names), and its fsck cannot repair that.  The index is
 * disposable -- the host rebuilds it -- so this removes the tree entry by
 * entry with FatFs.  Runs from the console between host commands only. */
#define PURGE_PATH_MAX 300
static FILINFO purge_fno;
static uint32_t purge_removed, purge_failed;

static void purge_rec(char *path, unsigned depth)
{
    DIR dir;
    size_t len = strlen(path);
    if (depth > 8 || f_opendir(&dir, path) != FR_OK)
    {
        purge_failed++;
        printf("[purge] cannot open %s\r\n", path);
        return;
    }
    for (;;)
    {
        FRESULT r = f_readdir(&dir, &purge_fno);
        if (r != FR_OK)
        {
            purge_failed++;
            printf("[purge] readdir %s: %d\r\n", path, (int)r);
            break;
        }
        if (!purge_fno.fname[0])
            break;
        if (len + 1u + strlen(purge_fno.fname) >= PURGE_PATH_MAX)
        {
            purge_failed++;
            continue;
        }
        path[len] = '/';
        strcpy(path + len + 1u, purge_fno.fname);
        if (purge_fno.fattrib & AM_DIR)
            purge_rec(path, depth + 1u);
        r = f_unlink(path);
        if (r == FR_DENIED)
        {
            (void)f_chmod(path, 0, AM_RDO);
            r = f_unlink(path);
        }
        if (r == FR_OK)
            purge_removed++;
        else
        {
            purge_failed++;
            printf("[purge] rm %s: %d\r\n", path, (int)r);
        }
        path[len] = 0;
    }
    f_closedir(&dir);
}

void img_purge_tree(const char *root)
{
    static char path[PURGE_PATH_MAX];
    FRESULT r;
    if (!img_fs_ok && img_scan() < 0)
        return;
    purge_removed = purge_failed = 0;
    strncpy(path, root, sizeof(path) - 1u);
    path[sizeof(path) - 1u] = 0;
    printf("[purge] deleting %s ...\r\n", path);
    purge_rec(path, 0);
    r = f_unlink(path);
    if (r == FR_OK)
        purge_removed++;
    else
    {
        purge_failed++;
        printf("[purge] rm %s: %d\r\n", path, (int)r);
    }
    printf("[purge] done: %lu entries removed, %lu failed\r\n", (unsigned long)purge_removed, (unsigned long)purge_failed);
}

/* ---- user key: cycle through the images in /IMG_SUBDIR ------------------- */
/* Position 0 = nothing mounted, 1..n = the folder's images in list order.  If
 * the folder is empty the root images are cycled instead.  Key-selected disk
 * images are mounted read-only (rw stays a deliberate ISOMOUNT.TXT choice). */
static bool     deferred_pending;
static uint32_t deferred_idx, deferred_pos, deferred_n, deferred_due;

static uint32_t key_candidates(uint32_t *idx, uint32_t max)
{
    uint32_t n = 0, i;
    for (i = 0; i < img_count && n < max; i++)
        if (img_list[i].in_subdir)
            idx[n++] = i;
    if (n == 0)
        for (i = 0; i < img_count && n < max; i++)
            idx[n++] = i;
    return n;
}

void img_key_pos(uint32_t *pos, uint32_t *n)
{
    uint32_t idx[IMG_MAX], cnt = key_candidates(idx, IMG_MAX), i;
    *pos = 0;
    *n = cnt;
    if (!img_cur.mounted)
        return;
    for (i = 0; i < cnt; i++)
        if (img_list[idx[i]].in_subdir == img_cur.in_subdir && ci_cmp(img_list[idx[i]].name, img_cur.name) == 0)
        {
            *pos = i + 1;
            return;
        }
}

bool img_cycle_next(void)
{
    uint32_t idx[IMG_MAX], cnt, pos, next;
    if (!sd_card.present)
    {
        printf("[key] no card\r\n");
        return false;
    }
    /* Rescan every time so files added to or removed from the folder (while
     * the card sits in the host as a drive) are picked up; the position is
     * recomputed against the fresh list.  A failed rescan changes nothing. */
    if (img_scan() < 0)
    {
        printf("[key] card rescan failed: keeping the old list\r\n");
        return false;
    }
    if (img_count == 0)
    {
        printf("[key] no images on the card\r\n");
        return false;
    }
    cnt = key_candidates(idx, IMG_MAX);
    if (cnt == 0)
    {
        printf("[key] no images in /%s or the root\r\n", IMG_SUBDIR);
        return false;
    }
    img_key_pos(&pos, &cnt);
    next = (pos + 1u) % (cnt + 1u);
    if (img_cur.mounted)
        img_unmount();
    deferred_pending = false;
    if (next == 0)
    {
        have_last = false;                          /* a mode switch must not bring the old image back */
        printf("[key] 0/%lu: no image mounted\r\n", (unsigned long)cnt);
        save_choice(NULL);
        return false;
    }
    /* The host must see the LUN empty for a moment (it drops the old volume on
     * NOT READY), then the new medium arrives as a not-ready-to-ready change. */
    deferred_idx = idx[next - 1u];
    deferred_pos = next;
    deferred_n = cnt;
    deferred_due = systime_us() + (uint32_t)KEY_SWAP_GAP_MS * 1000u;
    deferred_pending = true;
    printf("[key] %lu/%lu: %s in %u ms\r\n", (unsigned long)next, (unsigned long)cnt, img_list[deferred_idx].name, (unsigned)KEY_SWAP_GAP_MS);
    return true;
}

bool img_deferred_waiting(void)
{
    return deferred_pending;
}

bool img_deferred_due(void)
{
    return deferred_pending && (int32_t)(systime_us() - deferred_due) >= 0;
}

void img_deferred_mount(void)
{
    if (!deferred_pending)
        return;
    deferred_pending = false;
    if (deferred_idx >= img_count)
        return;
    if (img_mount_index(deferred_idx, false))
    {
        printf("[key] %lu/%lu: %s mounted\r\n", (unsigned long)deferred_pos, (unsigned long)deferred_n, img_list[deferred_idx].name);
        save_choice(&img_list[deferred_idx]);
    }
    else
        printf("[key] %lu/%lu: mount of %s failed\r\n", (unsigned long)deferred_pos, (unsigned long)deferred_n, img_list[deferred_idx].name);
}

/* ---- console 'C': CRC-32 of the whole virtual disk through the mapping layer ---- */
/* Reads every virtual sector the way the host would (zero runs included) and
 * prints the zlib-compatible CRC-32, so a VHD/VMDK can be checked bit-exactly
 * against the raw image it was made from (python3: zlib.crc32(open(f,'rb').read())). */
static uint32_t crc_table[256];

static uint32_t crc32_update(uint32_t crc, const uint8_t *p, uint32_t n)
{
    if (crc_table[1] == 0)
        for (uint32_t i = 0; i < 256; i++)
        {
            uint32_t c = i;
            for (int k = 0; k < 8; k++)
                c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            crc_table[i] = c;
        }
    while (n--)
        crc = crc_table[(crc ^ *p++) & 0xFFu] ^ (crc >> 8);
    return crc;
}

void img_crc_check(void)
{
    static __attribute__((aligned(16))) uint8_t buf[16384];
    uint32_t total, fsec = 0, crc = 0xFFFFFFFFu, zero = 0, t0 = systime_us();
    if (!img_cur.mounted)
    {
        printf("[crc] nothing mounted\r\n");
        return;
    }
    total = img_cur.blocks * (img_cur.block_size / 512u);
    printf("[crc] reading %s (%s), %lu sectors ...\r\n", img_cur.name, img_fmt_name(), (unsigned long)total);
    while (fsec < total)
    {
        uint32_t sd, run = img_map(fsec, sizeof(buf) / 512u, &sd);
        if (run == 0)
        {
            printf("[crc] map failed at virtual sector %lu\r\n", (unsigned long)fsec);
            return;
        }
        if (sd == IMG_ZERO_RUN)
        {
            memset(buf, 0, run * 512u);
            zero += run;
        }
        else if (sd_read_blocks(buf, sd, run) != SD_OK)
        {
            printf("[crc] read failed at virtual sector %lu\r\n", (unsigned long)fsec);
            return;
        }
        crc = crc32_update(crc, buf, run * 512u);
        fsec += run;
    }
    printf("[crc] %s: crc32 %08lX over %lu sectors (%lu unallocated), %lu ms\r\n", img_cur.name,
           (unsigned long)(crc ^ 0xFFFFFFFFu), (unsigned long)total, (unsigned long)zero,
           (unsigned long)(systime_elapsed_us(t0) / 1000u));
}

