/*
 * fat.c - see fat.h.  512-byte sectors only.
 */
#include "fat.h"
#include "uart.h"
#include "libc.h"

static uint8_t sec[512];
static uint32_t sec_lba = 0xFFFFFFFFu;

static int rd(struct fat_fs *fs, uint32_t lba)
{
    if (lba == sec_lba) return 0;
    int rc = fs->read(fs->ctx, lba, 1, sec);
    sec_lba = rc == 0 ? lba : 0xFFFFFFFFu;
    return rc;
}

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t le32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

static int is_boot_sector(const uint8_t *b)
{
    uint16_t bps = le16(b + 11);
    return (b[0] == 0xEB || b[0] == 0xE9) && bps == 512 && b[13] && le16(b + 14) && b[16];
}

int fat_mount(struct fat_fs *fs, fat_read_fn read, void *ctx)
{
    memset(fs, 0, sizeof *fs);
    fs->read = read;
    fs->ctx = ctx;
    sec_lba = 0xFFFFFFFFu;
    if (rd(fs, 0)) return -1;
    if (sec[510] != 0x55 || sec[511] != 0xAA) return -2;
    if (!memcmp(sec + 3, "EXFAT   ", 8)) return -3;          /* exFAT is not supported */
    if (!is_boot_sector(sec)) {                              /* MBR: first FAT partition */
        for (int i = 0; i < 4; i++) {
            const uint8_t *e = sec + 446 + 16 * i;
            uint8_t t = e[4];
            if (t == 0x01 || t == 0x04 || t == 0x06 || t == 0x0B || t == 0x0C || t == 0x0E) {
                fs->part_lba = le32(e + 8);
                break;
            }
            if (t == 0x07) {                                 /* NTFS or exFAT */
                fs->part_lba = le32(e + 8);
                break;
            }
        }
        if (!fs->part_lba) return -4;
        if (rd(fs, fs->part_lba)) return -1;
        if (!memcmp(sec + 3, "EXFAT   ", 8)) return -3;
        if (!is_boot_sector(sec)) return -5;
    }
    uint32_t reserved = le16(sec + 14), nfats = sec[16], root_ent = le16(sec + 17);
    uint32_t total = le16(sec + 19) ? le16(sec + 19) : le32(sec + 32);
    uint32_t fatsz = le16(sec + 22) ? le16(sec + 22) : le32(sec + 36);
    fs->sectors_per_cluster = sec[13];
    fs->fat_lba = fs->part_lba + reserved;
    fs->root_sectors = (root_ent * 32 + 511) / 512;
    fs->root_lba = fs->fat_lba + nfats * fatsz;
    fs->data_lba = fs->root_lba + fs->root_sectors;
    fs->clusters = (total - (fs->data_lba - fs->part_lba)) / fs->sectors_per_cluster;
    fs->type = fs->clusters < 4085 ? 12 : fs->clusters < 65525 ? 16 : 32;
    const uint8_t *lab = fs->type == 32 ? sec + 71 : sec + 43;
    memcpy(fs->label, lab, 11);
    fs->label[11] = 0;
    for (int i = 10; i >= 0 && fs->label[i] == ' '; i--) fs->label[i] = 0;
    if (fs->type == 32) fs->root_cluster = le32(sec + 44);
    return 0;
}

static uint32_t next_cluster(struct fat_fs *fs, uint32_t c)
{
    if (fs->type == 32) {
        if (rd(fs, fs->fat_lba + c * 4 / 512)) return 0x0FFFFFFFu;
        return le32(sec + (c * 4) % 512) & 0x0FFFFFFFu;
    }
    if (fs->type == 16) {
        if (rd(fs, fs->fat_lba + c * 2 / 512)) return 0xFFFFu;
        uint32_t v = le16(sec + (c * 2) % 512);
        return v >= 0xFFF8u ? 0x0FFFFFFFu : v;
    }
    uint32_t off = c + c / 2;                                /* FAT12: entry may straddle sectors */
    if (rd(fs, fs->fat_lba + off / 512)) return 0x0FFFFFFFu;
    uint32_t lo = sec[off % 512];
    if (rd(fs, fs->fat_lba + (off + 1) / 512)) return 0x0FFFFFFFu;
    uint32_t v = lo | (sec[(off + 1) % 512] << 8);
    v = (c & 1) ? v >> 4 : v & 0xFFF;
    return v >= 0xFF8u ? 0x0FFFFFFFu : v;
}

