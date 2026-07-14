idf_component_register(SRCS "spindoctor_gateway.c"
                    INCLUDE_DIRS "."
                    REQUIRES nvs_flash esp_wifi esp_event esp_netif driver json esp_http_client esp-tls)
