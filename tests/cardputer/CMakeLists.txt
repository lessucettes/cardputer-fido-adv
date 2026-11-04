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

cardputer_add_cxx_test(cardputer_text
    text_test.cpp
    "${ROOT}/src/cardputer/text.cpp"
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
