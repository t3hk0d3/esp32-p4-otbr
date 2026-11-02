idf_component_register(SRCS "test_migrator_core.c" "../../main/migrator_core.c"
                       INCLUDE_DIRS "../../main"
                       REQUIRES c6_bundle c6_board unity)

# On the linux target, libtfpsacrypto references the allocator in mbedtls/port/esp_mem.c, but the archive holding it comes
# first on the link line, so the linker never pulls it in. Force both symbols in.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u esp_mbedtls_mem_calloc" "-u esp_mbedtls_mem_free")
