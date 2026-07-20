#include "multipaste/core.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

void expect_equal(const std::wstring& actual, const std::wstring& expected, const char* name) {
    if (actual != expected) {
        std::wcerr << L"FAILED: " << name << L"\nexpected: [" << expected
                   << L"]\nactual:   [" << actual << L"]\n";
        std::exit(1);
    }
}

void expect_true(bool value, const char* name) {
    if (!value) {
        std::cerr << "FAILED: " << name << '\n';
        std::exit(1);
    }
}

} // namespace

int main() {
    using multipaste::HotkeySpec;
    using multipaste::MergeMode;

    expect_equal(multipaste::merge_text(L"hello", L"world", MergeMode::SmartSpace),
                 L"hello world", "smart mode inserts one space");
    expect_equal(multipaste::merge_text(L"hello ", L"world", MergeMode::SmartSpace),
                 L"hello world", "smart mode respects trailing whitespace");
    expect_equal(multipaste::merge_text(L"hello", L"\nworld", MergeMode::SmartSpace),
                 L"hello\nworld", "smart mode respects leading newline");
    expect_equal(multipaste::merge_text(L"鍛戒护", L"--flag", MergeMode::Raw),
                 L"鍛戒护--flag", "raw mode changes nothing");
    expect_equal(multipaste::merge_text(L"", L"text", MergeMode::SmartSpace),
                 L"text", "empty base has no separator");
    expect_equal(multipaste::merge_text(L"text", L"", MergeMode::SmartSpace),
                 L"text", "empty selection has no separator");
    expect_equal(multipaste::merge_text(L"first", L"second", MergeMode::SmartSpace,
                                        L","),
                 L"first,second", "custom separator is inserted");
    expect_equal(multipaste::merge_text(L"first,", L"second", MergeMode::SmartSpace,
                                        L","),
                 L"first,second", "existing trailing separator is not duplicated");
    expect_equal(multipaste::merge_text(L"first", L",second", MergeMode::SmartSpace,
                                        L","),
                 L"first,second", "existing leading separator is not duplicated");
    expect_equal(multipaste::merge_text(L"first", L"second", MergeMode::SmartSpace,
                                        L""),
                 L"firstsecond", "empty custom separator performs raw join");

    std::wstring decoded;
    expect_true(multipaste::decode_separator_expression(L"\\n", decoded),
                "newline expression is valid");
    expect_equal(decoded, L"\n", "newline expression is decoded");
    expect_true(multipaste::decode_separator_expression(L" | \\t", decoded),
                "mixed separator expression is valid");
    expect_equal(decoded, L" | \t", "mixed separator expression is decoded");
    expect_true(multipaste::decode_separator_expression(L"\\\\", decoded),
                "escaped slash is valid");
    expect_equal(decoded, L"\\", "escaped slash is decoded");
    expect_true(!multipaste::decode_separator_expression(L"\\x", decoded),
                "unknown escape is rejected");
    expect_true(!multipaste::decode_separator_expression(L"text\\", decoded),
                "trailing slash is rejected");
    expect_true(multipaste::decode_separator_expression(L"\\s", decoded),
                "space expression is valid");
    expect_equal(decoded, L" ", "space expression decodes correctly");

    expect_true(multipaste::is_hotkey_valid(HotkeySpec{0x0002U | 0x0004U, L'C'}),
                "modified hotkey is valid");
    expect_true(!multipaste::is_hotkey_valid(HotkeySpec{0U, L'C'}),
                "bare letter is rejected");
    expect_true(!multipaste::is_hotkey_valid(HotkeySpec{0x0002U, 0U}),
                "missing virtual key is rejected");
    expect_true(multipaste::hotkey_name(HotkeySpec{0x0002U | 0x0004U, L'C'}) ==
                    L"Ctrl+Shift+C",
                "hotkey name is stable");

    using multipaste::TransactionAction;
    using multipaste::TransactionModel;
    using multipaste::TransactionState;

    TransactionModel direct;
    expect_true(direct.begin(MergeMode::Raw, 100U, 5000U),
                "idle transaction begins");
    expect_true(!direct.begin(MergeMode::SmartSpace, 101U, 5001U),
                "active transaction rejects overlap");
    expect_true(direct.on_timer(5100U, false) == TransactionAction::SendCopy,
                "short release requests standard copy");
    expect_true(direct.state() == TransactionState::WaitingForClipboard,
                "direct copy waits for clipboard");
    expect_true(direct.on_clipboard_changed(100U) == TransactionAction::None,
                "unchanged sequence is ignored");
    expect_true(direct.on_clipboard_changed(101U) == TransactionAction::CaptureClipboard,
                "new sequence is captured");
    expect_true(direct.state() == TransactionState::Idle,
                "capture completes transaction");
    expect_true(direct.mode() == MergeMode::Raw,
                "transaction preserves merge mode");

    TransactionModel manual;
    expect_true(manual.begin(MergeMode::SmartSpace, 20U, 1000U),
                "manual candidate begins");
    expect_true(manual.on_timer(1399U, true) == TransactionAction::None,
                "hold threshold is not early");
    expect_true(manual.on_timer(1400U, true) == TransactionAction::None,
                "long hold arms without injecting copy");
    expect_true(manual.state() == TransactionState::WaitingForClipboard,
                "long hold waits for real copy");
    expect_true(manual.is_manual(), "long hold records manual mode");
    expect_true(manual.on_clipboard_changed(21U) == TransactionAction::CaptureClipboard,
                "manual mode accepts any new clipboard sequence");

    TransactionModel direct_timeout;
    expect_true(direct_timeout.begin(MergeMode::Raw, 7U, 0U),
                "direct timeout transaction begins");
    expect_true(direct_timeout.on_timer(20U, false) == TransactionAction::SendCopy,
                "direct timeout sends copy");
    expect_true(direct_timeout.on_timer(1519U, false) == TransactionAction::None,
                "direct timeout waits full interval");
    expect_true(direct_timeout.on_timer(1520U, false) == TransactionAction::Timeout,
                "direct timeout fires at deadline");
    expect_true(direct_timeout.state() == TransactionState::Idle,
                "timeout resets transaction");

    TransactionModel manual_timeout;
    expect_true(manual_timeout.begin(MergeMode::Raw, 9U, 100U),
                "manual timeout transaction begins");
    manual_timeout.on_timer(500U, true);
    expect_true(manual_timeout.on_timer(5499U, false) == TransactionAction::None,
                "manual timeout waits five seconds");
    expect_true(manual_timeout.on_timer(5500U, false) == TransactionAction::Timeout,
                "manual timeout fires at five seconds");

    TransactionModel idle;
    expect_true(idle.on_timer(999U, false) == TransactionAction::None,
                "idle timer does nothing");
    expect_true(idle.on_clipboard_changed(44U) == TransactionAction::None,
                "idle clipboard change does nothing");
    expect_true(idle.begin(MergeMode::SmartSpace, 44U, 1000U),
                "cancel candidate begins");
    idle.cancel();
    expect_true(idle.state() == TransactionState::Idle,
                "cancel returns to idle");
    expect_true(idle.begin(MergeMode::Raw, 45U, 1100U),
                "cancelled transaction can restart");
    expect_true(multipaste::hotkey_name(HotkeySpec{0x0002U, 0x70U}) == L"Ctrl+F1",
                "function key name is stable");
    expect_true(multipaste::hotkey_name(HotkeySpec{0x0001U, 0x2EU}) == L"Alt+Key-46",
                "fallback key name is stable");

    std::cout << "All core tests passed.\n";
    return 0;
}
