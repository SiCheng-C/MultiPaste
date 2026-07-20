#pragma once

#include <cstdint>
#include <string>

namespace multipaste {

enum class MergeMode {
    SmartSpace,
    Raw,
};

struct HotkeySpec {
    unsigned int modifiers{};
    unsigned int virtual_key{};

    friend bool operator==(const HotkeySpec&, const HotkeySpec&) = default;
};

enum class TransactionState {
    Idle,
    WaitingForRelease,
    WaitingForClipboard,
};

enum class TransactionAction {
    None,
    SendCopy,
    CaptureClipboard,
    Timeout,
};

class TransactionModel {
public:
    static constexpr std::uint64_t HoldThresholdMs = 400;
    static constexpr std::uint64_t DirectTimeoutMs = 1500;
    static constexpr std::uint64_t ManualTimeoutMs = 5000;

    bool begin(MergeMode mode, std::uint32_t baseline_sequence,
               std::uint64_t now_ms);
    TransactionAction on_timer(std::uint64_t now_ms, bool hotkey_is_down);
    TransactionAction on_clipboard_changed(std::uint32_t sequence);
    void cancel();

    TransactionState state() const { return state_; }
    MergeMode mode() const { return mode_; }
    bool is_manual() const { return manual_; }

private:
    TransactionState state_{TransactionState::Idle};
    MergeMode mode_{MergeMode::Raw};
    std::uint32_t baseline_sequence_{};
    std::uint64_t started_at_ms_{};
    std::uint64_t deadline_ms_{};
    bool manual_{};
};

std::wstring merge_text(const std::wstring& base,
                        const std::wstring& selection,
                        MergeMode mode);
std::wstring merge_text(const std::wstring& base,
                        const std::wstring& selection,
                        MergeMode mode,
                        const std::wstring& separator);
bool decode_separator_expression(const std::wstring& expression, std::wstring& output);
bool is_hotkey_valid(const HotkeySpec& hotkey);
std::wstring hotkey_name(const HotkeySpec& hotkey);

} // namespace multipaste
