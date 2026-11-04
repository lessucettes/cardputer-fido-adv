#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Prompt kinds shared with the C protocol core. Keep in sync with
 * cardputer::RequestKind; ui.cpp asserts the values at compile time. */
enum {
    CF_PRESENCE_KIND_EXCLUSION = 3,
    CF_PRESENCE_KIND_RESET = 4,
    CF_PRESENCE_KIND_SELECTION = 5
};

/* Called once before random/OTP initialization. */
void cardputer_ui_init(void);
/* Called from the owner loop; nonblocking. */
void cardputer_ui_pump(void);
/* Starts a fresh user-presence prompt and returns its generation. */
uint32_t cardputer_presence_begin(uint32_t timeout_ms, uint8_t operation);
/* 0 confirmed, 1 timeout, 2 cancelled, -1 still pending. */
int cardputer_presence_take(uint32_t generation);
/* Cancels the matching pending prompt (host CTAPHID CANCEL). */
void cardputer_presence_cancel(uint32_t generation);
/* Selects the prompt kind for operations without an OP_MC/OP_GA marker. */
void cardputer_presence_set_kind(int kind);
/* Shows a fatal storage message and halts before USB is installed. */
void cardputer_storage_failure(int status);

#ifdef __cplusplus
}
#endif
