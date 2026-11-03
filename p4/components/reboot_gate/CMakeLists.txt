idf_component_register(SRCS "reboot_gate.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_timer esp_system log)
