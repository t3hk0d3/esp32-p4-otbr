idf_component_register(SRCS "esp_ot_rcp.c"
                       INCLUDE_DIRS "."
                       REQUIRES c6_board
                       PRIV_REQUIRES esp_event nvs_flash openthread vfs)
