idf_component_register(SRCS "test_writer.c" "test_render.c" "sample_input.c"
                       REQUIRES metrics diag unity
                       WHOLE_ARCHIVE)
# tools/tests/test_metrics_format.py parses the sample written here with prometheus_client.
target_compile_definitions(${COMPONENT_LIB} PRIVATE METRICS_SAMPLE_PATH="${CMAKE_BINARY_DIR}/sample.prom")
