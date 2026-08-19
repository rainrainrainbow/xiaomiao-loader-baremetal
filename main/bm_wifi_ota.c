/*
 * bm_wifi_ota.c — WiFi OTA（可选编译）
 *
 * 仅在 CONFIG_XIAOMIAO_ENABLE_WIFI=y 时编译。依赖：
 *   - esp_wifi + esp_event + nvs_flash（WiFi 校准需要 NVS flash 驱动）
 *     （注意：本文件保留 nvs_flash 初始化，因为 esp_wifi 依赖它；
 *      但 loader 自身的状态块仍用 bm_flash 的自制方案）
 *   - lwip（BSD socket）
 *
 * WiFi 凭据从 SD 卡 /boot/wifi.conf 文件读取，不写死在固件中。
 *
 * 流程：加载 wifi.conf → 连 STA → HTTP GET → 跳过响应头 → 流式写 ota_0
 *       → 校验长度 → otadata → 返回
 */
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netdb.h>

#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "bm_config.h"
#include "bm_flash.h"
#include "bm_sd.h"
#include "bm_wifi_ota.h"

#if BM_WIFI_ENABLED

#define TAG "bm_wifi"

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
#define WIFI_MAX_RETRY     3

static EventGroupHandle_t s_wifi_events;
static int s_retry_cnt;

/* ── wifi.conf 解析 ─────────────────────────────────────── */

/* 去除行尾 \r \n，返回实际长度 */
static size_t trim_line(char *s, size_t len)
{
    while (len > 0 && (s[len-1] == '\r' || s[len-1] == '\n' || s[len-1] == ' '))
        s[--len] = 0;
    return len;
}

/* 跳过头部的 BOM（UTF-8 BOM: EF BB BF） */
static const char *skip_bom(const char *s, size_t *len)
{
    if (*len >= 3 && (uint8_t)s[0] == 0xEF &&
                     (uint8_t)s[1] == 0xBB &&
                     (uint8_t)s[2] == 0xBF) {
        *len -= 3;
        return s + 3;
    }
    return s;
}

bool bm_wifi_load_config(bm_sd_t *sd, bm_wifi_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->port = 80;                    /* 默认端口 */
    cfg->valid = false;

    if (!sd->mounted) {
        ESP_LOGE(TAG, "SD not mounted, cannot load wifi.conf");
        return false;
    }

    /* 打开 /wifi.conf（在 SD 卡根目录，不是 /boot/ 下） */
    bm_file_t f;
    if (!bm_file_open(sd, "/wifi.conf", &f)) {
        ESP_LOGW(TAG, "wifi.conf not found on SD card root");
        return false;
    }

    /* 一次性读入缓冲区（最大 4KB，wifi.conf 很小） */
    char buf[4096];
    size_t file_len = f.file_size;
    if (file_len > sizeof(buf) - 1) {
        file_len = sizeof(buf) - 1;
    }
    size_t read = bm_file_read(&f, buf, file_len);
    buf[read] = 0;
    if (read == 0) {
        ESP_LOGE(TAG, "wifi.conf empty");
        return false;
    }

    /* 逐行解析 */
    const char *p = skip_bom(buf, &read);
    size_t remaining = read - (size_t)(p - buf);
    bool has_ssid = false, has_pass = false;
    bool has_host = false, has_path = false;

    while (remaining > 0) {
        /* 找行尾 */
        const char *nl = (const char *)memchr(p, '\n', remaining);
        size_t line_len = nl ? (size_t)(nl - p) : remaining;

        char line[512];
        size_t cpy = line_len < sizeof(line) - 1 ? line_len : sizeof(line) - 1;
        memcpy(line, p, cpy);
        line[cpy] = 0;
        trim_line(line, cpy);

        if (nl) {
            remaining -= (size_t)(nl + 1 - p);
            p = nl + 1;
        } else {
            remaining = 0;
        }

        /* 跳过空行和注释 */
        if (line[0] == 0 || line[0] == '#') {
            continue;
        }

        /* 找 '=' */
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        const char *key = line;
        const char *val = eq + 1;

        /* 去掉 key 尾部空格 */
        {
            char *k = key + strlen(key);
            while (k > key && (k[-1] == ' ' || k[-1] == '\t')) *--k = 0;
        }
        /* 跳过 val 前导空格 */
        while (*val == ' ' || *val == '\t') val++;

        if (strcasecmp(key, "SSID") == 0) {
            strncpy(cfg->ssid, val, sizeof(cfg->ssid) - 1);
            has_ssid = true;
        } else if (strcasecmp(key, "PASS") == 0) {
            strncpy(cfg->pass, val, sizeof(cfg->pass) - 1);
            has_pass = true;
        } else if (strcasecmp(key, "HOST") == 0) {
            strncpy(cfg->host, val, sizeof(cfg->host) - 1);
            has_host = true;
        } else if (strcasecmp(key, "PORT") == 0) {
            unsigned long pv = strtoul(val, NULL, 10);
            if (pv > 0 && pv <= 65535) {
                cfg->port = (uint16_t)pv;
            }
        } else if (strcasecmp(key, "PATH") == 0) {
            strncpy(cfg->path, val, sizeof(cfg->path) - 1);
            has_path = true;
        }
    }

    if (!has_ssid || !has_pass || !has_host || !has_path) {
        ESP_LOGE(TAG, "wifi.conf incomplete: SSID=%d PASS=%d HOST=%d PATH=%d",
                 has_ssid, has_pass, has_host, has_path);
        return false;
    }

    cfg->valid = true;
    ESP_LOGI(TAG, "wifi.conf loaded: SSID=%s HOST=%s PORT=%u PATH=%s",
             cfg->ssid, cfg->host, cfg->port, cfg->path);
    return true;
}

