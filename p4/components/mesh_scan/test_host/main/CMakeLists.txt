idf_component_register(SRCS "test_render.c" "test_scan.c" "test_srp.c" "fake_ops.c" "sample_result.c"
                       REQUIRES mesh_scan unity
                       WHOLE_ARCHIVE)
