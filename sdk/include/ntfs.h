/*
 * ntfs.h - read-only NTFS reader on top of any 512-byte sector reader
 * (USB mass storage, ...).
 *
 * Supported: MBR (type 07), GPT and partitionless volumes; any cluster size;
 * MFT records with update-sequence fixups; resident and non-resident data,
 * fragmented files (data runs), sparse runs and uninitialised tails read as
 * zeros, files whose runs continue in other MFT records ($ATTRIBUTE_LIST);
 * directory indexes ($INDEX_ROOT + $INDEX_ALLOCATION + $BITMAP); UTF-16
 * names as UTF-8.  Not supported: compressed and encrypted files (open
 * returns NTFS_E_COMPRESSED / NTFS_E_ENCRYPTED), writing.
 *
 * Not re-entrant (static scratch buffers).  ntfs_open/ntfs_read may be called
 * from an ntfs_list callback, ntfs_list may not (collect entries first).
 */
#ifndef NTFS_H
#define NTFS_H

#include <stdint.h>

typedef int (*ntfs_read_fn)(void *ctx, uint32_t lba, uint32_t count, void *buf);
typedef int (*ntfs_write_fn)(void *ctx, uint32_t lba, uint32_t count, const void *buf);

#define NTFS_ROOT       5u             /* MFT record of the root directory */
#define NTFS_MAX_RUNS   512            /* fragments per open file */
#define NTFS_MAX_REC    4096           /* largest MFT record handled */

enum {
    NTFS_E_IO = -1, NTFS_E_NOFS = -2, NTFS_E_BADREC = -3, NTFS_E_NOTDIR = -4, NTFS_E_ISDIR = -5,
    NTFS_E_NODATA = -6, NTFS_E_COMPRESSED = -7, NTFS_E_ENCRYPTED = -8, NTFS_E_FRAGMENTED = -9,
    NTFS_E_UNSUPPORTED = -10, NTFS_E_NOSPC = -11, NTFS_E_EXIST = -12, NTFS_E_NOENT = -13,
    NTFS_E_RDONLY = -14,
};

struct ntfs_run { uint32_t vcn, lcn, len; };   /* clusters; lcn NTFS_SPARSE = hole */
#define NTFS_SPARSE 0xFFFFFFFFu

struct ntfs_fs {
    ntfs_read_fn  read;
    ntfs_write_fn write;
    void    *ctx;
    uint32_t part_lba;                 /* first sector of the volume */
    uint32_t cluster_shift;            /* log2(bytes per cluster) */
    uint32_t rec_shift;                /* log2(bytes per MFT record) */
    uint32_t total_clusters, mft_lcn;
    uint32_t mft_nruns;
    struct ntfs_run mft_runs[64];
    uint32_t serial_lo, serial_hi;
    char     label[64];                /* UTF-8 */
};

#define NTFS_ATTR_READONLY   0x0001u
#define NTFS_ATTR_HIDDEN     0x0002u
#define NTFS_ATTR_SYSTEM     0x0004u
#define NTFS_ATTR_ARCHIVE    0x0020u
#define NTFS_ATTR_SPARSE     0x0200u
#define NTFS_ATTR_REPARSE    0x0400u   /* symlink / junction: do not descend */
#define NTFS_ATTR_COMPRESSED 0x0800u
#define NTFS_ATTR_ENCRYPTED  0x4000u
#define NTFS_ATTR_DIRECTORY  0x10000000u

struct ntfs_dirent {
    char     name[256];                /* UTF-8, cut at 255 bytes */
    uint32_t mft;                      /* record number, for ntfs_list / ntfs_open */
    uint64_t size;                     /* from the directory index; may lag, ntfs_open has the exact size */
    uint32_t attr;                     /* NTFS_ATTR_* */
};

struct ntfs_file {
    uint32_t mft;
    uint64_t size;                     /* bytes */
    uint64_t init_size;                /* bytes actually written; the rest reads as zeros */
    int      resident;                 /* data stored inside the MFT record */
    uint32_t nruns;
    struct ntfs_run runs[NTFS_MAX_RUNS];
    uint8_t  data[NTFS_MAX_REC];       /* resident data */
};

/* mounts the first NTFS volume: partitionless, MBR type 07 or any GPT partition */
int ntfs_mount(struct ntfs_fs *fs, ntfs_read_fn read, void *ctx);
void ntfs_set_writer(struct ntfs_fs *fs, ntfs_write_fn write);
/* every entry of directory record dir_mft except "." and the system files;
 * DOS 8.3 aliases are skipped.  Stops early if cb returns nonzero. */
int ntfs_list(struct ntfs_fs *fs, uint32_t dir_mft, int (*cb)(const struct ntfs_dirent *e, void *arg), void *arg);
int ntfs_open(struct ntfs_fs *fs, uint32_t mft, struct ntfs_file *f);
/* reads up to len bytes at offset; returns bytes read (0 at end of file) or <0 */
int ntfs_read(struct ntfs_fs *fs, struct ntfs_file *f, uint64_t off, void *buf, uint32_t len);
/* write support (requires ntfs_set_writer): create file/dir, write data, truncate, remove */
int ntfs_create(struct ntfs_fs *fs, uint32_t dir_mft, const char *name, int is_dir, uint32_t *out_mft);
int ntfs_write(struct ntfs_fs *fs, struct ntfs_file *f, uint32_t dir_mft, const char *name,
               uint64_t off, const void *buf, uint32_t len);
int ntfs_sync(struct ntfs_fs *fs, struct ntfs_file *f, uint32_t dir_mft, const char *name);
int ntfs_truncate(struct ntfs_fs *fs, struct ntfs_file *f, uint32_t dir_mft, const char *name, uint64_t new_size);
int ntfs_remove(struct ntfs_fs *fs, uint32_t dir_mft, const char *name);
const char *ntfs_strerror(int err);

#endif
