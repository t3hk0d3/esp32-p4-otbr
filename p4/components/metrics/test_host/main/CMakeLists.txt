idf_component_register(SRCS "test_writer.c" "test_render.c" "sample_input.c"
                       REQUIRES metrics diag routing unity
                       WHOLE_ARCHIVE)
# tools/tests/test_metrics_format.py parses the samples written here with prometheus_client.
# sample.prom renders with routing method bgp, sample-ripng.prom with ripng (the two exclude each other's series).
target_compile_definitions(${COMPONENT_LIB} PRIVATE METRICS_SAMPLE_PATH="${CMAKE_BINARY_DIR}/sample.prom"
                                                    METRICS_SAMPLE_RIPNG_PATH="${CMAKE_BINARY_DIR}/sample-ripng.prom")
