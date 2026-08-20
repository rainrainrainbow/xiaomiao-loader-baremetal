# Xiaomiao Loader — Bare-metal Edition

学而思小喵掌机 ROM 选择器固件的**全裸机重构版**。

- ✅ **无 LVGL**（省 ~120-180KB）
- ✅ **无 fatfs/VFS**（自制 FAT32 只读解析，省 ~20-30KB）
- ✅ **无 nvs_flash**（自制状态块，省 ~15-25KB）
- ✅ **无 app_update/esp_ota**（esp_partition 直写 + 手写 otadata，省 ~15-25KB）
- ✅ **体积：无 WiFi 版 app 226KB ✅（factory 256KB + launcher 扩充至 2432KB），WiFi 版用独立分区表（factory 1.125MB）**
- 📶 **可选 WiFi OTA 更新**（Kconfig 开关，凭据从 SD 卡 wifi.conf 读取）
- 🔑 **WiFi 凭据不写死**——在 SD 卡根目录放 `wifi.conf` 即可

## 目录结构

```
main/
├── main.c             # 入口 + 按键 + 主循环
├── bm_config.h        # 编译期配置（引脚/分区/常量）
├── bm_st7735.c/h      # ST7735 寄存器驱动 + RGB565 帧缓冲
├── bm_font5x7.h       # 5x7 ASCII 点阵字体（~0.7KB）
├── bm_ui.c/h          # 极简文本 UI（列表/进度/信息）
├── bm_sd.c/h          # SD 卡 + FAT32 只读解析
├── bm_flash.c/h       # 状态块 + ROM 写入 + otadata
├── bm_rom.c/h         # ROM 解析（app-only/merged/.img）
├── bm_wifi_ota.c/h    # 【可选】WiFi OTA（凭据从 SD 卡读取）
└── Kconfig.projbuild  # WiFi 开关
```

## 按键

| 按键 | 功能 |
|------|------|
| ↑/↓ | 选择 ROM |
| A | 加载选中 ROM |
| ← | About 信息页 |
| B | 返回列表 |
| → (+A+B) | 进入 WiFi OTA（仅 WiFi 版） |

## 构建

```bash
# 需要 ESP-IDF v5.5+
./build.sh
```

- 默认（无 WiFi）：`app 226KB` 🎯 factory 分区 256KB（0x40000）完全够用
- 开 WiFi：`app 673KB`，需 `partitions_wifi.csv`（factory 1.125MB）

## 烧录

```bash
idf.py -p /dev/ttyACM0 flash
# 或
esptool.py --chip esp32 -b 460800 write_flash 0x0 build/xiaomiao-loader-merged.bin
```

## 📡 WiFi OTA 配置（SD 卡方式）

WiFi 凭据**不写死在固件中**。在 SD 卡根目录创建 `wifi.conf` 文件：

```ini
SSID=MyWiFi
PASS=MyPassword
HOST=192.168.1.100
PORT=8080
PATH=/latest.bin
```

- `SSID` / `PASS` — WiFi 名称和密码
- `HOST` — OTA 服务器 IP 或域名
- `PORT` — 端口（默认 80）
- `PATH` — HTTP 下载路径
- 支持 `#` 注释行和空行
- 保存后插回掌机，按 `A+B` 同时按 `→` 键启动 WiFi OTA

## WiFi OTA 服务器

任意 HTTP 静态服务器即可：

```bash
# 放一个 ROM 的 .bin 到目录，然后
python3 -m http.server 8080 --directory /opt/roms
```

Loader 会 GET `http://<HOST>:8080/latest.bin`，下载后写入 ota_0 并重启。

## 与上游对比

| | 上游 (LVGL) | 本版 (Bare-metal) |
|---|---|---|
| GUI | LVGL 9.5 | 自制 5x7 字体 |
| 文件系统 | fatfs | 自制 FAT32 解析 |
| NVS | nvs_flash | 自制状态块 |
| OTA | esp_ota | esp_partition + otadata |
| WiFi 凭据 | 无 WiFi | SD 卡 wifi.conf 文件 |
| **app 体积** | ~300-450KB | **226KB（无 WiFi）** |
| factory 分区 | 568KB（0x8E000） | **256KB（0x40000）✅ 缩小 55%** |
| launcher 分区 | 2.12MB（0x220000） | **2.375MB（0x260000）✅ 扩容 256KB** |
| WiFi | - | ✅ 可选（~673KB，需独立分区表） |