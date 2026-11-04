# Cardputer ADV integration. Included only when CARDPUTER_ADV is enabled.
set(CARDPUTER_ADV 1 CACHE BOOL "Build the Cardputer ADV port" FORCE)
add_compile_definitions(CARDPUTER_ADV=1)

# M5Unified 0.2.13 has an uninitialized-buffer warning in its optional
# PowerHub RTC code. It is third-party code; keep the warning visible but do
# not let it fail the ADV build.
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-error=maybe-uninitialized")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wno-error=maybe-uninitialized")

list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_SOURCE_DIR}/src/cardputer")
list(APPEND COMPONENTS cardputer)

message(STATUS "Cardputer ADV target:\t enabled")
