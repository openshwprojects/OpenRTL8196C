/*
 * ntfs.c - read-only NTFS, see ntfs.h
 *
 * On-disk layout used here (all little-endian):
 *  boot sector  0x0B bytes/sector, 0x0D sectors/cluster, 0x28 total sectors,
 *               0x30 $MFT cluster, 0x40 clusters (or -log2 bytes) per MFT record
 *  MFT record   "FILE", 0x04 update sequence offset/count, 0x14 first attribute,
 *               0x16 flags (1 in use, 2 directory), 0x18 bytes in use
 *  attribute    0x00 type, 0x04 length, 0x08 non-resident, 0x09 name length,
 *               0x0A name offset, 0x0C flags; resident: 0x10 value length,
 *               0x14 value offset; non-resident: 0x10 lowest VCN, 0x20 runs
 *               offset, 0x30 data size, 0x38 initialised size
 *  index entry  0x00 file reference, 0x08 length, 0x0A key length, 0x0C flags
 *               (1 sub-node, 2 last), 0x10 $FILE_NAME key: 0x30 data size,
 *               0x38 attributes, 0x40 name length, 0x41 namespace, 0x42 name
 *
 * No libgcc: 64-bit values only use add/compare/constant shifts; variable
 * shifts go through shl64/shr64.
 */
#include "ntfs.h"
#include "libc.h"

#define AT_ATTRIBUTE_LIST     0x20u
#define AT_FILE_NAME          0x30u
#define AT_VOLUME_NAME        0x60u
#define AT_DATA               0x80u
#define AT_INDEX_ROOT         0x90u
#define AT_INDEX_ALLOCATION   0xA0u
#define AT_BITMAP             0xB0u
#define AT_END                0xFFFFFFFFu

static uint8_t sec[512] __attribute__((aligned(32)));
static uint8_t orec[NTFS_MAX_REC] __attribute__((aligned(32)));   /* ntfs_open: base record */
static uint8_t oext[NTFS_MAX_REC] __attribute__((aligned(32)));   /* ntfs_open: extension record */
static uint8_t alist[4096] __attribute__((aligned(32)));          /* $ATTRIBUTE_LIST value */
static uint8_t lrec[NTFS_MAX_REC] __attribute__((aligned(32)));   /* ntfs_list: directory record */
static uint8_t lidx[65536] __attribute__((aligned(32)));          /* ntfs_list: one index block */
static uint8_t lbmp[8192] __attribute__((aligned(32)));           /* ntfs_list: index block bitmap */
static struct ntfs_run lruns[NTFS_MAX_RUNS], truns[64];

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t le32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint64_t le64(const uint8_t *p) { return ((uint64_t)le32(p + 4) << 32) | le32(p); }

static uint64_t shl64(uint64_t v, uint32_t s)
{
    uint32_t hi = (uint32_t)(v >> 32), lo = (uint32_t)v;
    if (!s) return v;
    if (s >= 32) return (uint64_t)(lo << (s - 32)) << 32;
    return ((uint64_t)((hi << s) | (lo >> (32 - s))) << 32) | (uint32_t)(lo << s);
}

static uint64_t shr64(uint64_t v, uint32_t s)
{
    uint32_t hi = (uint32_t)(v >> 32), lo = (uint32_t)v;
    if (!s) return v;
    if (s >= 32) return hi >> (s - 32);
    return ((uint64_t)(hi >> s) << 32) | ((lo >> s) | (hi << (32 - s)));
}

static int log2u(uint32_t v)
{
    int n = 0;
    if (!v || (v & (v - 1))) return -1;
    while (v > 1) { v >>= 1; n++; }
    return n;
}

const char *ntfs_strerror(int err)
{
    switch (err) {
    case NTFS_E_IO: return "read error";
    case NTFS_E_NOFS: return "no NTFS volume";
    case NTFS_E_BADREC: return "damaged MFT record";
    case NTFS_E_NOTDIR: return "not a directory";
    case NTFS_E_ISDIR: return "is a directory";
    case NTFS_E_NODATA: return "no data attribute";
    case NTFS_E_COMPRESSED: return "compressed file (not supported)";
    case NTFS_E_ENCRYPTED: return "encrypted file (not supported)";
    case NTFS_E_FRAGMENTED: return "too many fragments";
    case NTFS_E_UNSUPPORTED: return "unsupported layout";
    default: return "error";
    }
}

/* volume-relative sectors */
static int rd(struct ntfs_fs *fs, uint32_t lba, uint32_t n, void *buf)
{
    return fs->read(fs->ctx, fs->part_lba + lba, n, buf) < 0 ? NTFS_E_IO : 0;
}

/* len bytes at byte offset off of the stream described by runs; holes read as zeros */
static int read_runs(struct ntfs_fs *fs, const struct ntfs_run *r, uint32_t nr, uint64_t off, uint8_t *buf,
                     uint32_t len)
{
    const uint32_t cs = fs->cluster_shift, cmask = (1u << cs) - 1u;
    uint32_t i = 0;
    while (len) {
        uint32_t vcn = (uint32_t)shr64(off, cs), in_cl = (uint32_t)off & cmask;
        while (i < nr && vcn >= r[i].vcn + r[i].len) i++;
        if (i == nr || vcn < r[i].vcn) {                     /* not sorted by the caller: search again */
            for (i = 0; i < nr && !(vcn >= r[i].vcn && vcn < r[i].vcn + r[i].len); i++) ;
            if (i == nr) return NTFS_E_BADREC;               /* beyond the allocation */
        }
        uint64_t left = shl64(r[i].vcn + r[i].len - vcn, cs) - in_cl;
        uint32_t n = (uint64_t)len < left ? len : (uint32_t)left;
        if (r[i].lcn == NTFS_SPARSE) {
            memset(buf, 0, n);
        } else {
            uint64_t pos = shl64(r[i].lcn + (vcn - r[i].vcn), cs) + in_cl;
            uint32_t s = (uint32_t)(pos >> 9), so = (uint32_t)pos & 511u;
            if (so == 0 && n >= 512) {
                n &= ~511u;
                if (n > 65536u) n = 65536u;
                if (rd(fs, s, n >> 9, buf) < 0) return NTFS_E_IO;
            } else {
                if (rd(fs, s, 1, sec) < 0) return NTFS_E_IO;
                if (n > 512u - so) n = 512u - so;
                memcpy(buf, sec + so, n);
            }
        }
        off += n; buf += n; len -= n;
    }
    return 0;
}

/* undo the update sequence: the last two bytes of every 512-byte block */
static int fixup(uint8_t *b, uint32_t size, const char *magic)
{
    if (memcmp(b, magic, 4) != 0) return NTFS_E_BADREC;
    uint32_t uo = le16(b + 4), un = le16(b + 6);
    if (un < 2 || (un - 1) * 512u != size || uo + un * 2u > size) return NTFS_E_BADREC;
    for (uint32_t i = 1; i < un; i++) {
        uint8_t *e = b + i * 512u - 2;
        if (e[0] != b[uo] || e[1] != b[uo + 1]) return NTFS_E_BADREC;   /* torn write */
        e[0] = b[uo + 2 * i];
        e[1] = b[uo + 2 * i + 1];
    }
    return 0;
}

static int load_rec(struct ntfs_fs *fs, uint32_t n, uint8_t *b)
{
    uint32_t size = 1u << fs->rec_shift;
    int rc = read_runs(fs, fs->mft_runs, fs->mft_nruns, shl64(n, fs->rec_shift), b, size);
    if (rc < 0) return rc;
    if (fixup(b, size, "FILE") < 0) return NTFS_E_BADREC;
    if (!(le16(b + 0x16) & 1)) return NTFS_E_BADREC;          /* record not in use */
    return 0;
}

/* attribute of the given type and name (NULL = unnamed), searching after prev */
static uint8_t *find_attr(struct ntfs_fs *fs, uint8_t *rec, uint32_t type, const char *name, uint8_t *prev)
{
    uint32_t size = 1u << fs->rec_shift, used = le32(rec + 0x18);
    if (used > size) used = size;
    uint32_t off = prev ? (uint32_t)(prev - rec) + le32(prev + 4) : le16(rec + 0x14);
    while (off + 16 <= used) {
        uint8_t *a = rec + off;
        uint32_t t = le32(a), len = le32(a + 4);
        if (t == AT_END || len < 16 || off + len > used) break;
        if (t == type) {
            uint32_t nl = a[9], no = le16(a + 0x0A);
            if (!name) {
                if (nl == 0) return a;
            } else if (nl == strlen(name) && no + nl * 2 <= len) {
                uint32_t k = 0;
                while (k < nl && le16(a + no + 2 * k) == (uint8_t)name[k]) k++;
                if (k == nl) return a;
            }
        }
        off += len;
    }
    return 0;
}

