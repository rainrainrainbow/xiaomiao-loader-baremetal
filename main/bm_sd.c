/*
 * bm_sd.c — SD 卡（sdspi）初始化 + FAT32 只读文件系统
 *
 * 不依赖 esp_vfs_fat / fatfs：直接通过 sdspi_host 读扇区，
 * 自解析 FAT32 BPB 与目录链。仅实现 ROM 加载所需子集：
 *   - 打开根目录 / 子目录
 *   - 枚举 *.bin / *.img
 *   - 顺序读文件 + seek
 */
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "driver/gpio.h"
#include "driver/sdspi_host.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "sdmmc_cmd.h"
#include "soc/soc_caps.h"

#include "bm_config.h"
#include "bm_sd.h"

#define TAG "bm_sd"

static sdmmc_card_t s_card_inst;   /* 卡实例（sdmmc_card_init 填充） */
static sdmmc_card_t *s_card = &s_card_inst;

/* ── 底层扇区读 ────────────────────────────────────────── */
static bool sd_read_sector(uint32_t sector, uint8_t *dst)
{
    if (!s_card || sector >= s_card->csd.capacity) {
        return false;
    }
    return sdmmc_read_sectors(s_card, dst, sector, 1) == ESP_OK;
}

/* 前置声明（bm_file_read / bm_file_seek 使用，定义在文件后部） */
static uint32_t fat_get_next_sd(bm_file_t *bf, uint32_t cluster);
static uint32_t cluster_to_sector_sd(bm_file_t *bf, uint32_t cluster);

/* ── FAT32 帮助函数 ────────────────────────────────────── */
static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

/* 取 FAT 表项（簇号 → 下一簇或 EOC） */
static uint32_t fat_get_next(bm_sd_t *sd, uint32_t cluster)
{
    uint8_t buf[512];
    uint32_t fat_off = cluster * 4;
    uint32_t sector = sd->fat_start + fat_off / 512;
    if (!sd_read_sector(sector, buf)) {
        return 0x0FFFFFF7;    /* bad cluster */
    }
    uint32_t val = le32(buf + (fat_off % 512)) & 0x0FFFFFFF;
    return val;
}

/* 簇号 → 绝对扇区 */
static uint32_t cluster_to_sector(bm_sd_t *sd, uint32_t cluster)
{
    return sd->data_start + (uint64_t)(cluster - 2) * sd->cluster_size;
}

/* ── 公开 API ──────────────────────────────────────────── */

bool bm_sd_init(bm_sd_t *sd)
{
    memset(sd, 0, sizeof(*sd));
    sd->mounted = false;

    /* SPI 总线已被 LCD 初始化，直接添加 SD 设备 */
    sdspi_device_config_t slot_cfg = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_cfg.host_id = LCD_SPI_HOST;
    slot_cfg.gpio_cs = PIN_SD_CS;
    slot_cfg.gpio_int = SDSPI_SLOT_NO_INT;

    sdspi_dev_handle_t sd_handle = 0;
    esp_err_t err = sdspi_host_init_device(&slot_cfg, &sd_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "sdspi init dev failed: %s", esp_err_to_name(err));
        return false;
    }

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = sd_handle;
    host.max_freq_khz = SD_SPI_MAX_FREQ_KHZ;

    err = sdmmc_card_init(&host, s_card);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "sdmmc_card_init failed: %s", esp_err_to_name(err));
        return false;
    }

    sd->mounted = true;
    sd->sector_count = s_card->csd.capacity;
    strncpy(sd->name, (const char *)s_card->cid.name, sizeof(sd->name) - 1);
    ESP_LOGI(TAG, "SD: %s sectors=%lu", sd->name, (unsigned long)sd->sector_count);

    /* 读 BPB */
    uint8_t bpb[512];
    if (!sd_read_sector(0, bpb)) {
        ESP_LOGE(TAG, "read BPB failed");
        sd->mounted = false;
        return false;
    }
    uint16_t bytes_per_sector = le16(bpb + 11);
    uint8_t  sectors_per_cluster = bpb[13];
    uint16_t reserved = le16(bpb + 14);
    uint8_t  num_fats = bpb[16];
    uint32_t sectors_per_fat = le32(bpb + 36);
    uint32_t root_cluster = le32(bpb + 44);

    if (bytes_per_sector != 512 || sectors_per_cluster == 0) {
        ESP_LOGE(TAG, "unsupported BPB: bps=%u spc=%u",
                 bytes_per_sector, sectors_per_cluster);
        sd->mounted = false;
        return false;
    }

    sd->fat_start = reserved;
    sd->fat_sectors = sectors_per_fat;
    sd->root_cluster = root_cluster;
    sd->data_start = reserved + num_fats * sectors_per_fat;
    sd->cluster_size = sectors_per_cluster;

    ESP_LOGI(TAG, "FAT32: fat@%lu (%lu sec) root=%lu data@%lu spc=%u",
             (unsigned long)sd->fat_start, (unsigned long)sd->fat_sectors,
             (unsigned long)sd->root_cluster, (unsigned long)sd->data_start,
             sectors_per_cluster);
    return true;
}

