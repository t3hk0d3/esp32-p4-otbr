idf_component_register(SRCS "supervisor.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES board_cores br_core esp_ot_br_server esp_http_server openthread esp_system freertos log)