static const uint8_t *res_value(const uint8_t *a, uint32_t *len)
{
    *len = le32(a + 0x10);
    if (le16(a + 0x14) + *len > le32(a + 4)) *len = 0;
    return a + le16(a + 0x14);
}

/* appends the data runs of a non-resident attribute */
static int decode_runs(const uint8_t *a, struct ntfs_run *runs, uint32_t *nr, uint32_t max)
{
    uint32_t vcn = (uint32_t)le64(a + 0x10);
    const uint8_t *p = a + le16(a + 0x20), *end = a + le32(a + 4);
    int64_t lcn = 0;
    while (p < end && *p) {
        uint32_t L = *p & 15u, O = *p >> 4;
        p++;
        if (!L || L > 8 || O > 8 || p + L + O > end) return NTFS_E_BADREC;
        uint64_t len = 0;
        for (uint32_t i = L; i-- > 0;) len = (len << 8) | p[i];
        p += L;
        uint32_t this_lcn = NTFS_SPARSE;
        if (O) {
            uint64_t d = (p[O - 1] & 0x80) ? ~(uint64_t)0 : 0;   /* sign extension */
            for (uint32_t i = O; i-- > 0;) d = (d << 8) | p[i];
            p += O;
            lcn += (int64_t)d;
            if (lcn < 0 || lcn >= 0xFFFFFFFFll) return NTFS_E_BADREC;
            this_lcn = (uint32_t)lcn;
        }
        if (len == 0 || len > 0xFFFFFFFFull) return NTFS_E_BADREC;
        if (*nr >= max) return NTFS_E_FRAGMENTED;
        runs[*nr].vcn = vcn;
        runs[*nr].lcn = this_lcn;
        runs[*nr].len = (uint32_t)len;
        (*nr)++;
        vcn += (uint32_t)len;
    }
    return 0;
}

/* UTF-16LE -> UTF-8, surrogate pairs included, cut at max-1 bytes */
static void utf16_to_utf8(const uint8_t *s, uint32_t n, char *out, uint32_t max)
{
    uint32_t o = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t c = le16(s + 2 * i);
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < n) {
            uint32_t d = le16(s + 2 * i + 2);
            if (d >= 0xDC00 && d < 0xE000) { c = 0x10000u + ((c - 0xD800u) << 10) + (d - 0xDC00u); i++; }
        }
        char t[4];
        uint32_t k;
        if (c < 0x80) { t[0] = (char)c; k = 1; }
        else if (c < 0x800) { t[0] = (char)(0xC0 | (c >> 6)); t[1] = (char)(0x80 | (c & 63)); k = 2; }
        else if (c < 0x10000) {
            t[0] = (char)(0xE0 | (c >> 12)); t[1] = (char)(0x80 | ((c >> 6) & 63)); t[2] = (char)(0x80 | (c & 63));
            k = 3;
        } else {
            t[0] = (char)(0xF0 | (c >> 18)); t[1] = (char)(0x80 | ((c >> 12) & 63));
            t[2] = (char)(0x80 | ((c >> 6) & 63)); t[3] = (char)(0x80 | (c & 63));
            k = 4;
        }
        if (o + k >= max) break;
        memcpy(out + o, t, k);
        o += k;
    }
    out[o] = 0;
}

/* ------------------------------------------------------------------ mount */
static int try_volume(struct ntfs_fs *fs, uint32_t lba)
{
    fs->part_lba = lba;
    if (rd(fs, 0, 1, sec) < 0) return NTFS_E_IO;
    if (memcmp(sec + 3, "NTFS    ", 8) != 0 || sec[510] != 0x55 || sec[511] != 0xAA) return NTFS_E_NOFS;
    if (le16(sec + 0x0B) != 512) return NTFS_E_UNSUPPORTED;          /* 4Kn sectors */
    uint32_t spc = sec[0x0D];
    int cs = spc > 0x80 ? (int)(256 - spc) : log2u(spc * 512u);       /* >0x80: 2^(256-x) bytes */
    if (cs < 9 || cs > 21) return NTFS_E_NOFS;
    fs->cluster_shift = (uint32_t)cs;
    int8_t cpr = (int8_t)sec[0x40];
    int rs = cpr > 0 ? log2u((uint32_t)cpr) + cs : -cpr;
    if (rs < 9 || (1u << rs) > NTFS_MAX_REC) return NTFS_E_UNSUPPORTED;
    fs->rec_shift = (uint32_t)rs;
    uint64_t total = le64(sec + 0x28), mft = le64(sec + 0x30);
    fs->total_clusters = (uint32_t)shr64(total, (uint32_t)cs - 9);
    if (mft >= fs->total_clusters) return NTFS_E_NOFS;
    fs->mft_lcn = (uint32_t)mft;
    fs->serial_lo = le32(sec + 0x48);
    fs->serial_hi = le32(sec + 0x4C);

    /* bootstrap: the first records of $MFT are contiguous at mft_lcn */
    fs->mft_runs[0].vcn = 0;
    fs->mft_runs[0].lcn = fs->mft_lcn;
    fs->mft_runs[0].len = (uint32_t)(shr64(shl64(16, fs->rec_shift), fs->cluster_shift) + 1);
    fs->mft_nruns = 1;
    int rc = load_rec(fs, 0, orec);
    if (rc < 0) return rc;
    uint8_t *d = find_attr(fs, orec, AT_DATA, 0, 0);
    if (!d || !d[8]) return NTFS_E_BADREC;
    uint32_t nr = 0;
    rc = decode_runs(d, truns, &nr, 64);
    if (rc < 0) return rc;
    memcpy(fs->mft_runs, truns, nr * sizeof truns[0]);
    fs->mft_nruns = nr;

    fs->label[0] = 0;
    if (load_rec(fs, 3, orec) == 0) {                                 /* $Volume */
        uint8_t *v = find_attr(fs, orec, AT_VOLUME_NAME, 0, 0);
        if (v && !v[8]) {
            uint32_t vl;
            const uint8_t *s = res_value(v, &vl);
            utf16_to_utf8(s, vl / 2, fs->label, sizeof fs->label);
        }
    }
    return 0;
}

int ntfs_mount(struct ntfs_fs *fs, ntfs_read_fn read, void *ctx)
{
    memset(fs, 0, sizeof *fs);
    fs->read = read;
    fs->ctx = ctx;
    int rc = try_volume(fs, 0);                                       /* partitionless */
    if (rc != NTFS_E_NOFS) return rc;
    if (read(ctx, 0, 1, sec) < 0) return NTFS_E_IO;
    if (sec[510] != 0x55 || sec[511] != 0xAA) return NTFS_E_NOFS;
    uint32_t start[4];
    uint8_t type[4];
    for (int i = 0; i < 4; i++) { type[i] = sec[446 + 16 * i + 4]; start[i] = le32(sec + 446 + 16 * i + 8); }
    for (int i = 0; i < 4; i++) {
        if (type[i] == 0xEE) {                                        /* GPT */
            if (read(ctx, 1, 1, sec) < 0) return NTFS_E_IO;
            if (memcmp(sec, "EFI PART", 8) != 0) return NTFS_E_NOFS;
            uint32_t ent = le32(sec + 72), n = le32(sec + 80), esz = le32(sec + 84);
            if (esz < 128 || esz > 512 || (512 % esz)) return NTFS_E_UNSUPPORTED;
            for (uint32_t k = 0; k < n && k < 128; k++) {
                static uint8_t es[512];
                if (read(ctx, ent + k * esz / 512, 1, es) < 0) return NTFS_E_IO;
                const uint8_t *e = es + (k * esz) % 512;
                uint32_t first = le32(e + 32);
                if (!first) continue;
                rc = try_volume(fs, first);
                if (rc != NTFS_E_NOFS) return rc;
            }
            return NTFS_E_NOFS;
        }
        if (type[i] == 0x07) {
            rc = try_volume(fs, start[i]);
            if (rc != NTFS_E_NOFS) return rc;
        }
    }
    return NTFS_E_NOFS;
}

/* ------------------------------------------------------------ directories */
struct list_ctx {
    uint32_t dir;
    int (*cb)(const struct ntfs_dirent *e, void *arg);
    void *arg;
    int stop;
};

