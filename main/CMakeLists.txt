idf_component_register(
    SRCS "main.c"
         "bm_st7735.c"
         "bm_ui.c"
         "bm_sd.c"
         "bm_flash.c"
         "bm_rom.c"
         "bm_wifi_ota.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES driver esp_lcd esp_driver_spi sdmmc esp_partition spi_flash
                  esp_timer esp_system esp_common freertos heap
                  # WiFi 版额外组件（Kconfig 开关；未启用时符号未被引用会被
                  # 链接器剔除，不影响无 WiFi 版体积）
                  nvs_flash esp_wifi esp_event esp_netif lwip
)

# 数学库（snprintf 等浮点格式化在 nano 下可用；此处保留以便 CRC 等）
target_link_libraries(${COMPONENT_LIB} PRIVATE m)