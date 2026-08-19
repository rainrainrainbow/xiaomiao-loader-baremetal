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
)

# WiFi 版：仅在 Kconfig 启用时编译 bm_wifi_ota.c 并链接 WiFi 依赖
if(CONFIG_XIAOMIAO_ENABLE_WIFI)
    target_sources(${COMPONENT_LIB} PRIVATE "bm_wifi_ota.c")
    idf_component_add_requirements(
        PRIV_REQUIRES esp_event esp_wifi esp_netif nvs_flash lwip
    )
    target_link_libraries(${COMPONENT_LIB} PRIVATE
        nvs_flash esp_wifi esp_event esp_netif lwip)
endif()

target_link_libraries(${COMPONENT_LIB} PRIVATE m)