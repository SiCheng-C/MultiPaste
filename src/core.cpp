#include "multipaste/core.hpp"

#include <cwctype>
#include <string>

namespace multipaste {

namespace {
constexpr unsigned int kModAlt = 0x0001U;
constexpr unsigned int kModControl = 0x0002U;
constexpr unsigned int kModShift = 0x0004U;
constexpr unsigned int kModWin = 0x0008U;
}

bool TransactionModel::begin(MergeMode mode, std::uint32_t baseline_sequence,
                             std::uint64_t now_ms) {
    if (state_ != TransactionState::Idle) return false;
    state_ = TransactionState::WaitingForRelease;
    mode_ = mode;
    baseline_sequence_ = baseline_sequence;
    started_at_ms_ = now_ms;
    deadline_ms_ = 0;
    manual_ = false;
    return true;
}

TransactionAction TransactionModel::on_timer(std::uint64_t now_ms,
                                             bool hotkey_is_down) {
    if (state_ == TransactionState::WaitingForRelease) {
        const std::uint64_t elapsed = now_ms - started_at_ms_;
        if (elapsed >= HoldThresholdMs) {
            manual_ = true;
            state_ = TransactionState::WaitingForClipboard;
            deadline_ms_ = now_ms + ManualTimeoutMs;
            return TransactionAction::None;
        }
        if (!hotkey_is_down) {
            manual_ = false;
            state_ = TransactionState::WaitingForClipboard;
            deadline_ms_ = now_ms + DirectTimeoutMs;
            return TransactionAction::SendCopy;
        }
        return TransactionAction::None;
    }
    if (state_ == TransactionState::WaitingForClipboard && now_ms >= deadline_ms_) {
        state_ = TransactionState::Idle;
        return TransactionAction::Timeout;
    }
    return TransactionAction::None;
}

TransactionAction TransactionModel::on_clipboard_changed(std::uint32_t sequence) {
    if (state_ != TransactionState::WaitingForClipboard ||
        sequence == baseline_sequence_) {
        return TransactionAction::None;
    }
    state_ = TransactionState::Idle;
    return TransactionAction::CaptureClipboard;
}

void TransactionModel::cancel() {
    state_ = TransactionState::Idle;
}

std::wstring merge_text(const std::wstring& base,
                        const std::wstring& selection,
                        MergeMode mode) {
    return merge_text(base, selection, mode, L" ");
}

std::wstring merge_text(const std::wstring& base,
                        const std::wstring& selection,
                        MergeMode mode,
                        const std::wstring& separator) {
    if (base.empty()) {
        return selection;
    }
    if (selection.empty()) {
        return base;
    }

    std::wstring result = base;
    if (mode == MergeMode::SmartSpace &&
        !separator.empty() &&
        !std::iswspace(static_cast<wint_t>(base.back())) &&
        !std::iswspace(static_cast<wint_t>(selection.front())) &&
        !base.ends_with(separator) &&
        !selection.starts_with(separator)) {
        result.append(separator);
    }
    result.append(selection);
    return result;
}

bool decode_separator_expression(const std::wstring& expression, std::wstring& output) {
    output.clear();
    output.reserve(expression.size());
    for (std::size_t index = 0; index < expression.size(); ++index) {
        const wchar_t current = expression[index];
        if (current != L'\\') {
            output.push_back(current);
            continue;
        }
        if (++index >= expression.size()) {
            output.clear();
            return false;
        }
        switch (expression[index]) {
        case L'n': output.push_back(L'\n'); break;
        case L'r': output.push_back(L'\r'); break;
        case L't': output.push_back(L'\t'); break;
        case L's': output.push_back(L' '); break;
        case L'\\': output.push_back(L'\\'); break;
        default:
            output.clear();
            return false;
        }
    }
    return true;
}

bool is_hotkey_valid(const HotkeySpec& hotkey) {
    constexpr unsigned int kAnyModifier = kModAlt | kModControl | kModShift | kModWin;
    return hotkey.virtual_key != 0U && (hotkey.modifiers & kAnyModifier) != 0U;
}

std::wstring hotkey_name(const HotkeySpec& hotkey) {
    std::wstring result;
    const auto add = [&result](const wchar_t* part) {
        if (!result.empty()) {
            result.push_back(L'+');
        }
        result.append(part);
    };

    if ((hotkey.modifiers & kModControl) != 0U) add(L"Ctrl");
    if ((hotkey.modifiers & kModAlt) != 0U) add(L"Alt");
    if ((hotkey.modifiers & kModShift) != 0U) add(L"Shift");
    if ((hotkey.modifiers & kModWin) != 0U) add(L"Win");

    if (hotkey.virtual_key >= L'A' && hotkey.virtual_key <= L'Z') {
        wchar_t key[] = {static_cast<wchar_t>(hotkey.virtual_key), L'\0'};
        add(key);
    } else if (hotkey.virtual_key >= L'0' && hotkey.virtual_key <= L'9') {
        wchar_t key[] = {static_cast<wchar_t>(hotkey.virtual_key), L'\0'};
        add(key);
    } else if (hotkey.virtual_key >= 0x70U && hotkey.virtual_key <= 0x87U) {
        const std::wstring key = L"F" + std::to_wstring(hotkey.virtual_key - 0x6FU);
        add(key.c_str());
    } else {
        const std::wstring key = L"Key-" + std::to_wstring(hotkey.virtual_key);
        add(key.c_str());
    }
    return result;
}

} // namespace multipaste