/* ── WiFi 连接 ──────────────────────────────────────────── */

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    (void)arg; (void)base;
    if (id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_cnt < WIFI_MAX_RETRY) {
            s_retry_cnt++;
            esp_wifi_connect();
        } else {
            xEventGroupSetBits(s_wifi_events, WIFI_FAIL_BIT);
        }
    } else if (id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    }
}

static bool wifi_connect(const bm_wifi_config_t *cfg)
{
    s_wifi_events = xEventGroupCreate();
    s_retry_cnt = 0;

    /* esp_wifi 依赖 NVS 存储校准数据；loader 不使用 nvs_flash 的其他功能，
     * 这里仅初始化（与 bm_flash 的自制状态块互不冲突：
     * 状态块用 nvs 分区 offset 0，而 nvs_flash 用其私有布局。
     * 实际 nvs 分区 20KB，状态块 4KB，错开无碍） */
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES ||
        nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wcfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               &wifi_event_handler, NULL));

    wifi_config_t wc = { 0 };
    strncpy((char *)wc.sta.ssid, cfg->ssid, sizeof(wc.sta.ssid) - 1);
    strncpy((char *)wc.sta.password, cfg->pass, sizeof(wc.sta.password) - 1);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());

    EventBits_t bits = xEventGroupWaitBits(s_wifi_events,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(20000));
    return (bits & WIFI_CONNECTED_BIT) != 0;
}

