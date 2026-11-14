/* Behavioral tests for the header-only BackupPassword input buffer.
 *
 * The class is UI input state, not the KDF: it filters non-printable bytes,
 * enforces the 64-byte maximum, supports destructive backspace and clear, and
 * compares in constant time over the fixed buffer. These checks exercise the
 * public interface only.
 */
#include "backup_password.h"

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <type_traits>

using cardputer::BackupPassword;

static void check(bool condition, const char* expression, int line) {
    if (!condition) {
        std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, line, expression);
        std::exit(1);
    }
}

#define CHECK(cond) check((cond), #cond, __LINE__)

static void check_size(const char* what, std::size_t actual,
                       std::size_t expected, int line) {
    if (actual != expected) {
        std::fprintf(stderr, "FAIL %s:%d: %s: got %zu, expected %zu\n",
                     __FILE__, line, what, actual, expected);
        std::exit(1);
    }
}

#define CHECK_SIZE(what, actual, expected) \
    check_size((what), (actual), (expected), __LINE__)

static bool all_zero(const unsigned char* data, std::size_t size) {
    unsigned char value = 0;
    for (std::size_t i = 0; i < size; ++i) {
        value |= data[i];
    }
    return value == 0;
}

static void test_initial_state(void) {
    BackupPassword password;
    CHECK_SIZE("initial size", password.size(), 0);
    CHECK(all_zero(password.data(), CF_BACKUP_PASSWORD_MAX));
    CHECK(password.equals(BackupPassword()));
}

static void test_filtering(void) {
    BackupPassword password;
    const char accepted[] = " ~!Az09";
    password.append(accepted);
    CHECK_SIZE("printable append", password.size(), std::strlen(accepted));
    CHECK(std::memcmp(password.data(), accepted, std::strlen(accepted)) == 0);

    const char filtered[] = {
        'a', static_cast<char>(0x01), 'b', static_cast<char>(0x1F),
        'c', 0x7F, 'd', static_cast<char>(0x80), 'e',
        static_cast<char>(0xFF), 'f', 0};
    BackupPassword only_printable;
    only_printable.append(filtered);
    const char expected[] = "abcdef";
    CHECK_SIZE("filtered size", only_printable.size(), std::strlen(expected));
    CHECK(std::memcmp(only_printable.data(), expected, std::strlen(expected)) ==
          0);

    BackupPassword boundaries;
    const char edges[] = {
        static_cast<char>(0x1F), ' ', '~', 0x7F, static_cast<char>(0x80), 0};
    boundaries.append(edges);
    CHECK_SIZE("filter boundaries", boundaries.size(), 2);
    CHECK(boundaries.data()[0] == ' ');
    CHECK(boundaries.data()[1] == '~');

    BackupPassword nul_terminated;
    const char embedded[] = {'x', 'y', 0, 'z', 0};
    nul_terminated.append(embedded);
    CHECK_SIZE("stops at NUL", nul_terminated.size(), 2);
    CHECK(std::memcmp(nul_terminated.data(), "xy", 2) == 0);

    nul_terminated.append(nullptr);
    CHECK_SIZE("NULL append is a no-op", nul_terminated.size(), 2);
}

static void test_max_length(void) {
    char input[80];
    std::memset(input, 'A', sizeof(input));
    input[sizeof(input) - 1] = 0;

    BackupPassword password;
    password.append(input);
    CHECK_SIZE("maximum length", password.size(), CF_BACKUP_PASSWORD_MAX);
    for (std::size_t i = 0; i < CF_BACKUP_PASSWORD_MAX; ++i) {
        CHECK(password.data()[i] == 'A');
    }

    password.append("BC");
    CHECK_SIZE("append beyond maximum is ignored", password.size(),
               CF_BACKUP_PASSWORD_MAX);
    CHECK(password.data()[CF_BACKUP_PASSWORD_MAX - 1] == 'A');

    password.backspace();
    password.backspace();
    CHECK_SIZE("backspace below maximum", password.size(),
               CF_BACKUP_PASSWORD_MAX - 2);
    password.append("xy");
    CHECK_SIZE("refill to maximum", password.size(), CF_BACKUP_PASSWORD_MAX);
    CHECK(password.data()[CF_BACKUP_PASSWORD_MAX - 2] == 'x');
    CHECK(password.data()[CF_BACKUP_PASSWORD_MAX - 1] == 'y');
    password.append("z");
    CHECK_SIZE("maximum remains capped", password.size(),
               CF_BACKUP_PASSWORD_MAX);
}

static void test_backspace_and_clear(void) {
    BackupPassword password;
    password.backspace();
    CHECK_SIZE("backspace empty", password.size(), 0);

    password.append("abc");
    CHECK_SIZE("append abc", password.size(), 3);
    password.backspace();
    CHECK_SIZE("backspace once", password.size(), 2);
    CHECK(password.data()[2] == 0);
    CHECK(std::memcmp(password.data(), "ab", 2) == 0);

    password.backspace();
    password.backspace();
    CHECK_SIZE("backspace to empty", password.size(), 0);
    CHECK(all_zero(password.data(), CF_BACKUP_PASSWORD_MAX));
    password.backspace();
    CHECK_SIZE("backspace past empty", password.size(), 0);

    password.append("secret");
    CHECK_SIZE("append after empty", password.size(), 6);
    password.clear();
    CHECK_SIZE("clear size", password.size(), 0);
    CHECK(all_zero(password.data(), CF_BACKUP_PASSWORD_MAX));

    password.append("confirmed");
    CHECK_SIZE("append after clear", password.size(), 9);
    CHECK(std::memcmp(password.data(), "confirmed", 9) == 0);
}

static void test_equals(void) {
    BackupPassword first;
    BackupPassword second;
    first.append("correct horse");
    second.append("correct horse");
    CHECK(first.equals(second));
    CHECK(second.equals(first));
    CHECK(first.equals(first));

    second.backspace();
    CHECK(!first.equals(second));
    CHECK(!second.equals(first));

    BackupPassword shorter;
    shorter.append("correct");
    CHECK(!first.equals(shorter));
    CHECK(!shorter.equals(first));

    BackupPassword empty;
    BackupPassword cleared;
    cleared.append("temporary");
    cleared.clear();
    CHECK(empty.equals(cleared));
    CHECK(cleared.equals(empty));

    BackupPassword different;
    different.append("correct horsf");
    CHECK(!first.equals(different));

    /* A differing length must not be hidden by equal leading bytes. */
    BackupPassword prefix;
    prefix.append("correct horse ");
    CHECK(!first.equals(prefix));
}

static void test_api_traits(void) {
    static_assert(!std::is_copy_constructible<BackupPassword>::value,
                  "password buffers must not be copied");
    static_assert(!std::is_copy_assignable<BackupPassword>::value,
                  "password buffers must not be assigned");

    BackupPassword password;
    password.append("a");
    CHECK(password.data() != nullptr);
    CHECK_SIZE("one character", password.size(), 1);
}

int main() {
    test_initial_state();
    test_filtering();
    test_max_length();
    test_backspace_and_clear();
    test_equals();
    test_api_traits();
    std::puts("backup_password_test: all checks passed");
    return 0;
}
