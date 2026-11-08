idf_component_register(SRCS "main.c" "light.c" "zigbee_node.c" "repl.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES console esp_app_format esp_timer nvs_flash)
