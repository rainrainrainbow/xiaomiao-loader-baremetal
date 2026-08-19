/*
 * bm_wifi_ota.h — WiFi OTA（可选编译，CONFIG_XIAOMIAO_ENABLE_WIFI）
 * 通过 lwip BSD socket 直接 HTTP GET，流式写入 ota_0。
 * WiFi 凭据从 SD 卡 /boot/wifi.conf 文件读取，不写死在固件中。
 */
#ifndef BM_WIFI_OTA_H
#define BM_WIFI_OTA_H

#include <stdbool.h>
#include <stdint.h>

#include "bm_sd.h"

#ifdef __cplusplus
extern "C" {
#endif

/* WiFi 配置（从 wifi.conf 解析） */
typedef struct {
    char     ssid[33];       /* SSID 最大 32 字节 + NUL */
    char     pass[65];       /* 密码最大 64 字节 + NUL */
    char     host[128];      /* 服务器主机名或 IP */
    uint16_t port;           /* 端口号 */
    char     path[256];      /* HTTP 路径，如 /latest.bin */
    bool     valid;          /* 解析成功标志 */
} bm_wifi_config_t;

/* 从 SD 卡根目录 /wifi.conf 加载 WiFi 配置。
 * sd 必须已初始化挂载。返回 true 表示解析成功，cfg->valid 为 true。
 * wifi.conf 格式（每行一个，KEY=VALUE，忽略 # 注释和空行）：
 *   SSID=MyWiFi
 *   PASS=MyPassword
 *   HOST=192.168.1.100
 *   PORT=8080
 *   PATH=/latest.bin
 */
bool bm_wifi_load_config(bm_sd_t *sd, bm_wifi_config_t *cfg);

/* 触发 WiFi 更新流程：
 *  - 连接 WiFi（cfg 中的凭据，重试 3 次）
 *  - HTTP GET cfg->host:port + cfg->path
 *  - 解析响应头，流式下载写 ota_0
 *  - 成功后写 otadata 并返回 true（调用方重启）
 *  - progress(pct) 由调用方用于 UI 刷新
 * 返回 true=成功（调用方应重启）。
 */
bool bm_wifi_ota_update(const bm_wifi_config_t *cfg,
                        void (*progress)(int pct));

#ifdef __cplusplus
}
#endif

#endif /* BM_WIFI_OTA_H */