static int end_of_chain(struct fat_fs *fs, uint32_t c)
{
    return c < 2 || c >= 0x0FFFFFF8u || c >= fs->clusters + 2;
}

static uint32_t cluster_lba(struct fat_fs *fs, uint32_t c)
{
    return fs->data_lba + (c - 2) * fs->sectors_per_cluster;
}

int fat_list(struct fat_fs *fs, uint32_t dir_cluster,
             int (*cb)(const struct fat_dirent *e, void *arg), void *arg)
{
    struct fat_dirent de;
    char lfn[64];
    int lfn_ok = 0;
    uint32_t cl = dir_cluster ? dir_cluster : (fs->type == 32 ? fs->root_cluster : 0);
    uint32_t lba = cl ? cluster_lba(fs, cl) : fs->root_lba;
    uint32_t left = cl ? fs->sectors_per_cluster : fs->root_sectors;
    memset(lfn, 0, sizeof lfn);

    for (;;) {
        if (!left) {
            if (!cl) return 0;                               /* end of fixed FAT12/16 root */
            cl = next_cluster(fs, cl);
            if (end_of_chain(fs, cl)) return 0;
            lba = cluster_lba(fs, cl);
            left = fs->sectors_per_cluster;
        }
        if (rd(fs, lba)) return -1;
        for (int i = 0; i < 512; i += 32) {
            const uint8_t *e = sec + i;
            if (e[0] == 0x00) return 0;                      /* end of directory */
            if (e[0] == 0xE5) { lfn_ok = 0; continue; }
            if (e[11] == 0x0F) {                             /* long-name piece, 13 UTF-16 chars */
                int seq = (e[0] & 0x1F) - 1;
                if (e[0] & 0x40) { memset(lfn, 0, sizeof lfn); lfn_ok = 1; }
                static const uint8_t pos[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
                for (int k = 0; k < 13; k++) {
                    int idx = seq * 13 + k;
                    uint16_t ch = le16(e + pos[k]);
                    if (idx < (int)sizeof lfn - 1 && ch != 0xFFFF && ch != 0)
                        lfn[idx] = ch < 128 ? (char)ch : '?';
                }
                continue;
            }
            if (e[11] & 0x08) { lfn_ok = 0; continue; }      /* volume label */
            memset(&de, 0, sizeof de);
            if (lfn_ok && lfn[0]) {
                memcpy(de.name, lfn, sizeof de.name - 1);
            } else {
                int n = 0;
                for (int k = 0; k < 8 && e[k] != ' '; k++) de.name[n++] = (char)e[k];
                if (e[8] != ' ') {
                    de.name[n++] = '.';
                    for (int k = 8; k < 11 && e[k] != ' '; k++) de.name[n++] = (char)e[k];
                }
            }
            lfn_ok = 0;
            de.attr = e[11];
            de.size = le32(e + 28);
            de.cluster = ((uint32_t)le16(e + 20) << 16) | le16(e + 26);
            if (cb && cb(&de, arg)) return 1;
            /* the callback may have caused other sector reads: reload ours */
            if (rd(fs, lba)) return -1;
        }
        lba++;
        left--;
    }
}

/* Whole sectors go straight into the caller's buffer, and runs of
 * consecutive clusters are fetched with one read (up to 64 KB), so a file
 * costs one USB command per fragment instead of one per sector. */
int fat_read_file(struct fat_fs *fs, const struct fat_dirent *e, void *buf, uint32_t max)
{
    uint8_t *p = buf;
    uint32_t want = e->size < max ? e->size : max, got = 0, cl = e->cluster;
    const uint32_t csize = fs->sectors_per_cluster * 512u;
    while (got < want && !end_of_chain(fs, cl)) {
        uint32_t first = cl, run = 1, next = next_cluster(fs, cl);
        while (!end_of_chain(fs, next) && next == first + run && (run + 1) * csize <= 65536u &&
               got + (run + 1) * csize <= want) {
            run++;
            next = next_cluster(fs, next);
        }
        uint32_t whole = (want - got) / 512u;                       /* full sectors still wanted */
        uint32_t secs = run * fs->sectors_per_cluster;
        if (whole > secs) whole = secs;
        if (whole) {
            if (fs->read(fs->ctx, cluster_lba(fs, first), whole, p + got)) return -1;
            got += whole * 512u;
        }
        if (whole < secs && got < want) {                           /* partial last sector */
            if (rd(fs, cluster_lba(fs, first) + whole)) return -1;
            uint32_t n = want - got;
            memcpy(p + got, sec, n);
            got += n;
        }
        cl = next;
    }
    return (int)got;
}
