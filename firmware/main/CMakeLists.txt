# The portable core is compiled straight into the app rather than wrapped in a
# component: it has no IDF dependencies at all, which is what lets the host
# test suite and the simulator compile the same files.
file(GLOB AERIS_CORE_SRC "${CMAKE_CURRENT_LIST_DIR}/../../core/src/*.c")

idf_component_register(
    SRCS
        "main.c"
        "i2c_esp.c"
        "status_led.c"
        "web_server.c"
        "net.c"
        ${AERIS_CORE_SRC}
    INCLUDE_DIRS
        "."
        "../../core/include"
    REQUIRES
        driver
        esp_http_server
        esp_wifi
        esp_netif
        esp_event
        esp_timer
        nvs_flash
        app_update
    EMBED_FILES
        "../../web/dashboard.html"
)

# The core is warning-clean at this level on the host; hold it to the same bar
# on the cross build, where int widths differ.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Wshadow)
