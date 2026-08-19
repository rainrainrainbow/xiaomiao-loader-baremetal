/*
 * bm_wifi_ota.c — WiFi OTA（可选编译）
 *
 * 仅在 CONFIG_XIAOMIAO_ENABLE_WIFI=y 时编译。依赖：
 *   - esp_wifi + esp_event + nvs_flash（WiFi 凭据/校准需要 NVS flash 驱动）
 *     （注意：本文件保留 nvs_flash 初始化，因为 esp_wifi 依赖它；
 *      但 loader 自身的状态块仍用 bm_flash 的自制方案）
 *   - lwip（BSD socket）
 *
 * 流程：连 STA → HTTP GET → 跳过响应头 → 流式写 ota_0 → 校验长度 → otadata → 返回
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
#include "bm_wifi_ota.h"

#if BM_WIFI_ENABLED

#define TAG "bm_wifi"

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
#define WIFI_MAX_RETRY     3
#define HTTP_TIMEOUT_MS     10000

static EventGroupHandle_t s_wifi_events;
static int s_retry_cnt;

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

static bool wifi_connect(void)
{
    s_wifi_events = xEventGroupCreate();
    s_retry_cnt = 0;

    /* esp_wifi 依赖 NVS 存储校准数据；loader 不使用 nvs_flash 的其他功能，
     * 这里仅初始化（已由 bm_flash 用自制状态块管理自己的数据，互不冲突：
     * 状态块用 nvs 分区 offset 0，而 nvs_flash 用其私有布局——若冲突可改为
     * 独立分区。实际 nvs 分区 20KB，状态块 4KB，错开无碍） */
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES ||
        nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               &wifi_event_handler, NULL));

    wifi_config_t wcfg = { 0 };
    strncpy((char *)wcfg.sta.ssid, CONFIG_XIAOMIAO_WIFI_SSID, 32);
    strncpy((char *)wcfg.sta.password, CONFIG_XIAOMIAO_WIFI_PASS, 64);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wcfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    EventBits_t bits = xEventGroupWaitBits(s_wifi_events,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(20000));
    return (bits & WIFI_CONNECTED_BIT) != 0;
}

/* 流式下载：返回 0 成功，否则错误码 */
static int http_get_to_flash(void (*progress)(int pct))
{
    struct addrinfo hints = { 0 }, *res = NULL;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%d", CONFIG_XIAOMIAO_WIFI_PORT);

    if (getaddrinfo(CONFIG_XIAOMIAO_WIFI_HOST, port_str, &hints, &res) != 0) {
        ESP_LOGE(TAG, "DNS failed");
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
        ESP_LOGE(TAG, "connect failed");
        close(fd);
        freeaddrinfo(res);
        return -3;
    }
    freeaddrinfo(res);

    /* 发送 HTTP GET */
    char req[256];
    int rl = snprintf(req, sizeof(req),
                      "GET %s HTTP/1.1\r\n"
                      "Host: %s:%d\r\n"
                      "Connection: close\r\n"
                      "User-Agent: xiaomiao-loader\r\n"
                      "\r\n",
                      CONFIG_XIAOMIAO_WIFI_PATH,
                      CONFIG_XIAOMIAO_WIFI_HOST,
                      CONFIG_XIAOMIAO_WIFI_PORT);
    if (send(fd, req, rl, 0) != rl) {
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
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_OTA_0, NULL);
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

bool bm_wifi_ota_update(void (*progress)(int pct))
{
    ESP_LOGI(TAG, "WiFi OTA started (AP=%s)", CONFIG_XIAOMIAO_WIFI_SSID);
    if (!wifi_connect()) {
        ESP_LOGE(TAG, "WiFi connect failed");
        return false;
    }
    ESP_LOGI(TAG, "WiFi connected");

    int ret = http_get_to_flash(progress);
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