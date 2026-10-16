idf_component_register(
    SRCS "tiny_sine.c" "tinyml.c"
    INCLUDE_DIRS "."
    REQUIRES uart esp_timer
)
