idf_component_register(
    SRCS "switcher_proto.c" "switcher.c" "breeze_remote.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES lwip json)
