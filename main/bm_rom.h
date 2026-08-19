/*
 * bm_rom.h — ROM 文件识别与解析（app-only / merged bin / retro-go .img）
 */
#ifndef BM_ROM_H
#define BM_ROM_H

#include <stdbool.h>
#include <stdint.h>

#include "bm_sd.h"

#define ESP_IMAGE_MAGIC  0xE9
#define APP_OFFSET_MERGED 0x10000
#define IMG_PT_OFFSET     0x9000
#define IMG_PT_OFFSET_LEGACY 0x8000

typedef enum {
    ROM_SINGLE = 0,
    ROM_DUAL_IMG,
} rom_type_t;

typedef struct {
    char     name[64];            /* 显示名（不含扩展名） */
    char     path[280];           /* /boot/xxx.bin */
    uint32_t file_size;
    uint32_t app_offset;          /* 0=app-only, 0x10000=merged */
    uint32_t app_size;
    bool     valid;
    rom_type_t type;
    uint32_t img_launcher_off, img_launcher_size;
    uint32_t img_core_off,    img_core_size;
} rom_entry_t;

/* 解析一个已打开的文件，填充 rom_entry（不含 name/path，由调用方填） */
bool bm_rom_parse(bm_sd_t *sd, bm_file_t *f, rom_entry_t *r);

/* 计算一个 app 镜像在文件内的总大小（header+segs+pad+crc+hash） */
uint32_t bm_rom_calc_app_size(bm_file_t *f, uint32_t offset);

#endif /* BM_ROM_H */