static int index_entries(const uint8_t *hdr, uint32_t avail, struct list_ctx *c)
{
    uint32_t eo = le32(hdr), il = le32(hdr + 4);
    if (il > avail || eo >= il) return NTFS_E_BADREC;
    const uint8_t *e = hdr + eo, *end = hdr + il;
    static struct ntfs_dirent de;
    while (e + 16 <= end && !c->stop) {
        uint32_t elen = le16(e + 8), klen = le16(e + 10), fl = le16(e + 12);
        if (fl & 2) break;                                            /* last entry: no key */
        if (elen < 16 || e + elen > end) return NTFS_E_BADREC;
        if (klen >= 0x42 && 16 + klen <= elen) {
            const uint8_t *k = e + 16;
            uint32_t nl = k[0x40], ns = k[0x41], mft = le32(e);
            if (0x42 + nl * 2 <= klen && ns != 2 && mft >= 16 && mft != c->dir) {   /* 2 = DOS 8.3 alias */
                utf16_to_utf8(k + 0x42, nl, de.name, sizeof de.name);
                de.mft = mft;
                de.size = le64(k + 0x30);
                de.attr = le32(k + 0x38);
                if (c->cb(&de, c->arg)) c->stop = 1;
            }
        }
        e += elen;
    }
    return 0;
}

int ntfs_list(struct ntfs_fs *fs, uint32_t dir_mft, int (*cb)(const struct ntfs_dirent *e, void *arg), void *arg)
{
    int rc = load_rec(fs, dir_mft, lrec);
    if (rc < 0) return rc;
    if (!(le16(lrec + 0x16) & 2)) return NTFS_E_NOTDIR;
    struct list_ctx c = { dir_mft, cb, arg, 0 };

    uint8_t *root = find_attr(fs, lrec, AT_INDEX_ROOT, "$I30", 0);
    if (!root || root[8]) {
        return find_attr(fs, lrec, AT_ATTRIBUTE_LIST, 0, 0) ? NTFS_E_UNSUPPORTED : NTFS_E_BADREC;
    }
    uint32_t vl;
    const uint8_t *v = res_value(root, &vl);
    if (vl < 0x20) return NTFS_E_BADREC;
    uint32_t idx_size = le32(v + 8);
    rc = index_entries(v + 0x10, vl - 0x10, &c);
    if (rc < 0 || c.stop) return rc;

    uint8_t *alloc = find_attr(fs, lrec, AT_INDEX_ALLOCATION, "$I30", 0);
    if (!alloc) return 0;                                             /* small directory */
    if (!alloc[8] || idx_size < 512 || idx_size > sizeof lidx || log2u(idx_size) < 0) return NTFS_E_UNSUPPORTED;
    uint32_t nr = 0;
    rc = decode_runs(alloc, lruns, &nr, NTFS_MAX_RUNS);
    if (rc < 0) return rc;
    uint32_t nblocks = (uint32_t)shr64(le64(alloc + 0x30), (uint32_t)log2u(idx_size));

    /* in-use bitmap of the index blocks: stale blocks must not be listed */
    uint8_t *bm = find_attr(fs, lrec, AT_BITMAP, "$I30", 0);
    uint32_t bml = 0;
    if (bm && !bm[8]) {
        const uint8_t *bv = res_value(bm, &bml);
        if (bml > sizeof lbmp) bml = sizeof lbmp;
        memcpy(lbmp, bv, bml);
    } else if (bm) {
        uint32_t bnr = 0;
        if (decode_runs(bm, truns, &bnr, 64) < 0) return NTFS_E_BADREC;
        uint64_t sz = le64(bm + 0x30);
        bml = sz > sizeof lbmp ? sizeof lbmp : (uint32_t)sz;
        if (read_runs(fs, truns, bnr, 0, lbmp, bml) < 0) return NTFS_E_IO;
    }
    for (uint32_t b = 0; b < nblocks && !c.stop; b++) {
        if (b / 8 >= bml || !(lbmp[b / 8] & (1u << (b & 7)))) continue;
        rc = read_runs(fs, lruns, nr, shl64(b, (uint32_t)log2u(idx_size)), lidx, idx_size);
        if (rc < 0) return rc;
        if (fixup(lidx, idx_size, "INDX") < 0) return NTFS_E_BADREC;
        rc = index_entries(lidx + 0x18, idx_size - 0x18, &c);
        if (rc < 0) return rc;
    }
    return 0;
}

/* ------------------------------------------------------------------ files */
static int take_data(struct ntfs_fs *fs, const uint8_t *a, struct ntfs_file *f, int sizes)
{
    uint32_t fl = le16(a + 0x0C);
    if (fl & 0x00FFu) return NTFS_E_COMPRESSED;
    if (fl & 0x4000u) return NTFS_E_ENCRYPTED;
    if (!a[8]) {
        uint32_t vl;
        const uint8_t *v = res_value(a, &vl);
        if (vl > sizeof f->data) return NTFS_E_BADREC;
        memcpy(f->data, v, vl);
        f->resident = 1;
        f->size = f->init_size = vl;
        return 0;
    }
    if (sizes) {
        f->size = le64(a + 0x30);
        f->init_size = le64(a + 0x38);
        if (f->init_size > f->size) f->init_size = f->size;
    }
    (void)fs;
    return decode_runs(a, f->runs, &f->nruns, NTFS_MAX_RUNS);
}

int ntfs_open(struct ntfs_fs *fs, uint32_t mft, struct ntfs_file *f)
{
    memset(f, 0, sizeof *f - sizeof f->data);
    f->mft = mft;
    int rc = load_rec(fs, mft, orec);
    if (rc < 0) return rc;
    if (le16(orec + 0x16) & 2) return NTFS_E_ISDIR;

    uint8_t *al = find_attr(fs, orec, AT_ATTRIBUTE_LIST, 0, 0);
    if (!al) {
        uint8_t *d = find_attr(fs, orec, AT_DATA, 0, 0);
        return d ? take_data(fs, d, f, 1) : NTFS_E_NODATA;
    }

    /* the data attribute may be split over several MFT records */
    uint32_t alen;
    if (!al[8]) {
        const uint8_t *v = res_value(al, &alen);
        if (alen > sizeof alist) return NTFS_E_UNSUPPORTED;
        memcpy(alist, v, alen);
    } else {
        uint32_t anr = 0;
        if (decode_runs(al, truns, &anr, 64) < 0) return NTFS_E_BADREC;
        uint64_t sz = le64(al + 0x30);
        if (sz > sizeof alist) return NTFS_E_UNSUPPORTED;
        alen = (uint32_t)sz;
        if (read_runs(fs, truns, anr, 0, alist, alen) < 0) return NTFS_E_IO;
    }
    int found = 0;
    for (uint32_t off = 0; off + 0x1A <= alen;) {
        const uint8_t *e = alist + off;
        uint32_t elen = le16(e + 4);
        if (elen < 0x1A || off + elen > alen) return NTFS_E_BADREC;
        if (le32(e) == AT_DATA && e[6] == 0) {
            uint32_t ref = le32(e + 0x10);
            uint64_t lowest = le64(e + 8);
            uint8_t *rec = orec;
            if (ref != mft) {
                rc = load_rec(fs, ref, oext);
                if (rc < 0) return rc;
                rec = oext;
            }
            uint8_t *d = 0;
            while ((d = find_attr(fs, rec, AT_DATA, 0, d)) != 0)
                if (!d[8] ? lowest == 0 : le64(d + 0x10) == lowest) break;
            if (!d) return NTFS_E_BADREC;
            rc = take_data(fs, d, f, lowest == 0);
            if (rc < 0) return rc;
            found = 1;
            if (f->resident) break;
        }
        off += elen;
    }
    return found ? 0 : NTFS_E_NODATA;
}

int ntfs_read(struct ntfs_fs *fs, struct ntfs_file *f, uint64_t off, void *buf, uint32_t len)
{
    if (off >= f->size) return 0;
    if ((uint64_t)len > f->size - off) len = (uint32_t)(f->size - off);
    if (f->resident) {
        memcpy(buf, f->data + (uint32_t)off, len);
        return (int)len;
    }
    uint32_t real = 0;                                                /* bytes below the initialised size */
    if (off < f->init_size) real = (uint64_t)len < f->init_size - off ? len : (uint32_t)(f->init_size - off);
    if (real) {
        int rc = read_runs(fs, f->runs, f->nruns, off, buf, real);
        if (rc < 0) return rc;
    }
    if (real < len) memset((uint8_t *)buf + real, 0, len - real);
    return (int)len;
}