/* ── 目录枚举 ──────────────────────────────────────────── */

/* 目录项：如果 name 匹配 *.bin / *.img 且非目录，返回文件大小；否则 0 */
static uint32_t dir_entry_match(const uint8_t *e, char *out_name, size_t out_sz)
{
    /* 空闲项或结束 */
    if (e[0] == 0x00 || e[0] == 0xE5) {
        return 0;
    }
    uint8_t attr = e[11];
    if (attr & 0x18) {   /* volume label / directory */
        return 0;
    }
    /* 8.3 短名 */
    char name8[9];
    char ext[4];
    memcpy(name8, &e[0], 8);
    memcpy(ext, &e[8], 3);
    name8[8] = 0;
    ext[3] = 0;
    /* 去空格 */
    char *p = name8 + 7;
    while (p >= name8 && *p == ' ') *p-- = 0;
    p = ext + 2;
    while (p >= ext && *p == ' ') *p-- = 0;

    if (ext[0] == 0) {
        return 0;   /* 无扩展名 */
    }
    /* 只关心 .bin/.img（不区分大小写） */
    if (strcasecmp(ext, "BIN") != 0 && strcasecmp(ext, "IMG") != 0) {
        return 0;
    }

    if (out_name && out_sz > 0) {
        if (name8[0]) {
            snprintf(out_name, out_sz, "%s.%s", name8, ext);
        } else {
            snprintf(out_name, out_sz, ".%s", ext);
        }
    }
    return le32(&e[28]);   /* 文件大小 */
}

/* 遍历簇链，找 /boot 下所有 ROM */
static void scan_cluster_chain(bm_sd_t *sd, uint32_t start_cluster,
                               char names[][64], int max, int *count)
{
    uint32_t cluster = start_cluster;
    uint8_t buf[512];
    int guard = 0;

    while (cluster >= 2 && cluster < 0x0FFFFFF8 && guard++ < 4096) {
        uint32_t sector = cluster_to_sector(sd, cluster);
        for (uint32_t i = 0; i < sd->cluster_size && *count < max; i++) {
            if (!sd_read_sector(sector + i, buf)) {
                return;
            }
            for (int off = 0; off < 512; off += 32) {
                const uint8_t *e = buf + off;
                if (e[0] == 0x00) {
                    return;   /* 目录结束 */
                }
                char tmp[64];
                if (dir_entry_match(e, tmp, sizeof(tmp))) {
                    if (*count < max) {
                        snprintf(names[*count], 64, "%s", tmp);
                        (*count)++;
                    }
                }
            }
        }
        cluster = fat_get_next(sd, cluster);
    }
}