/* ── HTTP 流式下载 ──────────────────────────────────────── */
/* 返回 0 成功，否则错误码 */
static int http_get_to_flash(const bm_wifi_config_t *cfg,
                             void (*progress)(int pct))
{
    struct addrinfo hints = { 0 }, *res = NULL;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", cfg->port);

    if (getaddrinfo(cfg->host, port_str, &hints, &res) != 0) {
        ESP_LOGE(TAG, "DNS failed for %s", cfg->host);
        return -1;
    }

    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) {
        freeaddrinfo(res);
        return -2;
    }
    struct timeval tv = { .tv_sec = 10, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if (connect(fd, res->ai_addr, res->ai_addrlen) != 0) {
        ESP_LOGE(TAG, "connect to %s:%u failed", cfg->host, cfg->port);
        close(fd);
        freeaddrinfo(res);
        return -3;
    }
    freeaddrinfo(res);

    /* 发送 HTTP GET */
    char req[512];
    int rl = snprintf(req, sizeof(req),
                      "GET %s HTTP/1.1\r\n"
                      "Host: %s:%u\r\n"
                      "Connection: close\r\n"
                      "User-Agent: xiaomiao-loader\r\n"
                      "\r\n",
                      cfg->path, cfg->host, cfg->port);
    if (rl <= 0 || rl >= (int)sizeof(req)) {
        ESP_LOGE(TAG, "request too long");
        close(fd);
        return -4;
    }
    if (send(fd, req, (size_t)rl, 0) != rl) {
        ESP_LOGE(TAG, "send failed");
        close(fd);
        return -4;
    }

    /* 读响应头（直到 \r\n\r\n） */
    uint8_t hbuf[2048];
    size_t hlen = 0;
    while (hlen < sizeof(hbuf) - 1) {
        int n = recv(fd, hbuf + hlen, 1, 0);
        if (n <= 0) {
            ESP_LOGE(TAG, "header recv failed");
            close(fd);
            return -5;
        }
        hlen++;
        if (hlen >= 4 &&
            hbuf[hlen-4] == '\r' && hbuf[hlen-3] == '\n' &&
            hbuf[hlen-2] == '\r' && hbuf[hlen-1] == '\n') {
            break;
        }
    }
    hbuf[hlen] = 0;

    /* 检查 HTTP 200 */
    if (memcmp(hbuf, "HTTP/1.", 7) != 0) {
        ESP_LOGE(TAG, "bad response");
        close(fd);
        return -6;
    }
    int status = 0;
    sscanf((char *)hbuf + 9, "%d", &status);
    if (status != 200) {
        ESP_LOGE(TAG, "HTTP status %d", status);
        close(fd);
        return -7;
    }

    /* Content-Length 可选；这里按流式处理，一直读到 EOF（Connection: close） */
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
    if (!part) {
        close(fd);
        return -8;
    }
    esp_partition_erase_range(part, 0, part->size);

    uint8_t chunk[4096];
    uint32_t written = 0;
    int n;
    while ((n = recv(fd, chunk, sizeof(chunk), 0)) > 0) {
        esp_err_t err = esp_partition_write(part, written, chunk, (size_t)n);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "flash write failed: %s", esp_err_to_name(err));
            close(fd);
            return -9;
        }
        written += (uint32_t)n;
        if (progress) {
            progress((int)(written / 1024));   /* 粗略 KB 进度 */
        }
    }
    close(fd);

    if (written == 0) {
        ESP_LOGE(TAG, "no data downloaded");
        return -10;
    }
    ESP_LOGI(TAG, "downloaded %lu bytes", (unsigned long)written);
    return 0;
}

/* ── 公开 API ───────────────────────────────────────────── */

bool bm_wifi_ota_update(const bm_wifi_config_t *cfg,
                        void (*progress)(int pct))
{
    if (!cfg || !cfg->valid) {
        ESP_LOGE(TAG, "invalid WiFi config");
        return false;
    }

    ESP_LOGI(TAG, "WiFi OTA started (AP=%s, HOST=%s:%u, PATH=%s)",
             cfg->ssid, cfg->host, cfg->port, cfg->path);
    if (!wifi_connect(cfg)) {
        ESP_LOGE(TAG, "WiFi connect failed");
        return false;
    }
    ESP_LOGI(TAG, "WiFi connected");

    int ret = http_get_to_flash(cfg, progress);
    if (ret != 0) {
        ESP_LOGE(TAG, "HTTP download failed (%d)", ret);
        esp_wifi_stop();
        return false;
    }

    /* 写 otadata 指向 ota_0 */
    bool ok = bm_ota_set_boot_ota0();
    esp_wifi_stop();
    return ok;
}

#endif /* BM_WIFI_ENABLED */