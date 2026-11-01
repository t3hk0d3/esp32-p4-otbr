if(IDF_TARGET STREQUAL "linux")
    # Host tests build the pcap encoder and the capture ring against stand-ins for br_core and the OpenThread types.
    idf_component_register(SRCS "pcap.c" "capture.c"
                           INCLUDE_DIRS "include" "test_host/stubs"
                           PRIV_REQUIRES log esp_timer freertos heap)
    return()
endif()

idf_component_register(SRCS "pcap.c" "capture.c"
                       INCLUDE_DIRS "include"
                       REQUIRES br_core
                       PRIV_REQUIRES openthread esp_timer freertos heap log)