int bm_sd_list_roms(bm_sd_t *sd, char names[][64], int max)
{
    int count = 0;
    /* /boot 目录：从根目录查找 "BOOT" 项，再到其簇链 */
    uint8_t buf[512];
    uint32_t cluster = sd->root_cluster;
    uint32_t boot_cluster = 0;
    int guard = 0;

    while (cluster >= 2 && cluster < 0x0FFFFFF8 && guard++ < 4096) {
        uint32_t sector = cluster_to_sector(sd, cluster);
        for (uint32_t i = 0; i < sd->cluster_size; i++) {
            if (!sd_read_sector(sector + i, buf)) {
                return count;
            }
            for (int off = 0; off < 512; off += 32) {
                const uint8_t *e = buf + off;
                if (e[0] == 0x00) {
                    /* 找完 /boot 如果还没找到就直接返回（可能卡里没 /boot） */
                    if (boot_cluster) {
                        scan_cluster_chain(sd, boot_cluster, names, max, &count);
                        return count;
                    }
                    return count;
                }
                if (e[0] != 0xE5 && !(e[11] & 0x18)) {
                    char n8[9]; char ext[4];
                    memcpy(n8, &e[0], 8); memcpy(ext, &e[8], 3);
                    n8[8] = 0; ext[3] = 0;
                    char *p = n8 + 7;
                    while (p >= n8 && *p == ' ') *p-- = 0;
                    p = ext + 2;
                    while (p >= ext && *p == ' ') *p-- = 0;
                    /* 目录 "BOOT"（无扩展名）且为目录 */
                    if ((e[11] & 0x10) && n8[0] && strcasecmp(n8, "BOOT") == 0 && ext[0] == 0) {
                        boot_cluster = le16(&e[26]) | (le32(&e[20]) << 16);
                        /* 立即扫描 */
                        scan_cluster_chain(sd, boot_cluster, names, max, &count);
                        return count;
                    }
                }
            }
        }
        cluster = fat_get_next(sd, cluster);
    }
    return count;
}

/* ── 文件 API ──────────────────────────────────────────── */

/* 在指定簇链中查找文件，返回起始簇 */
static uint32_t find_file_in_chain(bm_sd_t *sd, uint32_t start_cluster,
                                   const char *name, uint32_t *size_out,
                                   uint8_t *first_sector_buf)
{
    uint32_t cluster = start_cluster;
    uint8_t buf[512];
    int guard = 0;

    while (cluster >= 2 && cluster < 0x0FFFFFF8 && guard++ < 4096) {
        uint32_t sector = cluster_to_sector(sd, cluster);
        for (uint32_t i = 0; i < sd->cluster_size; i++) {
            if (!sd_read_sector(sector + i, buf)) {
                return 0;
            }
            for (int off = 0; off < 512; off += 32) {
                const uint8_t *e = buf + off;
                if (e[0] == 0x00) {
                    return 0;
                }
                if (e[0] == 0xE5 || (e[11] & 0x18)) {
                    continue;
                }
                char n8[9]; char ext[4];
                memcpy(n8, &e[0], 8); memcpy(ext, &e[8], 3);
                n8[8] = 0; ext[3] = 0;
                char *p = n8 + 7;
                while (p >= n8 && *p == ' ') *p-- = 0;
                p = ext + 2;
                while (p >= ext && *p == ' ') *p-- = 0;
                char full[16];
                if (ext[0]) {
                    snprintf(full, sizeof(full), "%s.%s", n8, ext);
                } else {
                    snprintf(full, sizeof(full), "%s", n8);
                }
                if (strcasecmp(full, name) == 0) {
                    if (first_sector_buf) {
                        memcpy(first_sector_buf, buf, 512);
                    }
                    *size_out = le32(&e[28]);
                    return le16(&e[26]) | (le32(&e[20]) << 16);
                }
            }
        }
        cluster = fat_get_next(sd, cluster);
    }
    return 0;
}

