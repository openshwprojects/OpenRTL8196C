/*
 * fat.h - small read-only FAT12/16/32 reader (long file names included),
 * on top of any 512-byte sector reader (USB mass storage, SPI flash, ...)
 */
#ifndef FAT_H
#define FAT_H

#include <stdint.h>

typedef int (*fat_read_fn)(void *ctx, uint32_t lba, uint32_t count, void *buf);

struct fat_fs {
    fat_read_fn read;
    void    *ctx;
    int      type;                 /* 12, 16 or 32 */
    uint32_t part_lba;             /* first sector of the volume */
    uint32_t fat_lba, data_lba, root_lba, root_sectors, root_cluster;
    uint32_t sectors_per_cluster, clusters;
    char     label[12];
};

struct fat_dirent {
    char     name[64];             /* long name if present, else 8.3 */
    uint32_t size, cluster;
    uint8_t  attr;                 /* 0x10 = directory */
};

/* mounts the first FAT partition (MBR) or a partitionless volume */
int fat_mount(struct fat_fs *fs, fat_read_fn read, void *ctx);
/* lists a directory (cluster 0 = root); calls cb for every entry, stops if cb returns nonzero */
int fat_list(struct fat_fs *fs, uint32_t dir_cluster, int (*cb)(const struct fat_dirent *e, void *arg), void *arg);
/* reads up to max bytes of a file starting at its first cluster */
int fat_read_file(struct fat_fs *fs, const struct fat_dirent *e, void *buf, uint32_t max);

#endif
