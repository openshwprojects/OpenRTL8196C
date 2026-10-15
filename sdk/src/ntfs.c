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
