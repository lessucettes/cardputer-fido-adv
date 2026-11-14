#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void esp_fill_random(void *out, size_t len);

#ifdef __cplusplus
}
#endif
