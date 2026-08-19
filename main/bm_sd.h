/*
 * bm_sd.h — SD 卡（sdspi）初始化 + FAT32 只读文件枚举/读取
 * 不含 VFS / fatfs，直接通过 sdspi_host 读扇区，自解析 FAT32。
 */
#ifndef BM_SD_H
#define BM_SD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* FAT32 只读文件句柄 */
typedef struct {
    uint32_t first_cluster;   /* 文件起始簇 */
    uint32_t cluster;         /* 当前簇 */
    uint32_t sector_in_cluster;
    uint32_t cluster_size;    /* 每簇扇区数 */
    uint32_t fat_start;       /* FAT 表起始扇区 */
    uint32_t data_start;      /* 数据区起始扇区 */
    uint32_t current_sector;  /* 当前要读的绝对扇区（含 SD 偏移） */
    uint32_t file_size;
    uint32_t pos;             /* 当前字节偏移 */
    uint8_t  buf[512];
} bm_file_t;

/* 卡结构 */
typedef struct {
    bool mounted;
    char name[16];
    uint32_t sector_count;
    /* FAT32 元数据 */
    uint32_t fat_start;
    uint32_t fat_sectors;
    uint32_t root_cluster;
    uint32_t data_start;      /* 数据区起始扇区 = fat_start + fat_sectors*2 */
    uint32_t cluster_size;    /* 扇区数/簇 */
} bm_sd_t;

/* 初始化：SPI + sdspi，尝试挂载 */
bool bm_sd_init(bm_sd_t *sd);

/* 打开一个文件（bf 由调用方提供） */
bool bm_file_open(bm_sd_t *sd, const char *path, bm_file_t *bf);

/* 从偏移 pos 读取 size 字节到 dst，返回实际读取数 */
size_t bm_file_read(bm_file_t *bf, void *dst, size_t size);

/* seek 到绝对偏移 */
bool bm_file_seek(bm_file_t *bf, uint32_t off);

/* 列出 /boot 目录下 .bin 与 .img 文件，填充 names（最多 max） */
int bm_sd_list_roms(bm_sd_t *sd, char names[][64], int max);

#endif /* BM_SD_H */