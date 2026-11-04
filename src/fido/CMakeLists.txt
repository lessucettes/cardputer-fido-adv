idf_component_register(
    SRCS ${SOURCES}
    INCLUDE_DIRS . ../cardputer
    REQUIRES esp_tinyusb mbedtls efuse pico-keys-sdk cardputer
)
idf_component_set_property(${COMPONENT_NAME} WHOLE_ARCHIVE ON)
