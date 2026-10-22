idf_component_register(SRCS "main.c" "captive_dns.c" "tx_tune.c" "radio_verdict.c"
                            "oled_ssd1306.c" "font5x7_glcd.c" "airports.c"
                       INCLUDE_DIRS "."
                       REQUIRES nvs_flash
                                esp_wifi
                                esp_netif
                                esp_event
                                esp_http_client
                                esp_http_server
                                lwip
                                esp_https_ota
                                app_update
                                esp-tls
                                esp_timer
                                esp_hw_support
                                esp_driver_gpio
                                esp_driver_rmt
                                esp_driver_i2c
                                esp_lcd
                                efuse
                                espcoredump
                                mbedtls)
