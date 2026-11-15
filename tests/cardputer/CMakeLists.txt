cmake_minimum_required(VERSION 3.16)
project(cardputer_native_tests LANGUAGES C CXX)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
option(CARDPUTER_SANITIZERS "Enable address and undefined sanitizers" ON)
enable_testing()
set(ROOT "${CMAKE_CURRENT_LIST_DIR}/../..")

function(cardputer_add_cxx_test name)
    add_executable(${name} ${ARGN})
    target_include_directories(${name} PRIVATE "${ROOT}/src/cardputer")
    target_compile_options(${name} PRIVATE -Wall -Wextra -Werror)
    if(CARDPUTER_SANITIZERS AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${name} PRIVATE
            -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(${name} PRIVATE -fsanitize=address,undefined)
    endif()
    add_test(NAME ${name} COMMAND ${name})
endfunction()

function(cardputer_add_c_test name)
    add_executable(${name} ${ARGN})
    target_include_directories(${name} PRIVATE "${ROOT}/src/cardputer")
    target_compile_options(${name} PRIVATE -Wall -Wextra -Werror)
    if(CARDPUTER_SANITIZERS AND CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${name} PRIVATE
            -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(${name} PRIVATE -fsanitize=address,undefined)
    endif()
    add_test(NAME ${name} COMMAND ${name})
endfunction()

cardputer_add_cxx_test(cardputer_model
    model_test.cpp
    "${ROOT}/src/cardputer/model.cpp"
    "${ROOT}/src/cardputer/settings.cpp"
)

cardputer_add_cxx_test(cardputer_settings
    settings_test.cpp
    "${ROOT}/src/cardputer/settings.cpp"
)

cardputer_add_cxx_test(cardputer_audio
    audio_test.cpp
    "${ROOT}/src/cardputer/audio.cpp"
)
target_include_directories(cardputer_audio PRIVATE "${CMAKE_CURRENT_LIST_DIR}/audio_stub")
# Match the ESP32-S3 char ABI used by the keyboard header's key codes.
target_compile_options(cardputer_audio PRIVATE -funsigned-char)

cardputer_add_cxx_test(cardputer_display
    display_test.cpp
    "${ROOT}/src/cardputer/display.cpp"
)
target_include_directories(cardputer_display PRIVATE "${CMAKE_CURRENT_LIST_DIR}/display_stub")
target_compile_options(cardputer_display PRIVATE -funsigned-char)
add_test(NAME cardputer_display_fallback COMMAND cardputer_display --no-canvas)

cardputer_add_cxx_test(cardputer_text
    text_test.cpp
    "${ROOT}/src/cardputer/text.cpp"
)

cardputer_add_cxx_test(cardputer_view
    view_test.cpp
    "${ROOT}/src/cardputer/model.cpp"
    "${ROOT}/src/cardputer/settings.cpp"
)

cardputer_add_c_test(cardputer_key_store
    key_store_test.c
    fake_flash.c
    "${ROOT}/src/cardputer/key_store.c"
)

cardputer_add_c_test(cardputer_flash_layout
    flash_layout_test.c
    fake_flash.c
    "${ROOT}/src/cardputer/flash_layout.c"
)

cardputer_add_cxx_test(cardputer_presence_bridge
    presence_bridge_test.cpp
    "${ROOT}/src/cardputer/presence_bridge.cpp"
    "${ROOT}/src/cardputer/model.cpp"
    "${ROOT}/src/cardputer/settings.cpp"
    "${ROOT}/src/cardputer/text.cpp"
)

cardputer_add_c_test(cardputer_credential_service
    credential_service_test.c
    "${ROOT}/src/cardputer/credential_service.c"
)

# Password backup tests use the firmware's pinned Mbed TLS sources, with a
# minimal native software configuration instead of ESP hardware adapters.
set(MBEDTLS_DIR "${ROOT}/pico-keys-sdk/third-party/mbedtls")
if(NOT EXISTS "${MBEDTLS_DIR}/library/gcm.c")
    message(FATAL_ERROR "Fetch the pinned SDK dependencies first; see docs/BUILD.md")
endif()
add_library(cardputer_backup_core STATIC
    "${ROOT}/src/cardputer/backup_codec.c"
    "${ROOT}/src/cardputer/backup_store.c"
    "${ROOT}/src/cardputer/key_store.c"
)
foreach(source aes cipher cipher_wrap constant_time gcm md pkcs5 platform_util sha256)
    target_sources(cardputer_backup_core PRIVATE "${MBEDTLS_DIR}/library/${source}.c")
endforeach()
target_include_directories(cardputer_backup_core PUBLIC "${ROOT}/src/cardputer")
target_include_directories(cardputer_backup_core SYSTEM PUBLIC "${MBEDTLS_DIR}/include")
target_compile_definitions(cardputer_backup_core PUBLIC
    MBEDTLS_CONFIG_FILE="${CMAKE_CURRENT_LIST_DIR}/backup_mbedtls_config.h")
target_compile_options(cardputer_backup_core PRIVATE -Wall -Wextra -Werror -O2)
if(CARDPUTER_SANITIZERS AND CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(cardputer_backup_core PRIVATE
        -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(cardputer_backup_core PUBLIC -fsanitize=address,undefined)
endif()

cardputer_add_c_test(cardputer_backup_codec backup_codec_test.c)
target_link_libraries(cardputer_backup_codec PRIVATE cardputer_backup_core)
cardputer_add_c_test(cardputer_backup_store backup_store_test.c)
target_link_libraries(cardputer_backup_store PRIVATE cardputer_backup_core)
cardputer_add_cxx_test(cardputer_backup_password backup_password_test.cpp)
add_executable(cardputer_backup_protocol_tool backup_protocol_tool.c)
target_link_libraries(cardputer_backup_protocol_tool PRIVATE cardputer_backup_core)
target_compile_options(cardputer_backup_protocol_tool PRIVATE -Wall -Wextra -Werror)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME cardputer_backup_oracle
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/backup_codec_test.py"
            "$<TARGET_FILE:cardputer_backup_codec>")
