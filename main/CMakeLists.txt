idf_component_register(
    SRCS "main.c"
         "bm_st7735.c"
         "bm_ui.c"
         "bm_sd.c"
         "bm_flash.c"
         "bm_rom.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES driver esp_lcd esp_driver_spi sdmmc esp_partition spi_flash
                  esp_timer esp_system esp_common freertos heap
                  # WiFi 组件无条件列出以确保头文件可访问；链接器通过 --gc-sections
                  # 自动丢弃未使用的符号，不影响无 WiFi 版体积
                  esp_event esp_wifi esp_netif nvs_flash lwip
)

# WiFi 版：仅在 Kconfig 启用时编译 bm_wifi_ota.c
if(CONFIG_XIAOMIAO_ENABLE_WIFI)
    target_sources(${COMPONENT_LIB} PRIVATE "bm_wifi_ota.c")
endif()

target_link_libraries(${COMPONENT_LIB} PRIVATE m)