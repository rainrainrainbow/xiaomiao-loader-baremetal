/*
 * bm_wifi_ota.h — WiFi OTA（可选编译，CONFIG_XIAOMIAO_ENABLE_WIFI）
 * 通过 lwip BSD socket 直接 HTTP GET，流式写入 ota_0。
 */
#ifndef BM_WIFI_OTA_H
#define BM_WIFI_OTA_H

#include <stdbool.h>

/* 触发 WiFi 更新流程：
 *  - 连接 WiFi（sdkconfig 凭据，重试 3 次）
 *  - HTTP GET CONFIG_XIAOMIAO_WIFI_HOST:PORT + CONFIG_XIAOMIAO_WIFI_PATH
 *  - 解析响应头，流式下载写 ota_0
 *  - 成功后写 otadata 并返回 true（调用方重启）
 * progress(pct) 由调用方用于 UI 刷新；返回 true=成功。
 */
bool bm_wifi_ota_update(void (*progress)(int pct));

#endif /* BM_WIFI_OTA_H */