/* ----------------------------------------------------------- write support */
static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static void put64(uint8_t *p, uint64_t v) { put32(p, (uint32_t)v); put32(p + 4, (uint32_t)(v >> 32)); }

void ntfs_set_writer(struct ntfs_fs *fs, ntfs_write_fn write)
{
    if (fs) fs->write = write;
}

static int wr(struct ntfs_fs *fs, uint32_t lba, uint32_t n, const void *buf)
{
    if (!fs->write) return NTFS_E_RDONLY;
    return fs->write(fs->ctx, fs->part_lba + lba, n, buf) < 0 ? NTFS_E_IO : 0;
}

static int write_runs(struct ntfs_fs *fs, const struct ntfs_run *r, uint32_t nr, uint64_t off, const uint8_t *buf,
                      uint32_t len)
{
    const uint32_t cs = fs->cluster_shift, cmask = (1u << cs) - 1u;
    uint32_t i = 0;
    while (len) {
        uint32_t vcn = (uint32_t)shr64(off, cs), in_cl = (uint32_t)off & cmask;
        while (i < nr && vcn >= r[i].vcn + r[i].len) i++;
        if (i == nr || vcn < r[i].vcn) {
            for (i = 0; i < nr && !(vcn >= r[i].vcn && vcn < r[i].vcn + r[i].len); i++) ;
            if (i == nr || r[i].lcn == NTFS_SPARSE) return NTFS_E_BADREC;
        }
        uint64_t left = shl64(r[i].vcn + r[i].len - vcn, cs) - in_cl;
        uint32_t n = (uint64_t)len < left ? len : (uint32_t)left;
        uint64_t pos = shl64(r[i].lcn + (vcn - r[i].vcn), cs) + in_cl;
        uint32_t s = (uint32_t)shr64(pos, 9), so = (uint32_t)pos & 511u;
        if (so == 0 && n >= 512u) {
            n &= ~511u;
            if (n > 65536u) n = 65536u;
            if (wr(fs, s, n >> 9, buf) < 0) return NTFS_E_IO;
        } else {
            if (rd(fs, s, 1, sec) < 0) return NTFS_E_IO;
            if (n > 512u - so) n = 512u - so;
            memcpy(sec + so, buf, n);
            if (wr(fs, s, 1, sec) < 0) return NTFS_E_IO;
        }
        off += n; buf += n; len -= n;
    }
    return 0;
}

static int unfixup(uint8_t *b, uint32_t size)
{
    uint32_t uo = le16(b + 4), un = le16(b + 6);
    if (un < 2 || (un - 1) * 512u != size || uo + un * 2u > size) return NTFS_E_BADREC;
    uint16_t usn = (uint16_t)(le16(b + uo) + 1u);
    if (usn == 0 || usn == 0xFFFFu) usn = 1;
    put16(b + uo, usn);
    for (uint32_t i = 1; i < un; i++) {
        uint8_t *e = b + i * 512u - 2;
        b[uo + 2 * i]     = e[0];
        b[uo + 2 * i + 1] = e[1];
        e[0] = b[uo];
        e[1] = b[uo + 1];
    }
    return 0;
}

static int save_rec(struct ntfs_fs *fs, uint32_t n, uint8_t *b)
{
    uint32_t size = 1u << fs->rec_shift;
    if (unfixup(b, size) < 0) return NTFS_E_BADREC;
    int rc = write_runs(fs, fs->mft_runs, fs->mft_nruns, shl64(n, fs->rec_shift), b, size);
    fixup(b, size, "FILE");
    return rc;
}

