#pragma once
#include "board.h"

namespace cardputer {
enum class BackupMode : unsigned char { None = 0, Save = 1, Restore = 2 };
/* Requests are persisted by a worker after flushing SDK storage, then reboot.
 * Passwords are entered only in the pre-USB maintenance boot, never persisted. */
bool request_backup_mode(BackupMode mode);
void backup_startup(Board& board);
/* Fatal storage errors expose restore only, without starting USB/CTAP. */
[[noreturn]] void backup_recovery(Board& board);
}  // namespace cardputer
