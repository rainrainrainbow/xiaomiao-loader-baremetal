# Xiaomiao Loader — Bare-metal Edition

学而思小喵掌机 ROM 选择器固件的**全裸机重构版**。

- ✅ **无 LVGL**（省 ~120-180KB）
- ✅ **无 fatfs/VFS**（自制 FAT32 只读解析，省 ~20-30KB）
- ✅ **无 nvs_flash**（自制状态块，省 ~15-25KB）
- ✅ **无 app_update/esp_ota**（esp_partition 直写 + 手写 otadata，省 ~15-25KB）
- ✅ **体积目标：无 WiFi 版 app < 200KB**
- 📶 **可选 WiFi OTA 更新**（Kconfig 开关，体积 +100~160KB）

## 目录结构

```
main/
├── main.c             # 入口 + 按键 + 主循环
├── bm_config.h        # 编译期配置（引脚/分区/WiFi 常量）
├── bm_st7735.c/h      # ST7735 寄存器驱动 + RGB565 帧缓冲
├── bm_font5x7.h       # 5x7 ASCII 点阵字体（~0.7KB）
├── bm_ui.c/h          # 极简文本 UI（列表/进度/信息）
├── bm_sd.c/h          # SD 卡 + FAT32 只读解析
├── bm_flash.c/h       # 状态块 + ROM 写入 + otadata
├── bm_rom.c/h         # ROM 解析（app-only/merged/.img）
├── bm_wifi_ota.c/h    # 【可选】WiFi OTA
└── Kconfig.projbuild  # WiFi 开关与配置
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
# 需要 ESP-IDF v5.5+（LVGL 不再需要）
./build.sh
```

- 默认（无 WiFi）：`app < 200KB`（目标）
- 开 WiFi：`idf.py menuconfig → Component config → Xiaomiao Loader → Enable WiFi OTA`

## 烧录

```bash
idf.py -p /dev/ttyACM0 flash
# 或
esptool.py --chip esp32 -b 460800 write_flash 0x0 xiaomiao-loader-merged.bin
```

## WiFi OTA 服务器

任意 HTTP 静态服务器即可：

```bash
# 放一个 ROM 的 .bin 到目录，然后
python3 -m http.server 8080 --directory /opt/roms
```

Loader 会 GET `http://<HOST>:8080/latest.bin`（Kconfig 可改），下载后写入 ota_0 并重启。

> 注意：WiFi 凭据与 URL 是编译期常量（Kconfig），需重新编译烧录。

## 与上游对比

| | 上游 (LVGL) | 本版 (Bare-metal) |
|---|---|---|
| GUI | LVGL 9.5 | 自制 5x7 字体 |
| 文件系统 | fatfs | 自制 FAT32 解析 |
| NVS | nvs_flash | 自制状态块 |
| OTA | esp_ota | esp_partition + otadata |
| **app 体积** | ~300-450KB | **~150-190KB（无 WiFi）** |
| WiFi | - | ✅ 可选