static uint32_t utf8_to_utf16(const char *s, uint8_t *out, uint32_t max_chars)
{
    uint32_t n = 0;
    const uint8_t *p = (const uint8_t *)s;
    while (*p && n < max_chars) {
        uint32_t c;
        if (*p < 0x80) { c = *p++; }
        else if ((*p & 0xE0) == 0xC0 && p[1]) { c = ((p[0] & 0x1F) << 6) | (p[1] & 0x3F); p += 2; }
        else if ((*p & 0xF0) == 0xE0 && p[1] && p[2]) {
            c = ((p[0] & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
            p += 3;
        } else { c = *p++; }
        put16(out + 2 * n, (uint16_t)c);
        n++;
    }
    return n;
}

static int utf16_cmp_ci(const uint8_t *a, uint32_t alen, const uint8_t *b, uint32_t blen)
{
    uint32_t n = alen < blen ? alen : blen;
    for (uint32_t i = 0; i < n; i++) {
        uint16_t ca = le16(a + 2 * i);
        uint16_t cb = le16(b + 2 * i);
        if (ca >= 'a' && ca <= 'z') ca -= 32;
        if (cb >= 'a' && cb <= 'z') cb -= 32;
        if (ca != cb) return (int)ca - (int)cb;
    }
    return (int)alen - (int)blen;
}

/* Allocate or free an MFT record in $MFT's $BITMAP (record 0) */
static int mft_bitmap_op(struct ntfs_fs *fs, uint32_t *io_mft, int alloc)
{
    int rc = load_rec(fs, 0, oext);
    if (rc < 0) return rc;
    uint8_t *d = find_attr(fs, oext, AT_DATA, 0, 0);
    if (!d || !d[8]) return NTFS_E_BADREC;
    uint32_t max_recs = (uint32_t)shr64(le64(d + 0x30), fs->rec_shift);

    uint8_t *bm = find_attr(fs, oext, AT_BITMAP, 0, 0);
    if (!bm) return NTFS_E_BADREC;

    if (!bm[8]) {
        uint32_t bml = le32(bm + 0x10);
        uint8_t *bv = bm + le16(bm + 0x14);
        if (!alloc) {
            uint32_t m = *io_mft;
            if ((m >> 3) < bml) bv[m >> 3] &= ~(1u << (m & 7));
            return save_rec(fs, 0, oext);
        }
        for (uint32_t m = 24; m < max_recs && (m >> 3) < bml; m++) {
            if (!(bv[m >> 3] & (1u << (m & 7)))) {
                bv[m >> 3] |= (uint8_t)(1u << (m & 7));
                *io_mft = m;
                return save_rec(fs, 0, oext);
            }
        }
        return NTFS_E_NOSPC;
    }

    uint32_t bnr = 0;
    if (decode_runs(bm, truns, &bnr, 64) < 0) return NTFS_E_BADREC;
    uint32_t bml = (uint32_t)le64(bm + 0x30);
    static uint32_t s_mft_hint = 24;
    if (!alloc) {
        uint32_t m = *io_mft, boff = (m >> 3) & ~511u;
        if (boff >= bml) return 0;
        uint32_t chunk = (bml - boff) > 512u ? 512u : (bml - boff);
        if (read_runs(fs, truns, bnr, boff, lbmp, chunk) < 0) return NTFS_E_IO;
        lbmp[(m >> 3) - boff] &= ~(1u << (m & 7));
        if (m >= 24 && m < s_mft_hint) s_mft_hint = m;
        return write_runs(fs, truns, bnr, boff, lbmp, chunk);
    }

    uint32_t start_sec = (s_mft_hint >> 3) & ~511u;
    if (start_sec >= bml) start_sec = 0;
    for (int pass = 0; pass < 2; pass++) {
        uint32_t b0 = (pass == 0) ? start_sec : 0;
        uint32_t b1 = (pass == 0) ? bml : start_sec;
        for (uint32_t boff = b0; boff < b1; boff += 512u) {
            uint32_t chunk = (bml - boff) > 512u ? 512u : (bml - boff);
            if (read_runs(fs, truns, bnr, boff, lbmp, chunk) < 0) return NTFS_E_IO;
            for (uint32_t bi = 0; bi < chunk; bi++) {
                if (lbmp[bi] == 0xFFu) continue;
                for (uint32_t bit = 0; bit < 8; bit++) {
                    uint32_t m = ((boff + bi) << 3) + bit;
                    if (m < 24) continue;
                    if (m >= max_recs) break;
                    if (!(lbmp[bi] & (1u << bit))) {
                        lbmp[bi] |= (uint8_t)(1u << bit);
                        if (write_runs(fs, truns, bnr, boff, lbmp, chunk) < 0) return NTFS_E_IO;
                        s_mft_hint = m + 1;
                        *io_mft = m;
                        return 0;
                    }
                }
            }
        }
    }
    return NTFS_E_NOSPC;
}

static uint32_t s_bm_part_lba = 0xFFFFFFFFu;
static uint32_t s_bm_bnr = 0, s_bm_bml = 0, s_bm_hint = 0;
static struct ntfs_run s_bm_runs[64];
static uint8_t  s_bm_cache[4096] __attribute__((aligned(32)));
static uint32_t s_bm_cache_off = 0xFFFFFFFFu, s_bm_cache_len = 0;
static uint32_t s_bm_dirty_lo = 0xFFFFFFFFu, s_bm_dirty_hi = 0;

static int bitmap_flush_cache(struct ntfs_fs *fs)
{
    if (s_bm_dirty_lo == 0xFFFFFFFFu || s_bm_cache_off == 0xFFFFFFFFu)
        return 0;
    uint32_t lo = s_bm_dirty_lo & ~511u;
    uint32_t hi = (s_bm_dirty_hi + 511u) & ~511u;
    if (hi > s_bm_cache_len) hi = s_bm_cache_len;
    s_bm_dirty_lo = 0xFFFFFFFFu;
    s_bm_dirty_hi = 0;
    if (hi <= lo) return 0;
    return write_runs(fs, s_bm_runs, s_bm_bnr, s_bm_cache_off + lo, s_bm_cache + lo, hi - lo);
}

/* Allocate `need` contiguous clusters from $Bitmap (MFT record 6) */
static int bitmap_alloc_clusters(struct ntfs_fs *fs, uint32_t need, uint32_t *out_lcn)
{
    if (need == 0) return NTFS_E_BADREC;
    if (s_bm_part_lba != fs->part_lba || s_bm_bnr == 0) {
        int rc = load_rec(fs, 6, oext);
        if (rc < 0) return rc;
        uint8_t *d = find_attr(fs, oext, AT_DATA, 0, 0);
        if (!d || !d[8]) return NTFS_E_BADREC;
        s_bm_bnr = 0;
        if (decode_runs(d, s_bm_runs, &s_bm_bnr, 64) < 0) return NTFS_E_BADREC;
        s_bm_bml = (uint32_t)le64(d + 0x30);
        s_bm_part_lba = fs->part_lba;
        s_bm_hint = 0;
        s_bm_cache_off = 0xFFFFFFFFu;
        s_bm_dirty_lo = 0xFFFFFFFFu;
    }
    uint32_t bml = s_bm_bml;

    for (int pass = 0; pass < 2; pass++) {
        uint32_t start_boff = (pass == 0) ? (s_bm_hint & ~4095u) : 0;
        uint32_t end_boff   = (pass == 0) ? bml : (s_bm_hint & ~4095u);
        uint32_t run_start = 0, run_len = 0;
        for (uint32_t boff = start_boff; boff < end_boff; boff += 4096u) {
            uint32_t chunk = (bml - boff) > 4096u ? 4096u : (bml - boff);
            if (s_bm_cache_off != boff) {
                if (bitmap_flush_cache(fs) < 0) return NTFS_E_IO;
                if (read_runs(fs, s_bm_runs, s_bm_bnr, boff, s_bm_cache, chunk) < 0) return NTFS_E_IO;
                s_bm_cache_off = boff;
                s_bm_cache_len = chunk;
            }
            for (uint32_t bi = 0; bi < chunk; bi++) {
                uint8_t byte = s_bm_cache[bi];
                if (byte == 0xFFu) { run_len = 0; continue; }
                for (uint32_t bit = 0; bit < 8; bit++) {
                    uint32_t lcn = ((boff + bi) << 3) + bit;
                    if (lcn < 64) continue;
                    if (lcn >= fs->total_clusters) { run_len = 0; break; }
                    if (!(byte & (1u << bit))) {
                        if (run_len == 0) run_start = lcn;
                        run_len++;
                        if (run_len == need) {
                            uint32_t first_b = (run_start >> 3);
                            uint32_t last_b  = ((run_start + need - 1) >> 3);
                            if (first_b >= boff && last_b < boff + chunk) {
                                /* Fast path: mark in cached 4 KB window, flush on sync */
                                for (uint32_t c = run_start; c < run_start + need; c++)
                                    s_bm_cache[(c >> 3) - boff] |= (uint8_t)(1u << (c & 7));
                                uint32_t rel_lo = first_b - boff;
                                uint32_t rel_hi = last_b - boff + 1u;
                                if (rel_lo < s_bm_dirty_lo) s_bm_dirty_lo = rel_lo;
                                if (rel_hi > s_bm_dirty_hi) s_bm_dirty_hi = rel_hi;
                            } else {
                                if (bitmap_flush_cache(fs) < 0) return NTFS_E_IO;
                                s_bm_cache_off = 0xFFFFFFFFu;
                                uint32_t first_sec = first_b & ~511u;
                                uint32_t last_sec  = last_b & ~511u;
                                for (uint32_t sb = first_sec; sb <= last_sec; sb += 512u) {
                                    uint32_t schunk = (bml - sb) > 512u ? 512u : (bml - sb);
                                    if (read_runs(fs, s_bm_runs, s_bm_bnr, sb, lbmp, schunk) < 0) return NTFS_E_IO;
                                    for (uint32_t c = run_start; c < run_start + need; c++) {
                                        uint32_t bpos = c >> 3;
                                        if (bpos >= sb && bpos < sb + schunk)
                                            lbmp[bpos - sb] |= (uint8_t)(1u << (c & 7));
                                    }
                                    if (write_runs(fs, s_bm_runs, s_bm_bnr, sb, lbmp, schunk) < 0) return NTFS_E_IO;
                                }
                            }
                            s_bm_hint = first_b & ~4095u;
                            *out_lcn = run_start;
                            return 0;
                        }
                    } else {
                        run_len = 0;
                    }
                }
            }
        }
    }
    return NTFS_E_NOSPC;
}

/* Encode runlist into out; returns bytes written (including terminating 0) */
static uint32_t encode_runs(const struct ntfs_run *runs, uint32_t nruns, uint8_t *out)
{
    uint32_t pos = 0;
    int32_t prev_lcn = 0;
    for (uint32_t i = 0; i < nruns; i++) {
        int32_t dlcn = (int32_t)runs[i].lcn - prev_lcn;
        prev_lcn = (int32_t)runs[i].lcn;
        out[pos++] = 0x44; /* 4-byte length, 4-byte signed LCN delta */
        put32(out + pos, runs[i].len);
        pos += 4;
        put32(out + pos, (uint32_t)dlcn);
        pos += 4;
    }
    out[pos++] = 0;
    return pos;
}

/* Locate entry position for uname in directory `dir_mft`.
 * Loads `lrec` (and `lidx` if descending into $INDEX_ALLOCATION).
 * Sets *out_hdr, *out_e, *out_in_root, *out_blk_off, *out_idx_size, *out_exact. */
static int dir_locate(struct ntfs_fs *fs, uint32_t dir_mft, const uint8_t *uname, uint32_t ulen,
                      uint8_t **out_hdr, uint8_t **out_e, int *out_in_root,
                      uint64_t *out_blk_off, uint32_t *out_idx_size, int *out_exact)
{
    int rc = load_rec(fs, dir_mft, lrec);
    if (rc < 0) return rc;
    if (!(le16(lrec + 0x16) & 2)) return NTFS_E_NOTDIR;

    uint8_t *root = find_attr(fs, lrec, AT_INDEX_ROOT, "$I30", 0);
    if (!root || root[8]) return NTFS_E_UNSUPPORTED;
    uint32_t vl = le32(root + 0x10);
    uint8_t *v = root + le16(root + 0x14);
    if (vl < 0x20) return NTFS_E_BADREC;
    uint32_t idx_size = le32(v + 8);
    *out_idx_size = idx_size;

    uint8_t *hdr = v + 0x10;
    int in_root = 1;
    uint64_t blk_off = 0;
    uint32_t alloc_nr = 0;

    for (int depth = 0; depth < 8; depth++) {
        uint32_t eo = le32(hdr), il = le32(hdr + 4);
        uint8_t *e = hdr + eo, *end = hdr + il;
        while (e + 16 <= end) {
            uint32_t elen = le16(e + 8), klen = le16(e + 10), fl = le16(e + 12);
            if (fl & 2) break;
            if (elen < 16 || e + elen > end) return NTFS_E_BADREC;
            if (klen >= 0x42 && 16 + klen <= elen) {
                const uint8_t *k = e + 16;
                uint32_t nl = k[0x40];
                int cmp = utf16_cmp_ci(k + 0x42, nl, uname, ulen);
                if (cmp == 0) {
                    *out_hdr = hdr;
                    *out_e = e;
                    *out_in_root = in_root;
                    *out_blk_off = blk_off;
                    *out_exact = 1;
                    return 0;
                }
                if (cmp > 0) break;
            }
            e += elen;
        }
        uint32_t elen = le16(e + 8), fl = le16(e + 12);
        if (!(fl & 1)) {
            *out_hdr = hdr;
            *out_e = e;
            *out_in_root = in_root;
            *out_blk_off = blk_off;
            *out_exact = 0;
            return 0;
        }
        /* Follow B-tree child pointer in last 8 bytes of entry `e` */
        if (in_root) {
            uint8_t *alloc = find_attr(fs, lrec, AT_INDEX_ALLOCATION, "$I30", 0);
            if (!alloc || !alloc[8] || idx_size < 512 || idx_size > sizeof lidx || log2u(idx_size) < 0)
                return NTFS_E_UNSUPPORTED;
            alloc_nr = 0;
            rc = decode_runs(alloc, lruns, &alloc_nr, NTFS_MAX_RUNS);
            if (rc < 0) return rc;
            in_root = 0;
        }
        uint64_t vcn = le64(e + elen - 8);
        blk_off = (idx_size >= (1u << fs->cluster_shift))
                      ? shl64(vcn, fs->cluster_shift)
                      : shl64(vcn, 9);
        rc = read_runs(fs, lruns, alloc_nr, blk_off, lidx, idx_size);
        if (rc < 0) return rc;
        if (fixup(lidx, idx_size, "INDX") < 0) return NTFS_E_BADREC;
        hdr = lidx + 0x18;
    }
    return NTFS_E_BADREC;
}

int ntfs_create(struct ntfs_fs *fs, uint32_t dir_mft, const char *name, int is_dir, uint32_t *out_mft)
{
    if (!fs || !fs->write) return NTFS_E_RDONLY;
    uint8_t uname[512];
    uint32_t ulen = utf8_to_utf16(name, uname, 255);
    if (ulen == 0) return NTFS_E_BADREC;

    uint8_t *hdr = 0, *e = 0;
    int in_root = 1, exact = 0;
    uint64_t blk_off = 0;
    uint32_t idx_size = 4096;
    int rc = dir_locate(fs, dir_mft, uname, ulen, &hdr, &e, &in_root, &blk_off, &idx_size, &exact);
    if (rc < 0) return rc;
    if (exact) {
        if (out_mft) *out_mft = le32(e);
        return NTFS_E_EXIST;
    }

    uint16_t dir_seq = le16(lrec + 0x10);
    uint32_t mft = 0;
    rc = mft_bitmap_op(fs, &mft, 1);
    if (rc < 0) return rc;

    uint32_t rec_size = 1u << fs->rec_shift;
    uint16_t seq = 1;
    if (read_runs(fs, fs->mft_runs, fs->mft_nruns, shl64(mft, fs->rec_shift), orec, rec_size) == 0 &&
        memcmp(orec, "FILE", 4) == 0) {
        seq = (uint16_t)(le16(orec + 0x10) + 1u);
        if (seq == 0) seq = 1;
    }

    /* Build MFT record `mft` in `orec` */
    memset(orec, 0, rec_size);
    memcpy(orec, "FILE", 4);
    uint16_t uo = 0x30;
    uint16_t un = (uint16_t)(1u + (rec_size >> 9));
    put16(orec + 0x04, uo);
    put16(orec + 0x06, un);
    put16(orec + 0x10, seq);
    put16(orec + 0x12, 1); /* hard_link_count = 1 */
    uint16_t first_attr = (uint16_t)((uo + un * 2u + 7u) & ~7u);
    put16(orec + 0x14, first_attr);
    put16(orec + 0x16, is_dir ? 0x0003u : 0x0001u);
    put32(orec + 0x1C, rec_size);
    put16(orec + 0x28, 4); /* next_attr_id */
    put32(orec + 0x2C, mft);
    put16(orec + uo, 1);   /* initial USN = 1 */

    uint32_t off = first_attr;
    /* 1. $STANDARD_INFORMATION (0x10), 72 bytes */
    uint8_t *a_si = orec + off;
    put32(a_si + 0x00, 0x10u);
    put32(a_si + 0x04, 0x48u);
    put16(a_si + 0x0A, 0x18u);
    put16(a_si + 0x0E, 0);
    put32(a_si + 0x10, 0x30u);
    put16(a_si + 0x14, 0x18u);
    uint64_t ts = 0x01DA000000000000ULL;
    put64(a_si + 0x18 + 0x00, ts);
    put64(a_si + 0x18 + 0x08, ts);
    put64(a_si + 0x18 + 0x10, ts);
    put64(a_si + 0x18 + 0x18, ts);
    put32(a_si + 0x18 + 0x20, is_dir ? 0u : NTFS_ATTR_ARCHIVE);
    off += 0x48u;

    /* 2. $FILE_NAME (0x30) */
    uint8_t fn_val[600];
    memset(fn_val, 0, sizeof(fn_val));
    put32(fn_val + 0x00, dir_mft);
    put16(fn_val + 0x06, dir_seq);
    put64(fn_val + 0x08, ts);
    put64(fn_val + 0x10, ts);
    put64(fn_val + 0x18, ts);
    put64(fn_val + 0x20, ts);
    put32(fn_val + 0x38, is_dir ? NTFS_ATTR_DIRECTORY : NTFS_ATTR_ARCHIVE);
    fn_val[0x40] = (uint8_t)ulen;
    fn_val[0x41] = 3; /* Win32 & DOS namespace */
    memcpy(fn_val + 0x42, uname, ulen * 2u);
    uint32_t fn_val_len  = 0x42u + ulen * 2u;
    uint32_t fn_attr_len = (0x18u + fn_val_len + 7u) & ~7u;

    uint8_t *a_fn = orec + off;
    put32(a_fn + 0x00, AT_FILE_NAME);
    put32(a_fn + 0x04, fn_attr_len);
    put16(a_fn + 0x0A, 0x18u);
    put16(a_fn + 0x0E, 1);
    put32(a_fn + 0x10, fn_val_len);
    put16(a_fn + 0x14, 0x18u);
    put16(a_fn + 0x16, 1); /* indexed */
    memcpy(a_fn + 0x18, fn_val, fn_val_len);
    off += fn_attr_len;

    if (!is_dir) {
        /* 3. $DATA (0x80) empty resident (24 bytes) */
        uint8_t *a_dat = orec + off;
        put32(a_dat + 0x00, AT_DATA);
        put32(a_dat + 0x04, 0x18u);
        put16(a_dat + 0x0A, 0x18u);
        put16(a_dat + 0x0E, 2);
        put32(a_dat + 0x10, 0);
        put16(a_dat + 0x14, 0x18u);
        off += 0x18u;
    } else {
        /* 3. $INDEX_ROOT (0x90, named "$I30") with empty sentinel entry (80 bytes) */
        uint8_t *a_ir = orec + off;
        put32(a_ir + 0x00, AT_INDEX_ROOT);
        put32(a_ir + 0x04, 0x50u);
        a_ir[0x09] = 4; /* name_len = 4 ("$I30") */
        put16(a_ir + 0x0A, 0x18u);
        put16(a_ir + 0x0E, 2);
        put32(a_ir + 0x10, 0x30u); /* value length = 48 bytes */
        put16(a_ir + 0x14, 0x20u); /* value offset = 32 */
        put16(a_ir + 0x18, '$'); put16(a_ir + 0x1A, 'I'); put16(a_ir + 0x1C, '3'); put16(a_ir + 0x1E, '0');
        uint8_t *irv = a_ir + 0x20;
        put32(irv + 0x00, AT_FILE_NAME);
        put32(irv + 0x04, 1); /* collation rule: FILENAME */
        put32(irv + 0x08, 4096);
        irv[0x0C] = 1;
        /* Index header at irv + 0x10 */
        put32(irv + 0x10 + 0x00, 0x10u); /* entries_offset = 16 */
        put32(irv + 0x10 + 0x04, 0x20u); /* index_length = 32 */
        put32(irv + 0x10 + 0x08, 0x20u); /* allocated_size = 32 */
        /* Sentinel entry at irv + 0x20 */
        put16(irv + 0x20 + 0x08, 0x10u); /* elen = 16 */
        put16(irv + 0x20 + 0x0C, 0x02u); /* flags = END */
        off += 0x50u;
    }

    put32(orec + off, AT_END);
    put32(orec + 0x18, off + 8u);
    rc = save_rec(fs, mft, orec);
    if (rc < 0) {
        mft_bitmap_op(fs, &mft, 0);
        return rc;
    }

    /* Insert index entry into parent directory `dir_mft` */
    uint32_t new_elen = (0x10u + fn_val_len + 7u) & ~7u;
    uint8_t new_ent[624];
    memset(new_ent, 0, new_elen);
    put32(new_ent + 0x00, mft);
    put16(new_ent + 0x06, seq);
    put16(new_ent + 0x08, (uint16_t)new_elen);
    put16(new_ent + 0x0A, (uint16_t)fn_val_len);
    put16(new_ent + 0x0C, 0);
    memcpy(new_ent + 0x10, fn_val, fn_val_len);

    if (in_root) {
        uint32_t used = le32(lrec + 0x18);
        if (used + new_elen <= rec_size - 8u) {
            uint8_t *root = find_attr(fs, lrec, AT_INDEX_ROOT, "$I30", 0);
            memmove(e + new_elen, e, (size_t)((lrec + used) - e));
            memcpy(e, new_ent, new_elen);
            put32(hdr + 0x04, le32(hdr + 0x04) + new_elen);
            put32(hdr + 0x08, le32(hdr + 0x08) + new_elen);
            put32(root + 0x10, le32(root + 0x10) + new_elen);
            put32(root + 0x04, le32(root + 0x04) + new_elen);
            put32(lrec + 0x18, used + new_elen);
            rc = save_rec(fs, dir_mft, lrec);
        } else {
            rc = NTFS_E_NOSPC;
        }
    } else {
        uint32_t il = le32(hdr + 0x04), avail = le32(hdr + 0x08);
        if (il + new_elen <= avail) {
            memmove(e + new_elen, e, (size_t)((hdr + il) - e));
            memcpy(e, new_ent, new_elen);
            put32(hdr + 0x04, il + new_elen);
            if (unfixup(lidx, idx_size) < 0) rc = NTFS_E_BADREC;
            else {
                uint8_t *alloc = find_attr(fs, lrec, AT_INDEX_ALLOCATION, "$I30", 0);
                uint32_t anr = 0;
                decode_runs(alloc, lruns, &anr, NTFS_MAX_RUNS);
                rc = write_runs(fs, lruns, anr, blk_off, lidx, idx_size);
                fixup(lidx, idx_size, "INDX");
            }
        } else {
            rc = NTFS_E_NOSPC;
        }
    }

    if (rc < 0) {
        mft_bitmap_op(fs, &mft, 0);
        return rc;
    }
    if (out_mft) *out_mft = mft;
    return 0;
}

static void dir_update_size(struct ntfs_fs *fs, uint32_t dir_mft, const char *name,
                            uint64_t alloc_sz, uint64_t data_sz)
{
    uint8_t uname[512];
    uint32_t ulen = utf8_to_utf16(name, uname, 255);
    if (!ulen) return;
    uint8_t *hdr = 0, *e = 0;
    int in_root = 1, exact = 0;
    uint64_t blk_off = 0;
    uint32_t idx_size = 4096;
    if (dir_locate(fs, dir_mft, uname, ulen, &hdr, &e, &in_root, &blk_off, &idx_size, &exact) < 0 || !exact)
        return;
    put64(e + 0x10 + 0x28, alloc_sz);
    put64(e + 0x10 + 0x30, data_sz);
    if (in_root) {
        save_rec(fs, dir_mft, lrec);
    } else {
        if (unfixup(lidx, idx_size) == 0) {
            uint8_t *alloc = find_attr(fs, lrec, AT_INDEX_ALLOCATION, "$I30", 0);
            uint32_t anr = 0;
            if (alloc && decode_runs(alloc, lruns, &anr, NTFS_MAX_RUNS) == 0)
                write_runs(fs, lruns, anr, blk_off, lidx, idx_size);
            fixup(lidx, idx_size, "INDX");
        }
    }
}

static int sync_file_mft(struct ntfs_fs *fs, struct ntfs_file *f, uint32_t dir_mft, const char *name)
{
    int rc = load_rec(fs, f->mft, orec);
    if (rc < 0) return rc;
    uint32_t rec_size = 1u << fs->rec_shift;

    /* Update $FILE_NAME size fields inside MFT record if present */
    uint8_t *fn = find_attr(fs, orec, AT_FILE_NAME, 0, 0);
    uint64_t alloc_sz = f->resident ? ((f->size + 7ull) & ~7ull)
                                    : shl64(f->nruns ? (f->runs[f->nruns - 1].vcn + f->runs[f->nruns - 1].len) : 0,
                                            fs->cluster_shift);
    if (fn && !fn[8] && le32(fn + 0x10) >= 0x42u) {
        uint8_t *fnv = fn + le16(fn + 0x14);
        put64(fnv + 0x28, alloc_sz);
        put64(fnv + 0x30, f->size);
    }

    uint8_t *d = find_attr(fs, orec, AT_DATA, 0, 0);
    if (!d) return NTFS_E_NODATA;
    uint32_t used = le32(orec + 0x18);
    uint32_t old_len = le32(d + 0x04);
    uint16_t attr_id = le16(d + 0x0E);

    if (f->resident) {
        uint32_t vo = 0x18u;
        uint32_t new_len = (vo + (uint32_t)f->size + 7u) & ~7u;
        if (used - old_len + new_len > rec_size - 8u) return NTFS_E_NOSPC;
        memmove(d + new_len, d + old_len, (size_t)((orec + used) - (d + old_len)));
        memset(d, 0, new_len);
        put32(d + 0x00, AT_DATA);
        put32(d + 0x04, new_len);
        put16(d + 0x0A, (uint16_t)vo);
        put16(d + 0x0E, attr_id);
        put32(d + 0x10, (uint32_t)f->size);
        put16(d + 0x14, (uint16_t)vo);
        if (f->size > 0)
            memcpy(d + vo, f->data, (size_t)f->size);
        put32(orec + 0x18, used - old_len + new_len);
    } else {
        uint8_t rl[256];
        uint32_t rl_len = encode_runs(f->runs, f->nruns, rl);
        uint32_t new_len = (0x40u + rl_len + 7u) & ~7u;
        if (used - old_len + new_len > rec_size - 8u) return NTFS_E_NOSPC;
        uint32_t total_cl = f->nruns ? (f->runs[f->nruns - 1].vcn + f->runs[f->nruns - 1].len) : 0;
        memmove(d + new_len, d + old_len, (size_t)((orec + used) - (d + old_len)));
        memset(d, 0, new_len);
        put32(d + 0x00, AT_DATA);
        put32(d + 0x04, new_len);
        d[0x08] = 1; /* non-resident */
        put16(d + 0x0A, 0x40u);
        put16(d + 0x0E, attr_id);
        put64(d + 0x10, 0);
        put64(d + 0x18, total_cl ? (uint64_t)(total_cl - 1u) : 0);
        put16(d + 0x20, 0x40u);
        put64(d + 0x28, alloc_sz);
        put64(d + 0x30, f->size);
        put64(d + 0x38, f->init_size);
        memcpy(d + 0x40, rl, rl_len);
        put32(orec + 0x18, used - old_len + new_len);
    }

    if (bitmap_flush_cache(fs) < 0) return NTFS_E_IO;
    rc = save_rec(fs, f->mft, orec);
    if (rc < 0) return rc;
    if (name && name[0])
        dir_update_size(fs, dir_mft, name, alloc_sz, f->size);
    return 0;
}

int ntfs_sync(struct ntfs_fs *fs, struct ntfs_file *f, uint32_t dir_mft, const char *name)
{
    if (!fs || !fs->write) return NTFS_E_RDONLY;
    return sync_file_mft(fs, f, dir_mft, name);
}

int ntfs_write(struct ntfs_fs *fs, struct ntfs_file *f, uint32_t dir_mft, const char *name,
               uint64_t off, const void *buf, uint32_t len)
{
    if (!fs || !fs->write) return NTFS_E_RDONLY;
    if (len == 0) return 0;
    uint64_t end_off = off + len;

    /* Keep resident if <= 600 bytes and currently resident */
    if (f->resident && end_off <= 600u) {
        if (off > f->size)
            memset(f->data + (uint32_t)f->size, 0, (size_t)(off - f->size));
        memcpy(f->data + (uint32_t)off, buf, len);
        if (end_off > f->size)
            f->size = f->init_size = end_off;
        if (name) {
            int rc = sync_file_mft(fs, f, dir_mft, name);
            if (rc < 0) return rc;
        }
        return (int)len;
    }

    /* Convert resident -> non-resident or extend non-resident allocation */
    uint32_t cs = fs->cluster_shift;
    uint32_t need_cl = (uint32_t)shr64(end_off + (1u << cs) - 1u, cs);
    if (need_cl == 0) need_cl = 1;
    uint32_t have_cl = (!f->resident && f->nruns)
                           ? (f->runs[f->nruns - 1].vcn + f->runs[f->nruns - 1].len)
                           : 0;

    if (need_cl > have_cl) {
        uint32_t add_cl = need_cl - have_cl;
        uint32_t new_lcn = 0;
        int rc = bitmap_alloc_clusters(fs, add_cl, &new_lcn);
        if (rc < 0) return rc;
        if (f->resident) {
            f->nruns = 1;
            f->runs[0].vcn = 0;
            f->runs[0].lcn = new_lcn;
            f->runs[0].len = add_cl;
            f->resident = 0;
            if (f->size > 0) {
                memset(f->data + (uint32_t)f->size, 0, (size_t)(((f->size + 511u) & ~511u) - f->size));
                rc = write_runs(fs, f->runs, f->nruns, 0, f->data, (uint32_t)((f->size + 511u) & ~511u));
                if (rc < 0) return rc;
            }
        } else if (f->nruns > 0 &&
                   f->runs[f->nruns - 1].lcn + f->runs[f->nruns - 1].len == new_lcn) {
            f->runs[f->nruns - 1].len += add_cl;
        } else {
            if (f->nruns >= 16) return NTFS_E_FRAGMENTED;
            f->runs[f->nruns].vcn = have_cl;
            f->runs[f->nruns].lcn = new_lcn;
            f->runs[f->nruns].len = add_cl;
            f->nruns++;
        }
    }

    int rc = write_runs(fs, f->runs, f->nruns, off, (const uint8_t *)buf, len);
    if (rc < 0) return rc;
    if (end_off > f->size) f->size = end_off;
    if (end_off > f->init_size) f->init_size = end_off;
    if (name) {
        rc = sync_file_mft(fs, f, dir_mft, name);
        if (rc < 0) return rc;
    }
    return (int)len;
}

static void bitmap_free_runs(struct ntfs_fs *fs, const struct ntfs_run *runs, uint32_t nruns)
{
    if (!fs || !fs->write || !runs || nruns == 0)
        return;
    if (s_bm_part_lba != fs->part_lba || s_bm_bnr == 0) {
        if (load_rec(fs, 6, oext) < 0) return;
        uint8_t *d = find_attr(fs, oext, AT_DATA, 0, 0);
        if (!d || !d[8]) return;
        s_bm_bnr = 0;
        if (decode_runs(d, s_bm_runs, &s_bm_bnr, 64) < 0) return;
        s_bm_bml = (uint32_t)le64(d + 0x30);
        s_bm_part_lba = fs->part_lba;
        s_bm_hint = 0;
        s_bm_cache_off = 0xFFFFFFFFu;
        s_bm_dirty_lo = 0xFFFFFFFFu;
    }
    bitmap_flush_cache(fs);
    s_bm_cache_off = 0xFFFFFFFFu;
    uint32_t bml = s_bm_bml;
    for (uint32_t r = 0; r < nruns; r++) {
        uint32_t lcn = runs[r].lcn;
        uint32_t len = runs[r].len;
        if (lcn == NTFS_SPARSE || len == 0 || lcn < 64)
            continue;
        uint32_t first_b = lcn >> 3;
        uint32_t last_b  = (lcn + len - 1u) >> 3;
        uint32_t first_sec = first_b & ~511u;
        uint32_t last_sec  = last_b & ~511u;
        for (uint32_t sb = first_sec; sb <= last_sec && sb < bml; sb += 512u) {
            uint32_t schunk = (bml - sb) > 512u ? 512u : (bml - sb);
            if (read_runs(fs, s_bm_runs, s_bm_bnr, sb, lbmp, schunk) < 0)
                break;
            uint32_t c_lo = (sb << 3) > lcn ? (sb << 3) : lcn;
            uint32_t c_hi = ((sb + schunk) << 3) < (lcn + len) ? ((sb + schunk) << 3) : (lcn + len);
            for (uint32_t c = c_lo; c < c_hi; c++)
                lbmp[(c >> 3) - sb] &= (uint8_t)~(1u << (c & 7));
            write_runs(fs, s_bm_runs, s_bm_bnr, sb, lbmp, schunk);
        }
        if ((first_b & ~4095u) < s_bm_hint)
            s_bm_hint = first_b & ~4095u;
    }
}

int ntfs_truncate(struct ntfs_fs *fs, struct ntfs_file *f, uint32_t dir_mft, const char *name, uint64_t new_size)
{
    if (!fs || !fs->write) return NTFS_E_RDONLY;
    if (new_size == f->size) return 0;
    if (new_size == 0 && !f->resident && f->nruns > 0) {
        bitmap_free_runs(fs, f->runs, f->nruns);
        f->nruns = 0;
        f->resident = 1;
        f->size = 0;
        f->init_size = 0;
        return sync_file_mft(fs, f, dir_mft, name);
    }
    if (new_size < f->size) {
        f->size = new_size;
        if (f->init_size > new_size) f->init_size = new_size;
        return sync_file_mft(fs, f, dir_mft, name);
    }
    return 0;
}

int ntfs_remove(struct ntfs_fs *fs, uint32_t dir_mft, const char *name)
{
    if (!fs || !fs->write) return NTFS_E_RDONLY;
    uint8_t uname[512];
    uint32_t ulen = utf8_to_utf16(name, uname, 255);
    if (!ulen) return NTFS_E_NOENT;

    uint8_t *hdr = 0, *e = 0;
    int in_root = 1, exact = 0;
    uint64_t blk_off = 0;
    uint32_t idx_size = 4096;
    int rc = dir_locate(fs, dir_mft, uname, ulen, &hdr, &e, &in_root, &blk_off, &idx_size, &exact);
    if (rc < 0) return rc;
    if (!exact) return NTFS_E_NOENT;

    uint32_t elen = le16(e + 8), fl = le16(e + 12);
    if (fl & 1) return NTFS_E_UNSUPPORTED; /* internal B-tree split node */
    uint32_t target_mft = le32(e);

    if (in_root) {
        uint8_t *root = find_attr(fs, lrec, AT_INDEX_ROOT, "$I30", 0);
        uint32_t used = le32(lrec + 0x18);
        memmove(e, e + elen, (size_t)((lrec + used) - (e + elen)));
        put32(hdr + 0x04, le32(hdr + 0x04) - elen);
        put32(hdr + 0x08, le32(hdr + 0x08) - elen);
        put32(root + 0x10, le32(root + 0x10) - elen);
        put32(root + 0x04, le32(root + 0x04) - elen);
        put32(lrec + 0x18, used - elen);
        rc = save_rec(fs, dir_mft, lrec);
    } else {
        uint32_t il = le32(hdr + 0x04);
        memmove(e, e + elen, (size_t)((hdr + il) - (e + elen)));
        put32(hdr + 0x04, il - elen);
        if (unfixup(lidx, idx_size) < 0) rc = NTFS_E_BADREC;
        else {
            uint8_t *alloc = find_attr(fs, lrec, AT_INDEX_ALLOCATION, "$I30", 0);
            uint32_t anr = 0;
            decode_runs(alloc, lruns, &anr, NTFS_MAX_RUNS);
            rc = write_runs(fs, lruns, anr, blk_off, lidx, idx_size);
            fixup(lidx, idx_size, "INDX");
        }
    }
    if (rc < 0) return rc;

    struct ntfs_file tf;
    if (ntfs_open(fs, target_mft, &tf) == 0 && !tf.resident && tf.nruns > 0)
        bitmap_free_runs(fs, tf.runs, tf.nruns);

    if (load_rec(fs, target_mft, orec) == 0) {
        put16(orec + 0x16, (uint16_t)(le16(orec + 0x16) & ~1u));
        save_rec(fs, target_mft, orec);
    }
    mft_bitmap_op(fs, &target_mft, 0);
    return 0;
}
