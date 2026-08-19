# 基础依赖（无 WiFi 版）
set(BASE_REQUIRES
    driver esp_lcd esp_driver_spi sdmmc esp_partition spi_flash
    esp_timer esp_system esp_common freertos heap
)

# WiFi 依赖（仅启用时添加）
if(CONFIG_XIAOMIAO_ENABLE_WIFI)
    set(WIFI_REQUIRES esp_event esp_wifi esp_netif nvs_flash lwip)
    set(WIFI_SRC "bm_wifi_ota.c")
else()
    set(WIFI_REQUIRES)
    set(WIFI_SRC)
endif()

idf_component_register(
    SRCS "main.c"
         "bm_st7735.c"
         "bm_ui.c"
         "bm_sd.c"
         "bm_flash.c"
         "bm_rom.c"
         ${WIFI_SRC}
    INCLUDE_DIRS "."
    PRIV_REQUIRES ${BASE_REQUIRES} ${WIFI_REQUIRES}
)

target_link_libraries(${COMPONENT_LIB} PRIVATE m)