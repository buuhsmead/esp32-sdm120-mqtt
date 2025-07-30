set(PROJECT_NAME "sdm120-mqtt")


idf_component_register(SRCS "sdm120-app.c"
        PRIV_REQUIRES mqtt esp_wifi nvs_flash esp_netif esp_event
                        INCLUDE_DIRS ".")
                        