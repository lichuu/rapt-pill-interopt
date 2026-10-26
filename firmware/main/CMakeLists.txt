idf_component_register(SRCS "app_main.c" "sensors.c" "pill_net.c" "pill_ble.c"
                       "pill_ota.c" "pill_upload.c" "pill_settings.c" "pill_mqtt.c"
                       INCLUDE_DIRS "."
                       REQUIRES driver bt nvs_flash esp_timer esp_http_server
                                  esp_wifi esp_netif json app_update
                                  esp_http_client mbedtls mqtt esp-tls)