bool bm_file_open(bm_sd_t *sd, const char *path, bm_file_t *bf)
{
    /* path 形如 "/boot/foo.bin"，我们只取最后一段 */
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;

    memset(bf, 0, sizeof(*bf));
    uint32_t size = 0;
    uint8_t sbuf[512];
    uint32_t first = find_file_in_chain(sd, sd->root_cluster, base, &size, sbuf);
    if (!first) {
        ESP_LOGE(TAG, "file not found: %s", base);
        return false;
    }

    bf->first_cluster = first;
    bf->cluster = first;
    bf->sector_in_cluster = 0;
    bf->cluster_size = sd->cluster_size;
    bf->fat_start = sd->fat_start;
    bf->data_start = sd->data_start;
    bf->file_size = size;
    bf->pos = 0;
    bf->current_sector = cluster_to_sector(sd, first);

    /* 预读首个扇区 */
    if (!sd_read_sector(bf->current_sector, bf->buf)) {
        ESP_LOGE(TAG, "read first sector failed");
        return false;
    }
    return true;
}

size_t bm_file_read(bm_file_t *bf, void *dst, size_t size)
{
    if (bf->pos >= bf->file_size) {
        return 0;
    }
    size_t remaining = bf->file_size - bf->pos;
    if (size > remaining) {
        size = remaining;
    }

    uint8_t *out = (uint8_t *)dst;
    size_t done = 0;
    while (done < size) {
        uint32_t in_sector = bf->pos % 512;
        uint32_t avail = 512 - in_sector;
        size_t want = size - done;
        if (want > avail) {
            want = avail;
        }

        memcpy(out + done, bf->buf + in_sector, want);
        done += want;
        bf->pos += (uint32_t)want;

        if (bf->pos % 512 == 0 && bf->pos < bf->file_size) {
            /* 推进扇区/簇 */
            bf->sector_in_cluster++;
            if (bf->sector_in_cluster >= bf->cluster_size) {
                bf->cluster = fat_get_next_sd(bf, bf->cluster);
                if (bf->cluster < 2) {
                    break;
                }
                bf->sector_in_cluster = 0;
            }
            bf->current_sector = cluster_to_sector_sd(bf, bf->cluster) +
                                 bf->sector_in_cluster;
            if (!sd_read_sector(bf->current_sector, bf->buf)) {
                break;
            }
        }
    }
    return done;
}

/* 便捷：用 bm_file_t 自带的 fat 元数据取下一簇 */
uint32_t fat_get_next_sd(bm_file_t *bf, uint32_t cluster)
{
    uint8_t buf[512];
    uint32_t fat_off = cluster * 4;
    uint32_t sector = bf->fat_start + fat_off / 512;
    if (!sd_read_sector(sector, buf)) {
        return 0x0FFFFFF7;
    }
    return le32(buf + (fat_off % 512)) & 0x0FFFFFFF;
}

uint32_t cluster_to_sector_sd(bm_file_t *bf, uint32_t cluster)
{
    return bf->data_start + (uint64_t)(cluster - 2) * bf->cluster_size;
}

bool bm_file_seek(bm_file_t *bf, uint32_t off)
{
    if (off > bf->file_size) {
        return false;
    }
    /* 重置到文件头再前进 */
    bf->pos = 0;
    bf->cluster = bf->first_cluster;
    bf->sector_in_cluster = 0;
    bf->current_sector = cluster_to_sector_sd(bf, bf->cluster);
    sd_read_sector(bf->current_sector, bf->buf);

    uint32_t target = off;
    while (target > 0) {
        uint32_t in_sector = bf->pos % 512;
        uint32_t avail = 512 - in_sector;
        uint32_t step = target > avail ? avail : target;
        bf->pos += step;
        target -= step;
        if (bf->pos % 512 == 0 && bf->pos < bf->file_size) {
            bf->sector_in_cluster++;
            if (bf->sector_in_cluster >= bf->cluster_size) {
                bf->cluster = fat_get_next_sd(bf, bf->cluster);
                if (bf->cluster < 2) {
                    return false;
                }
                bf->sector_in_cluster = 0;
            }
            bf->current_sector = cluster_to_sector_sd(bf, bf->cluster) +
                                 bf->sector_in_cluster;
            sd_read_sector(bf->current_sector, bf->buf);
        }
    }
    return true;
}