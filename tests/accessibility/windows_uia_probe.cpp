#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#define NOMINMAX

#include <windows.h>
#include <uiautomation.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cwchar>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "uiautomationcore.lib")

using Microsoft::WRL::ComPtr;

namespace {

constexpr int kDefaultTimeoutMs = 8000;
constexpr double kDoubleTolerance = 0.0001;
constexpr double kNoScrollPercent = -1.0;

class Failure : public std::runtime_error {
public:
    explicit Failure(const std::string &message) : std::runtime_error(message) {}
};

std::string narrow(const std::wstring &value) {
    if (value.empty()) return {};
    const int count = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (count <= 0) return {};
    std::string result(static_cast<size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), count, nullptr, nullptr);
    return result;
}

std::string hexHr(HRESULT hr) {
    std::ostringstream out;
    out << "0x" << std::hex << std::uppercase << static_cast<unsigned long>(hr);
    return out.str();
}

void require(bool condition, const std::string &message) {
    if (!condition) throw Failure(message);
}

void requireHr(HRESULT hr, const std::string &operation) {
    if (FAILED(hr)) throw Failure(operation + " failed with " + hexHr(hr));
}

bool approximately(double actual, double expected, double tolerance = kDoubleTolerance) {
    return std::abs(actual - expected) <= tolerance;
}

bool unavailable(HRESULT hr) {
    return hr == UIA_E_ELEMENTNOTAVAILABLE ||
           hr == RPC_E_DISCONNECTED ||
           hr == CO_E_OBJNOTCONNECTED ||
           hr == HRESULT_FROM_WIN32(RPC_S_SERVER_UNAVAILABLE) ||
           hr == HRESULT_FROM_WIN32(ERROR_INVALID_WINDOW_HANDLE);
}

struct Selector {
    PROPERTYID property = UIA_NamePropertyId;
    std::wstring value;

    static Selector parse(const std::wstring &text) {
        if (text.rfind(L"id:", 0) == 0) return { UIA_AutomationIdPropertyId, text.substr(3) };
        if (text.rfind(L"name:", 0) == 0) return { UIA_NamePropertyId, text.substr(5) };
        return { UIA_NamePropertyId, text };
    }

    std::string describe() const {
        return std::string(property == UIA_AutomationIdPropertyId ? "id:" : "name:") + narrow(value);
    }
};

struct TextSpan {
    int start = 0;
    int end = 0;
};

struct Options {
    DWORD pid = 0;
    int timeout_ms = kDefaultTimeoutMs;
    int max_materialized = 7;
    int expected_position = 42;
    int expected_size = 1000;
    bool close_host = true;
    int dump_limit = 128;
    std::wstring window_name;
    std::set<std::string> checks;

    Selector root = Selector::parse(L"name:Accessibility smoke");
    Selector list = Selector::parse(L"name:Lesson list");
    Selector item = Selector::parse(L"name:Lesson 42");
    Selector above = Selector::parse(L"name:Lesson 40");
    Selector replacement = Selector::parse(L"name:Lesson 48");
    Selector full_row = Selector::parse(L"name:Lesson 43");
    Selector invoke = Selector::parse(L"name:Count action");
    Selector toggle = Selector::parse(L"name:Study mode");
    Selector select = Selector::parse(L"name:Reading mode");
    Selector text = Selector::parse(L"name:Search lessons");
    Selector range_value = Selector::parse(L"name:Volume");
    Selector progress = Selector::parse(L"name:Completion");
    Selector details = Selector::parse(L"name:Details");
    Selector expanded_child = Selector::parse(L"name:Expanded details body");
    Selector grid = Selector::parse(L"name:Lesson grid");
    Selector grid_row = Selector::parse(L"name:Lesson row");
    Selector grid_cell = Selector::parse(L"name:Lesson cell");
    Selector state_cell = Selector::parse(L"name:State cell");
    Selector transient = Selector::parse(L"name:Transient target");
    Selector remove_transient = Selector::parse(L"name:Remove transient");
    Selector restore_transient = Selector::parse(L"name:Restore transient");

    Selector count_before = Selector::parse(L"name:Count result: 0");
    Selector count_after = Selector::parse(L"name:Count result: 1");
    Selector toggle_before = Selector::parse(L"name:Study mode result: off");
    Selector toggle_after = Selector::parse(L"name:Study mode result: on");
    Selector select_before = Selector::parse(L"name:Reading mode result: unselected");
    Selector select_after = Selector::parse(L"name:Reading mode result: selected");
    Selector text_before = Selector::parse(L"name:Search result: seed");
    Selector text_after = Selector::parse(L"name:Search result: \U0001f469\u200d\U0001f4bb" L"e\u0301Z");
    Selector value_before = Selector::parse(L"name:Volume result: 50");
    Selector value_after = Selector::parse(L"name:Volume result: 55");
    Selector details_before = Selector::parse(L"name:Details result: collapsed");
    Selector details_after = Selector::parse(L"name:Details result: expanded");
    Selector transient_before = Selector::parse(L"name:Transient result: present");
    Selector transient_after = Selector::parse(L"name:Transient result: removed");

    std::wstring initial_text = L"seed";
    std::wstring set_text = L"\U0001f469\u200d\U0001f4bb" L"e\u0301Z";
    TextSpan initial_selection = { 1, 3 };
    TextSpan initial_composition = { 2, 4 };
    TextSpan set_selection = { 1, 2 };
    std::wstring set_selection_text = L"e\u0301";
    bool set_selection_text_explicit = false;
    bool default_unicode_text = true;
    double initial_range_value = 0.50;
    double set_range_value = 0.55;
    double progress_value = 0.42;
};

const std::vector<std::string> &allChecks() {
    static const std::vector<std::string> checks = {
        "discovery", "hierarchy", "virtualization", "patterns", "bounds",
        "focus", "invoke", "toggle", "select", "text", "value", "expand",
        "events", "stale",
    };
    return checks;
}

void printUsage() {
    std::cout
        << "windows_uia_probe --pid PID [options]\n\n"
        << "Checks (comma-separated or repeated --check; default: all):\n"
        << "  discovery hierarchy virtualization patterns bounds focus invoke toggle\n"
        << "  select text value expand events stale\n\n"
        << "Selector syntax is name:TEXT (default) or id:AUTOMATION_ID.\n"
        << "Selector options: --root --list --item --above --replacement --full-row --invoke --toggle\n"
        << "  --select --text --value --progress --details --expanded-child --grid --grid-row --grid-cell\n"
        << "  --state-cell --transient --remove-transient --restore-transient and each --*-before/--*-after status.\n"
        << "Other options: --timeout-ms N --max-materialized N --expected-position N\n"
        << "  --expected-size N --window-name TEXT --no-close --dump-limit N\n"
        << "  --set-selection-text TEXT (required for custom Unicode selection spans)\n";
}

std::vector<std::wstring> splitWide(const std::wstring &value, wchar_t delimiter) {
    std::vector<std::wstring> result;
    size_t start = 0;
    while (start <= value.size()) {
        const size_t end = value.find(delimiter, start);
        result.push_back(value.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start));
        if (end == std::wstring::npos) break;
        start = end + 1;
    }
    return result;
}

int parseInt(const std::wstring &value, const std::string &option) {
    wchar_t *end = nullptr;
    const long parsed = std::wcstol(value.c_str(), &end, 10);
    if (!end || *end != L'\0') throw Failure(option + " expects an integer");
    return static_cast<int>(parsed);
}

double parseDouble(const std::wstring &value, const std::string &option) {
    wchar_t *end = nullptr;
    const double parsed = std::wcstod(value.c_str(), &end);
    if (!end || *end != L'\0') throw Failure(option + " expects a number");
    return parsed;
}

TextSpan parseSpan(const std::wstring &value, const std::string &option) {
    const auto parts = splitWide(value, L':');
    if (parts.size() != 2) throw Failure(option + " expects START:END");
    const TextSpan span = { parseInt(parts[0], option), parseInt(parts[1], option) };
    if (span.start < 0 || span.end < span.start) throw Failure(option + " has an invalid range");
    return span;
}

Options parseOptions(int argc, wchar_t **argv) {
    Options options;
    auto next = [&](int &index, const std::string &name) -> std::wstring {
        if (index + 1 >= argc) throw Failure(name + " requires a value");
        return argv[++index];
    };
    auto selector = [&](int &index, const std::string &name) {
        return Selector::parse(next(index, name));
    };

    for (int index = 1; index < argc; ++index) {
        const std::wstring arg = argv[index];
        if (arg == L"--help" || arg == L"-h") {
            printUsage();
            std::exit(0);
        } else if (arg == L"--pid") {
            const int pid = parseInt(next(index, "--pid"), "--pid");
            if (pid <= 0) throw Failure("--pid must be positive");
            options.pid = static_cast<DWORD>(pid);
        } else if (arg == L"--timeout-ms") {
            options.timeout_ms = parseInt(next(index, "--timeout-ms"), "--timeout-ms");
        } else if (arg == L"--max-materialized") {
            options.max_materialized = parseInt(next(index, "--max-materialized"), "--max-materialized");
        } else if (arg == L"--expected-position") {
            options.expected_position = parseInt(next(index, "--expected-position"), "--expected-position");
        } else if (arg == L"--expected-size") {
            options.expected_size = parseInt(next(index, "--expected-size"), "--expected-size");
        } else if (arg == L"--dump-limit") {
            options.dump_limit = parseInt(next(index, "--dump-limit"), "--dump-limit");
        } else if (arg == L"--window-name") {
            options.window_name = next(index, "--window-name");
        } else if (arg == L"--check") {
            for (const auto &check : splitWide(next(index, "--check"), L',')) {
                if (!check.empty()) options.checks.insert(narrow(check));
            }
        } else if (arg == L"--no-close") {
            options.close_host = false;
        } else if (arg == L"--root") options.root = selector(index, "--root");
        else if (arg == L"--list") options.list = selector(index, "--list");
        else if (arg == L"--item") options.item = selector(index, "--item");
        else if (arg == L"--above") options.above = selector(index, "--above");
        else if (arg == L"--replacement") options.replacement = selector(index, "--replacement");
        else if (arg == L"--full-row") options.full_row = selector(index, "--full-row");
        else if (arg == L"--invoke") options.invoke = selector(index, "--invoke");
        else if (arg == L"--toggle") options.toggle = selector(index, "--toggle");
        else if (arg == L"--select") options.select = selector(index, "--select");
        else if (arg == L"--text") options.text = selector(index, "--text");
        else if (arg == L"--value") options.range_value = selector(index, "--value");
        else if (arg == L"--progress") options.progress = selector(index, "--progress");
        else if (arg == L"--details") options.details = selector(index, "--details");
        else if (arg == L"--expanded-child") options.expanded_child = selector(index, "--expanded-child");
        else if (arg == L"--grid") options.grid = selector(index, "--grid");
        else if (arg == L"--grid-row") options.grid_row = selector(index, "--grid-row");
        else if (arg == L"--grid-cell") options.grid_cell = selector(index, "--grid-cell");
        else if (arg == L"--state-cell") options.state_cell = selector(index, "--state-cell");
        else if (arg == L"--transient") options.transient = selector(index, "--transient");
        else if (arg == L"--remove-transient") options.remove_transient = selector(index, "--remove-transient");
        else if (arg == L"--restore-transient") options.restore_transient = selector(index, "--restore-transient");
        else if (arg == L"--count-before") options.count_before = selector(index, "--count-before");
        else if (arg == L"--count-after") options.count_after = selector(index, "--count-after");
        else if (arg == L"--toggle-before") options.toggle_before = selector(index, "--toggle-before");
        else if (arg == L"--toggle-after") options.toggle_after = selector(index, "--toggle-after");
        else if (arg == L"--select-before") options.select_before = selector(index, "--select-before");
        else if (arg == L"--select-after") options.select_after = selector(index, "--select-after");
        else if (arg == L"--text-before") options.text_before = selector(index, "--text-before");
        else if (arg == L"--text-after") options.text_after = selector(index, "--text-after");
        else if (arg == L"--value-before") options.value_before = selector(index, "--value-before");
        else if (arg == L"--value-after") options.value_after = selector(index, "--value-after");
        else if (arg == L"--details-before") options.details_before = selector(index, "--details-before");
        else if (arg == L"--details-after") options.details_after = selector(index, "--details-after");
        else if (arg == L"--transient-before") options.transient_before = selector(index, "--transient-before");
        else if (arg == L"--transient-after") options.transient_after = selector(index, "--transient-after");
        else if (arg == L"--initial-text") options.initial_text = next(index, "--initial-text");
        else if (arg == L"--set-text") {
            options.set_text = next(index, "--set-text");
            options.default_unicode_text = false;
        }
        else if (arg == L"--initial-selection") options.initial_selection = parseSpan(next(index, "--initial-selection"), "--initial-selection");
        else if (arg == L"--initial-composition") options.initial_composition = parseSpan(next(index, "--initial-composition"), "--initial-composition");
        else if (arg == L"--set-selection") {
            options.set_selection = parseSpan(next(index, "--set-selection"), "--set-selection");
            options.default_unicode_text = false;
        }
        else if (arg == L"--set-selection-text") {
            options.set_selection_text = next(index, "--set-selection-text");
            options.set_selection_text_explicit = true;
        }
        else if (arg == L"--initial-range-value") options.initial_range_value = parseDouble(next(index, "--initial-range-value"), "--initial-range-value");
        else if (arg == L"--set-range-value") options.set_range_value = parseDouble(next(index, "--set-range-value"), "--set-range-value");
        else if (arg == L"--progress-value") options.progress_value = parseDouble(next(index, "--progress-value"), "--progress-value");
        else throw Failure("unknown option: " + narrow(arg));
    }

    require(options.pid != 0, "--pid is required");
    require(options.timeout_ms > 0, "--timeout-ms must be positive");
    require(options.max_materialized > 0, "--max-materialized must be positive");
    if (options.checks.empty() || options.checks.count("all") != 0) {
        options.checks.clear();
        options.checks.insert(allChecks().begin(), allChecks().end());
    }
    for (const auto &check : options.checks) {
        require(std::find(allChecks().begin(), allChecks().end(), check) != allChecks().end(), "unknown check: " + check);
    }
    return options;
}

std::wstring bstrString(BSTR value) {
    return value ? std::wstring(value, SysStringLen(value)) : std::wstring();
}

std::wstring currentName(IUIAutomationElement *element) {
    BSTR value = nullptr;
    requireHr(element->get_CurrentName(&value), "get_CurrentName");
    const std::wstring result = bstrString(value);
    SysFreeString(value);
    return result;
}

std::wstring currentAutomationId(IUIAutomationElement *element) {
    BSTR value = nullptr;
    requireHr(element->get_CurrentAutomationId(&value), "get_CurrentAutomationId");
    const std::wstring result = bstrString(value);
    SysFreeString(value);
    return result;
}

CONTROLTYPEID currentControlType(IUIAutomationElement *element) {
    CONTROLTYPEID value = 0;
    requireHr(element->get_CurrentControlType(&value), "get_CurrentControlType");
    return value;
}

std::string controlTypeName(CONTROLTYPEID value) {
    switch (value) {
        case UIA_GroupControlTypeId: return "Group";
        case UIA_ListControlTypeId: return "List";
        case UIA_ListItemControlTypeId: return "ListItem";
        case UIA_ButtonControlTypeId: return "Button";
        case UIA_CheckBoxControlTypeId: return "CheckBox";
        case UIA_RadioButtonControlTypeId: return "RadioButton";
        case UIA_EditControlTypeId: return "Edit";
        case UIA_SliderControlTypeId: return "Slider";
        case UIA_ProgressBarControlTypeId: return "ProgressBar";
        case UIA_DataGridControlTypeId: return "DataGrid";
        case UIA_DataItemControlTypeId: return "DataItem";
        case UIA_TextControlTypeId: return "Text";
        case UIA_WindowControlTypeId: return "Window";
        case UIA_PaneControlTypeId: return "Pane";
        default: return "control-type-" + std::to_string(value);
    }
}

RECT currentRect(IUIAutomationElement *element) {
    RECT rect = {};
    requireHr(element->get_CurrentBoundingRectangle(&rect), "get_CurrentBoundingRectangle");
    return rect;
}

bool currentOffscreen(IUIAutomationElement *element) {
    BOOL value = FALSE;
    requireHr(element->get_CurrentIsOffscreen(&value), "get_CurrentIsOffscreen");
    return value != FALSE;
}

int intProperty(IUIAutomationElement *element, PROPERTYID property) {
    VARIANT value;
    VariantInit(&value);
    requireHr(element->GetCurrentPropertyValue(property, &value), "GetCurrentPropertyValue(" + std::to_string(property) + ")");
    int result = 0;
    if (value.vt == VT_I4 || value.vt == VT_INT) result = value.lVal;
    else {
        const VARTYPE type = value.vt;
        VariantClear(&value);
        throw Failure("property " + std::to_string(property) + " returned VARIANT type " + std::to_string(type));
    }
    VariantClear(&value);
    return result;
}

std::vector<int> runtimeId(IUIAutomationElement *element) {
    SAFEARRAY *array = nullptr;
    requireHr(element->GetRuntimeId(&array), "GetRuntimeId");
    require(array != nullptr, "GetRuntimeId returned no runtime ID");
    LONG lower = 0;
    LONG upper = -1;
    requireHr(SafeArrayGetLBound(array, 1, &lower), "SafeArrayGetLBound(runtime ID)");
    requireHr(SafeArrayGetUBound(array, 1, &upper), "SafeArrayGetUBound(runtime ID)");
    std::vector<int> result;
    for (LONG index = lower; index <= upper; ++index) {
        int value = 0;
        requireHr(SafeArrayGetElement(array, &index, &value), "SafeArrayGetElement(runtime ID)");
        result.push_back(value);
    }
    SafeArrayDestroy(array);
    return result;
}

ComPtr<IUIAutomationCondition> conditionFor(IUIAutomation *automation, const Selector &selector) {
    VARIANT value;
    VariantInit(&value);
    value.vt = VT_BSTR;
    value.bstrVal = SysAllocStringLen(selector.value.data(), static_cast<UINT>(selector.value.size()));
    require(value.bstrVal != nullptr || selector.value.empty(), "could not allocate selector value");
    ComPtr<IUIAutomationCondition> condition;
    const HRESULT hr = automation->CreatePropertyCondition(selector.property, value, &condition);
    VariantClear(&value);
    requireHr(hr, "CreatePropertyCondition(" + selector.describe() + ")");
    return condition;
}

bool matches(IUIAutomationElement *element, const Selector &selector) {
    return selector.property == UIA_AutomationIdPropertyId
        ? currentAutomationId(element) == selector.value
        : currentName(element) == selector.value;
}

ComPtr<IUIAutomationElement> findElement(IUIAutomation *automation, IUIAutomationElement *root, const Selector &selector) {
    if (matches(root, selector)) {
        ComPtr<IUIAutomationElement> result = root;
        return result;
    }
    const auto condition = conditionFor(automation, selector);
    ComPtr<IUIAutomationElement> result;
    requireHr(root->FindFirst(TreeScope_Descendants, condition.Get(), &result), "FindFirst(" + selector.describe() + ")");
    return result;
}

template <typename Predicate>
bool waitUntil(int timeout_ms, Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    do {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    } while (std::chrono::steady_clock::now() < deadline);
    return predicate();
}

ComPtr<IUIAutomationElement> requireElement(IUIAutomation *automation, IUIAutomationElement *root, const Selector &selector, int timeout_ms) {
    ComPtr<IUIAutomationElement> result;
    HRESULT last_hr = S_OK;
    const bool found = waitUntil(timeout_ms, [&] {
        try {
            result = findElement(automation, root, selector);
            return result != nullptr;
        } catch (const Failure &) {
            last_hr = E_FAIL;
            return false;
        }
    });
    if (!found) {
        const std::string suffix = FAILED(last_hr) ? " after UIA query failures" : "";
        throw Failure("element " + selector.describe() + " was not found within " + std::to_string(timeout_ms) + " ms" + suffix);
    }
    return result;
}

void requireStatus(IUIAutomation *automation, IUIAutomationElement *root, const Selector &selector, int timeout_ms) {
    (void)requireElement(automation, root, selector, timeout_ms);
}

template <typename Pattern>
ComPtr<Pattern> requirePattern(IUIAutomationElement *element, PATTERNID pattern, REFIID iid, const std::string &name) {
    ComPtr<Pattern> result;
    const HRESULT hr = element->GetCurrentPatternAs(pattern, iid, reinterpret_cast<void **>(result.GetAddressOf()));
    if (hr == UIA_E_NOTSUPPORTED || (SUCCEEDED(hr) && !result)) throw Failure(name + " pattern is not available");
    requireHr(hr, "GetCurrentPatternAs(" + name + ")");
    return result;
}

bool hasPattern(IUIAutomationElement *element, PATTERNID pattern, REFIID iid) {
    ComPtr<IUnknown> result;
    const HRESULT hr = element->GetCurrentPatternAs(pattern, iid, reinterpret_cast<void **>(result.GetAddressOf()));
    if (hr == UIA_E_NOTSUPPORTED) return false;
    requireHr(hr, "GetCurrentPatternAs");
    return result != nullptr;
}

void expectControlType(IUIAutomationElement *element, CONTROLTYPEID expected, const std::string &label) {
    const CONTROLTYPEID actual = currentControlType(element);
    require(actual == expected, label + " has control type " + controlTypeName(actual) + ", expected " + controlTypeName(expected));
}

bool sameElement(IUIAutomation *automation, IUIAutomationElement *left, IUIAutomationElement *right) {
    BOOL same = FALSE;
    requireHr(automation->CompareElements(left, right, &same), "CompareElements");
    return same != FALSE;
}

ComPtr<IUIAutomationElement> rawParent(IUIAutomation *automation, IUIAutomationElement *element) {
    ComPtr<IUIAutomationTreeWalker> walker;
    requireHr(automation->get_RawViewWalker(&walker), "get_RawViewWalker");
    ComPtr<IUIAutomationElement> parent;
    requireHr(walker->GetParentElement(element, &parent), "RawViewWalker.GetParentElement");
    return parent;
}

std::wstring valueText(IUIAutomationValuePattern *pattern) {
    BSTR value = nullptr;
    requireHr(pattern->get_CurrentValue(&value), "Value.get_CurrentValue");
    const std::wstring result = bstrString(value);
    SysFreeString(value);
    return result;
}

HRESULT setValue(IUIAutomationValuePattern *pattern, const std::wstring &text) {
    BSTR value = SysAllocStringLen(text.data(), static_cast<UINT>(text.size()));
    if (!value && !text.empty()) return E_OUTOFMEMORY;
    const HRESULT hr = pattern->SetValue(value);
    SysFreeString(value);
    return hr;
}

std::wstring rangeText(IUIAutomationTextRange *range) {
    BSTR value = nullptr;
    requireHr(range->GetText(-1, &value), "TextRange.GetText");
    const std::wstring result = bstrString(value);
    SysFreeString(value);
    return result;
}

bool wellFormedUtf16(const std::wstring &value) {
    for (size_t index = 0; index < value.size(); ++index) {
        const wchar_t code_unit = value[index];
        if (code_unit >= 0xd800 && code_unit <= 0xdbff) {
            if (index + 1 >= value.size() || value[index + 1] < 0xdc00 || value[index + 1] > 0xdfff) return false;
            ++index;
        } else if (code_unit >= 0xdc00 && code_unit <= 0xdfff) {
            return false;
        }
    }
    return true;
}

ComPtr<IUIAutomationTextRange> firstTextRange(IUIAutomationTextRangeArray *array, const std::string &kind) {
    require(array != nullptr, kind + " returned no range array");
    int length = 0;
    requireHr(array->get_Length(&length), kind + ".get_Length");
    require(length == 1, kind + " must expose exactly one range");
    ComPtr<IUIAutomationTextRange> range;
    requireHr(array->GetElement(0, &range), kind + ".GetElement");
    require(range != nullptr, kind + " returned a null range");
    return range;
}

std::wstring selectedText(IUIAutomationTextPattern *pattern) {
    ComPtr<IUIAutomationTextRangeArray> selection;
    requireHr(pattern->GetSelection(&selection), "Text.GetSelection");
    auto range = firstTextRange(selection.Get(), "text selection");
    return rangeText(range.Get());
}

void expectCharacterUnits(IUIAutomationTextPattern *pattern, const std::vector<std::wstring> &expected) {
    ComPtr<IUIAutomationTextRange> document;
    requireHr(pattern->get_DocumentRange(&document), "Text.get_DocumentRange(character units)");
    ComPtr<IUIAutomationTextRange> range;
    requireHr(document->Clone(&range), "TextRange.Clone(character units)");
    requireHr(range->MoveEndpointByRange(TextPatternRangeEndpoint_End, document.Get(), TextPatternRangeEndpoint_Start), "TextRange collapse(character units)");
    for (const std::wstring &unit : expected) {
        int moved = 0;
        requireHr(range->MoveEndpointByUnit(TextPatternRangeEndpoint_End, TextUnit_Character, 1, &moved), "TextRange move character end");
        require(moved == 1, "TextRange stopped before the expected character unit");
        require(rangeText(range.Get()) == unit, "TextRange split a linguistic character unit");
        requireHr(range->MoveEndpointByRange(TextPatternRangeEndpoint_Start, range.Get(), TextPatternRangeEndpoint_End), "TextRange advance character start");
    }
    int moved = 0;
    requireHr(range->MoveEndpointByUnit(TextPatternRangeEndpoint_End, TextUnit_Character, 1, &moved), "TextRange move past document end");
    require(moved == 0, "TextRange moved past the document end");
}

void selectText(IUIAutomationTextPattern *pattern, TextSpan span) {
    ComPtr<IUIAutomationTextRange> document;
    requireHr(pattern->get_DocumentRange(&document), "Text.get_DocumentRange");
    ComPtr<IUIAutomationTextRange> selection;
    requireHr(document->Clone(&selection), "TextRange.Clone");
    requireHr(selection->MoveEndpointByRange(TextPatternRangeEndpoint_End, document.Get(), TextPatternRangeEndpoint_Start), "TextRange collapse");
    int moved = 0;
    requireHr(selection->MoveEndpointByUnit(TextPatternRangeEndpoint_End, TextUnit_Character, span.end, &moved), "TextRange move end");
    require(moved == span.end, "text document is shorter than requested selection end");
    moved = 0;
    requireHr(selection->MoveEndpointByUnit(TextPatternRangeEndpoint_Start, TextUnit_Character, span.start, &moved), "TextRange move start");
    require(moved == span.start, "text document is shorter than requested selection start");
    requireHr(selection->Select(), "TextRange.Select");
}

std::wstring substring(const std::wstring &value, TextSpan span) {
    require(span.start >= 0 && span.end >= span.start && static_cast<size_t>(span.end) <= value.size(), "configured text span is outside the text");
    return value.substr(static_cast<size_t>(span.start), static_cast<size_t>(span.end - span.start));
}

struct WindowSearch {
    DWORD pid = 0;
    std::wstring name;
    HWND result = nullptr;
};

BOOL CALLBACK findWindowCallback(HWND hwnd, LPARAM context_value) {
    auto *context = reinterpret_cast<WindowSearch *>(context_value);
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != context->pid || !IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER) != nullptr) return TRUE;
    if (!context->name.empty()) {
        const int length = GetWindowTextLengthW(hwnd);
        std::wstring title(static_cast<size_t>(std::max(length, 0)), L'\0');
        if (length > 0) GetWindowTextW(hwnd, title.data(), length + 1);
        if (title.find(context->name) == std::wstring::npos) return TRUE;
    }
    context->result = hwnd;
    return FALSE;
}

HWND waitForWindow(const Options &options) {
    HWND result = nullptr;
    const bool found = waitUntil(options.timeout_ms, [&] {
        WindowSearch search = { options.pid, options.window_name, nullptr };
        EnumWindows(findWindowCallback, reinterpret_cast<LPARAM>(&search));
        result = search.result;
        return result != nullptr;
    });
    if (!found) throw Failure("no visible top-level window was found for PID " + std::to_string(options.pid));
    return result;
}

struct Connection {
    ComPtr<IUIAutomation> automation;
    ComPtr<IUIAutomationElement> window;
    ComPtr<IUIAutomationElement> root;
    HWND hwnd = nullptr;
};

Connection connect(const Options &options) {
    Connection connection;
    requireHr(CoCreateInstance(CLSID_CUIAutomation8, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&connection.automation)), "CoCreateInstance(CUIAutomation8)");
    connection.hwnd = waitForWindow(options);
    requireHr(connection.automation->ElementFromHandle(connection.hwnd, &connection.window), "ElementFromHandle");
    connection.root = requireElement(connection.automation.Get(), connection.window.Get(), options.root, options.timeout_ms);
    return connection;
}

class AutomationEventRecorder final : public IUIAutomationEventHandler,
                                      public IUIAutomationFocusChangedEventHandler {
public:
    void setFocusTarget(IUIAutomation *automation, IUIAutomationElement *target) {
        automation_ = automation;
        focus_target_ = target;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **object) override {
        if (!object) return E_INVALIDARG;
        *object = nullptr;
        if (iid == IID_IUnknown || iid == IID_IUIAutomationEventHandler) *object = static_cast<IUIAutomationEventHandler *>(this);
        else if (iid == IID_IUIAutomationFocusChangedEventHandler) *object = static_cast<IUIAutomationFocusChangedEventHandler *>(this);
        else return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining = --references_;
        if (remaining == 0) delete this;
        return remaining;
    }
    HRESULT STDMETHODCALLTYPE HandleAutomationEvent(IUIAutomationElement *, EVENTID event_id) override {
        std::lock_guard<std::mutex> guard(mutex_);
        counts_[event_id] += 1;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE HandleFocusChangedEvent(IUIAutomationElement *sender) override {
        BOOL same = FALSE;
        if (!sender || !automation_ || !focus_target_ ||
            FAILED(automation_->CompareElements(sender, focus_target_.Get(), &same)) || !same) return S_OK;
        std::lock_guard<std::mutex> guard(mutex_);
        counts_[UIA_AutomationFocusChangedEventId] += 1;
        return S_OK;
    }
    int count(EVENTID event_id) const {
        std::lock_guard<std::mutex> guard(mutex_);
        const auto found = counts_.find(event_id);
        return found == counts_.end() ? 0 : found->second;
    }
private:
    std::atomic<ULONG> references_{1};
    mutable std::mutex mutex_;
    std::map<EVENTID, int> counts_;
    ComPtr<IUIAutomation> automation_;
    ComPtr<IUIAutomationElement> focus_target_;
};

class PropertyEventRecorder final : public IUIAutomationPropertyChangedEventHandler {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **object) override {
        if (!object) return E_INVALIDARG;
        *object = nullptr;
        if (iid == IID_IUnknown || iid == IID_IUIAutomationPropertyChangedEventHandler) *object = static_cast<IUIAutomationPropertyChangedEventHandler *>(this);
        else return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining = --references_;
        if (remaining == 0) delete this;
        return remaining;
    }
    HRESULT STDMETHODCALLTYPE HandlePropertyChangedEvent(IUIAutomationElement *, PROPERTYID property_id, VARIANT) override {
        std::lock_guard<std::mutex> guard(mutex_);
        counts_[property_id] += 1;
        return S_OK;
    }
    int count(PROPERTYID property_id) const {
        std::lock_guard<std::mutex> guard(mutex_);
        const auto found = counts_.find(property_id);
        return found == counts_.end() ? 0 : found->second;
    }
private:
    std::atomic<ULONG> references_{1};
    mutable std::mutex mutex_;
    std::map<PROPERTYID, int> counts_;
};

class TextEditEventRecorder final : public IUIAutomationTextEditTextChangedEventHandler {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **object) override {
        if (!object) return E_INVALIDARG;
        *object = nullptr;
        if (iid == IID_IUnknown || iid == IID_IUIAutomationTextEditTextChangedEventHandler) {
            *object = static_cast<IUIAutomationTextEditTextChangedEventHandler *>(this);
        } else {
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining = --references_;
        if (remaining == 0) delete this;
        return remaining;
    }
    HRESULT STDMETHODCALLTYPE HandleTextEditTextChangedEvent(
        IUIAutomationElement *,
        TextEditChangeType type,
        SAFEARRAY *event_strings
    ) override {
        std::vector<std::wstring> strings;
        bool valid = event_strings != nullptr && SafeArrayGetDim(event_strings) == 1;
        VARTYPE vartype = VT_EMPTY;
        LONG lower = 0;
        LONG upper = -1;
        if (valid && (FAILED(SafeArrayGetVartype(event_strings, &vartype)) || vartype != VT_BSTR ||
                      FAILED(SafeArrayGetLBound(event_strings, 1, &lower)) ||
                      FAILED(SafeArrayGetUBound(event_strings, 1, &upper)) || upper < lower)) valid = false;
        if (valid) {
            for (LONG index = lower; index <= upper; ++index) {
                BSTR value = nullptr;
                if (FAILED(SafeArrayGetElement(event_strings, &index, &value))) {
                    valid = false;
                    break;
                }
                strings.push_back(bstrString(value));
                SysFreeString(value);
            }
        }
        std::lock_guard<std::mutex> guard(mutex_);
        if (!valid) invalid_payloads_ += 1;
        events_[type].push_back(std::move(strings));
        return S_OK;
    }
    int count(TextEditChangeType type) const {
        std::lock_guard<std::mutex> guard(mutex_);
        const auto found = events_.find(type);
        return found == events_.end() ? 0 : (int)found->second.size();
    }
    bool hasSingleEmptyPayload(TextEditChangeType type) const {
        std::lock_guard<std::mutex> guard(mutex_);
        const auto found = events_.find(type);
        return found != events_.end() && found->second.size() == 1 &&
               found->second[0].size() == 1 && found->second[0][0].empty();
    }
    int invalidPayloads() const {
        std::lock_guard<std::mutex> guard(mutex_);
        return invalid_payloads_;
    }
private:
    std::atomic<ULONG> references_{1};
    mutable std::mutex mutex_;
    std::map<TextEditChangeType, std::vector<std::vector<std::wstring>>> events_;
    int invalid_payloads_ = 0;
};

class StructureEventRecorder final : public IUIAutomationStructureChangedEventHandler {
public:
    struct Record {
        StructureChangeType type;
        std::vector<int> sender_runtime_id;
        std::vector<int> changed_runtime_id;
    };
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **object) override {
        if (!object) return E_INVALIDARG;
        *object = nullptr;
        if (iid == IID_IUnknown || iid == IID_IUIAutomationStructureChangedEventHandler) *object = static_cast<IUIAutomationStructureChangedEventHandler *>(this);
        else return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining = --references_;
        if (remaining == 0) delete this;
        return remaining;
    }
    HRESULT STDMETHODCALLTYPE HandleStructureChangedEvent(IUIAutomationElement *sender, StructureChangeType change_type, SAFEARRAY *runtime_id) override {
        SAFEARRAY *sender_array = nullptr;
        std::vector<int> sender_id;
        if (sender && SUCCEEDED(sender->GetRuntimeId(&sender_array)) && sender_array) {
            sender_id = arrayValues(sender_array);
            SafeArrayDestroy(sender_array);
        }
        const std::vector<int> changed_id = arrayValues(runtime_id);
        std::lock_guard<std::mutex> guard(mutex_);
        counts_[change_type] += 1;
        records_.push_back({ change_type, std::move(sender_id), changed_id });
        if (change_type == StructureChangeType_ChildRemoved && changed_id.empty()) invalid_removed_runtime_ids_ += 1;
        return S_OK;
    }
    int total() const {
        std::lock_guard<std::mutex> guard(mutex_);
        int result = 0;
        for (const auto &entry : counts_) result += entry.second;
        return result;
    }
    int count(StructureChangeType type) const {
        std::lock_guard<std::mutex> guard(mutex_);
        const auto found = counts_.find(type);
        return found == counts_.end() ? 0 : found->second;
    }
    int invalidRemovedRuntimeIds() const {
        std::lock_guard<std::mutex> guard(mutex_);
        return invalid_removed_runtime_ids_;
    }
    bool hasExactChange(
        StructureChangeType type,
        const std::vector<int> &sender_runtime_id,
        const std::vector<int> &changed_runtime_id = {}
    ) const {
        std::lock_guard<std::mutex> guard(mutex_);
        for (const Record &record : records_) {
            if (record.type == type && record.sender_runtime_id == sender_runtime_id &&
                record.changed_runtime_id == changed_runtime_id) return true;
        }
        return false;
    }
private:
    static std::vector<int> arrayValues(SAFEARRAY *array) {
        std::vector<int> result;
        if (!array || SafeArrayGetDim(array) != 1) return result;
        VARTYPE vartype = VT_EMPTY;
        LONG lower = 0;
        LONG upper = -1;
        if (FAILED(SafeArrayGetVartype(array, &vartype)) || vartype != VT_I4 ||
            FAILED(SafeArrayGetLBound(array, 1, &lower)) ||
            FAILED(SafeArrayGetUBound(array, 1, &upper)) || upper < lower) return result;
        for (LONG index = lower; index <= upper; ++index) {
            int value = 0;
            if (FAILED(SafeArrayGetElement(array, &index, &value))) return {};
            result.push_back(value);
        }
        return result;
    }
    std::atomic<ULONG> references_{1};
    mutable std::mutex mutex_;
    std::map<StructureChangeType, int> counts_;
    std::vector<Record> records_;
    int invalid_removed_runtime_ids_ = 0;
};

class StructureEventSession {
public:
    ~StructureEventSession() { stop(); }

    void start(IUIAutomation *automation, IUIAutomationElement *root) {
        automation_ = automation;
        automation_->AddRef();
        root_ = root;
        events_.Attach(new StructureEventRecorder());
        const HRESULT hr = automation_->AddStructureChangedEventHandler(root_.Get(), TreeScope_Subtree, nullptr, events_.Get());
        if (FAILED(hr)) { stop(); requireHr(hr, "AddStructureChangedEventHandler(stale)"); }
        registered_ = true;
    }

    void stop() {
        if (!automation_) return;
        if (registered_) automation_->RemoveStructureChangedEventHandler(root_.Get(), events_.Get());
        registered_ = false;
        events_.Reset();
        root_.Reset();
        automation_->Release();
        automation_ = nullptr;
    }

    StructureEventRecorder *events() const { return events_.Get(); }

private:
    IUIAutomation *automation_ = nullptr;
    ComPtr<IUIAutomationElement> root_;
    ComPtr<StructureEventRecorder> events_;
    bool registered_ = false;
};

class PreciseEventSession {
public:
    void start(IUIAutomation *automation, IUIAutomationElement *root,
               IUIAutomationElement *focus, IUIAutomationElement *invoke,
               IUIAutomationElement *toggle, IUIAutomationElement *select,
               IUIAutomationElement *text, IUIAutomationElement *range,
               IUIAutomationElement *details) {
        automation_ = automation;
        automation_->AddRef();
        automation_events_.Attach(new AutomationEventRecorder());
        automation_events_->setFocusTarget(automation_, focus);
        property_events_.Attach(new PropertyEventRecorder());
        text_edit_events_.Attach(new TextEditEventRecorder());
        structure_events_.Attach(new StructureEventRecorder());
        root_ = root;
        text_element_ = text;
        try {
            requireHr(automation_->QueryInterface(IID_IUIAutomation3, reinterpret_cast<void **>(automation3_.GetAddressOf())), "QueryInterface(IUIAutomation3)");
            requireHr(automation_->AddFocusChangedEventHandler(nullptr, automation_events_.Get()), "AddFocusChangedEventHandler");
            focus_registered_ = true;
            addAutomation(invoke, UIA_Invoke_InvokedEventId);
            addAutomation(select, UIA_SelectionItem_ElementSelectedEventId);
            addAutomation(text, UIA_Text_TextChangedEventId);
            addAutomation(text, UIA_Text_TextSelectionChangedEventId);
            addAutomation(text, UIA_TextEdit_ConversionTargetChangedEventId);
            requireHr(automation3_->AddTextEditTextChangedEventHandler(
                text,
                TreeScope_Element,
                TextEditChangeType_CompositionFinalized,
                nullptr,
                text_edit_events_.Get()
            ), "AddTextEditTextChangedEventHandler(CompositionFinalized)");
            text_edit_registered_ = true;
            addProperties(toggle, { UIA_ToggleToggleStatePropertyId });
            addProperties(select, { UIA_SelectionItemIsSelectedPropertyId });
            addProperties(text, { UIA_ValueValuePropertyId });
            addProperties(range, { UIA_RangeValueValuePropertyId });
            addProperties(details, { UIA_ExpandCollapseExpandCollapseStatePropertyId });
            requireHr(automation_->AddStructureChangedEventHandler(root, TreeScope_Subtree, nullptr, structure_events_.Get()), "AddStructureChangedEventHandler");
            structure_registered_ = true;
        } catch (...) {
            stop();
            throw;
        }
    }

    ~PreciseEventSession() { stop(); }

    void stop() {
        if (!automation_) return;
        if (focus_registered_) automation_->RemoveFocusChangedEventHandler(automation_events_.Get());
        for (const auto &registration : automation_registrations_) {
            automation_->RemoveAutomationEventHandler(registration.event, registration.element.Get(), automation_events_.Get());
        }
        for (const auto &registration : property_registrations_) {
            automation_->RemovePropertyChangedEventHandler(registration.Get(), property_events_.Get());
        }
        if (text_edit_registered_) {
            automation3_->RemoveTextEditTextChangedEventHandler(text_element_.Get(), text_edit_events_.Get());
        }
        if (structure_registered_) automation_->RemoveStructureChangedEventHandler(root_.Get(), structure_events_.Get());
        automation_registrations_.clear();
        property_registrations_.clear();
        focus_registered_ = false;
        text_edit_registered_ = false;
        structure_registered_ = false;
        root_.Reset();
        automation_events_.Reset();
        property_events_.Reset();
        text_edit_events_.Reset();
        structure_events_.Reset();
        text_element_.Reset();
        automation3_.Reset();
        automation_->Release();
        automation_ = nullptr;
    }

    void verify(int timeout_ms) const {
        require(automation_events_ && property_events_ && text_edit_events_ && structure_events_, "event handlers were not registered");
        const bool complete = waitUntil(timeout_ms, [&] {
            return automation_events_->count(UIA_AutomationFocusChangedEventId) > 0 &&
                   automation_events_->count(UIA_Invoke_InvokedEventId) > 0 &&
                   automation_events_->count(UIA_SelectionItem_ElementSelectedEventId) > 0 &&
                   automation_events_->count(UIA_Text_TextChangedEventId) > 0 &&
                   automation_events_->count(UIA_Text_TextSelectionChangedEventId) > 0 &&
                   automation_events_->count(UIA_TextEdit_ConversionTargetChangedEventId) > 0 &&
                   text_edit_events_->count(TextEditChangeType_CompositionFinalized) > 0 &&
                   property_events_->count(UIA_ToggleToggleStatePropertyId) > 0 &&
                   property_events_->count(UIA_SelectionItemIsSelectedPropertyId) > 0 &&
                   property_events_->count(UIA_ValueValuePropertyId) > 0 &&
                   property_events_->count(UIA_RangeValueValuePropertyId) > 0 &&
                   property_events_->count(UIA_ExpandCollapseExpandCollapseStatePropertyId) > 0;
        });
        if (!complete) {
            std::ostringstream message;
            message << "missing precise events: focus=" << automation_events_->count(UIA_AutomationFocusChangedEventId)
                    << " invoke=" << automation_events_->count(UIA_Invoke_InvokedEventId)
                    << " select=" << automation_events_->count(UIA_SelectionItem_ElementSelectedEventId)
                    << " text=" << automation_events_->count(UIA_Text_TextChangedEventId)
                    << " text-selection=" << automation_events_->count(UIA_Text_TextSelectionChangedEventId)
                    << " conversion-target=" << automation_events_->count(UIA_TextEdit_ConversionTargetChangedEventId)
                    << " composition-finalized=" << text_edit_events_->count(TextEditChangeType_CompositionFinalized)
                    << " toggle=" << property_events_->count(UIA_ToggleToggleStatePropertyId)
                    << " selection-item=" << property_events_->count(UIA_SelectionItemIsSelectedPropertyId)
                    << " value=" << property_events_->count(UIA_ValueValuePropertyId)
                    << " range=" << property_events_->count(UIA_RangeValueValuePropertyId)
                    << " expand=" << property_events_->count(UIA_ExpandCollapseExpandCollapseStatePropertyId);
            throw Failure(message.str());
        }
        require(text_edit_events_->invalidPayloads() == 0,
                "TextEdit CompositionFinalized event carried an invalid SAFEARRAY payload");
        require(text_edit_events_->hasSingleEmptyPayload(TextEditChangeType_CompositionFinalized),
                "replacing active composition did not emit one empty finalized payload");
        require(structure_events_->count(StructureChangeType_ChildrenInvalidated) == 0,
                "action updates emitted a broad ChildrenInvalidated structure event");
    }

    void verifyNoStructureEvents() const {
        require(structure_events_, "structure event handler was not registered");
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
        while (std::chrono::steady_clock::now() < deadline) {
            require(structure_events_->total() == 0,
                    "a non-structural action emitted a delayed structure event");
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        require(structure_events_->total() == 0,
                "focus, invoke, toggle, selection, text, or value action emitted " +
                std::to_string(structure_events_->total()) + " structure events");
    }

    void verifyChildAdded(IUIAutomationElement *child, int timeout_ms) const {
        require(structure_events_ && child, "structure event handler or expanded child is unavailable");
        const std::vector<int> child_runtime_id = runtimeId(child);
        const bool exact = waitUntil(timeout_ms, [&] {
            return structure_events_->hasExactChange(
                StructureChangeType_ChildAdded,
                child_runtime_id
            );
        });
        require(exact, "expanded child emitted no exact ChildAdded event from the added element");
        require(structure_events_->count(StructureChangeType_ChildRemoved) == 0 &&
                structure_events_->count(StructureChangeType_ChildrenReordered) == 0 &&
                structure_events_->count(StructureChangeType_ChildrenInvalidated) == 0,
                "expansion emitted a non-additive structure delta");
    }

private:
    struct AutomationRegistration {
        EVENTID event;
        ComPtr<IUIAutomationElement> element;
    };

    void addAutomation(IUIAutomationElement *element, EVENTID event) {
        requireHr(automation_->AddAutomationEventHandler(event, element, TreeScope_Element, nullptr, automation_events_.Get()),
                  "AddAutomationEventHandler(" + std::to_string(event) + ")");
        automation_registrations_.push_back({ event, element });
    }

    void addProperties(IUIAutomationElement *element, const std::vector<PROPERTYID> &properties) {
        SAFEARRAY *array = SafeArrayCreateVector(VT_I4, 0, static_cast<ULONG>(properties.size()));
        require(array != nullptr, "could not allocate property event array");
        for (LONG index = 0; index < static_cast<LONG>(properties.size()); ++index) {
            LONG property = properties[static_cast<size_t>(index)];
            const HRESULT hr = SafeArrayPutElement(array, &index, &property);
            if (FAILED(hr)) {
                SafeArrayDestroy(array);
                requireHr(hr, "SafeArrayPutElement(property event)");
            }
        }
        const HRESULT hr = automation_->AddPropertyChangedEventHandler(element, TreeScope_Element, nullptr, property_events_.Get(), array);
        SafeArrayDestroy(array);
        requireHr(hr, "AddPropertyChangedEventHandler");
        property_registrations_.push_back(element);
    }

    IUIAutomation *automation_ = nullptr;
    ComPtr<IUIAutomation3> automation3_;
    ComPtr<AutomationEventRecorder> automation_events_;
    ComPtr<PropertyEventRecorder> property_events_;
    ComPtr<TextEditEventRecorder> text_edit_events_;
    ComPtr<StructureEventRecorder> structure_events_;
    ComPtr<IUIAutomationElement> root_;
    ComPtr<IUIAutomationElement> text_element_;
    std::vector<AutomationRegistration> automation_registrations_;
    std::vector<ComPtr<IUIAutomationElement>> property_registrations_;
    bool focus_registered_ = false;
    bool text_edit_registered_ = false;
    bool structure_registered_ = false;
};

struct Results {
    int passed = 0;
    int failed = 0;

    void run(const std::string &name, const std::function<void()> &check) {
        try {
            check();
            ++passed;
            std::cout << "PASS " << name << '\n';
        } catch (const std::exception &error) {
            ++failed;
            std::cerr << "FAIL " << name << ": " << error.what() << '\n';
        } catch (...) {
            ++failed;
            std::cerr << "FAIL " << name << ": unknown exception\n";
        }
    }
};

void checkHierarchy(const Options &options, const Connection &connection) {
    auto list = requireElement(connection.automation.Get(), connection.root.Get(), options.list, options.timeout_ms);
    auto item = requireElement(connection.automation.Get(), connection.root.Get(), options.item, options.timeout_ms);
    expectControlType(connection.root.Get(), UIA_GroupControlTypeId, options.root.describe());
    expectControlType(list.Get(), UIA_ListControlTypeId, options.list.describe());
    expectControlType(item.Get(), UIA_ListItemControlTypeId, options.item.describe());
    auto list_parent = rawParent(connection.automation.Get(), list.Get());
    auto item_parent = rawParent(connection.automation.Get(), item.Get());
    require(list_parent && sameElement(connection.automation.Get(), list_parent.Get(), connection.root.Get()),
            options.list.describe() + " is not an immediate child of " + options.root.describe());
    require(item_parent && sameElement(connection.automation.Get(), item_parent.Get(), list.Get()),
            options.item.describe() + " is not an immediate child of " + options.list.describe());
}

void checkVirtualization(const Options &options, const Connection &connection) {
    auto list = requireElement(connection.automation.Get(), connection.root.Get(), options.list, options.timeout_ms);
    auto item = requireElement(connection.automation.Get(), list.Get(), options.item, options.timeout_ms);
    auto above = requireElement(connection.automation.Get(), list.Get(), options.above, options.timeout_ms);
    const auto initial_list_runtime_id = runtimeId(list.Get());
    const auto initial_item_runtime_id = runtimeId(item.Get());
    const auto initial_above_runtime_id = runtimeId(above.Get());
    const auto initial_above_automation_id = currentAutomationId(above.Get());
    require(intProperty(item.Get(), UIA_PositionInSetPropertyId) == options.expected_position,
            "PositionInSet is not " + std::to_string(options.expected_position));
    require(intProperty(item.Get(), UIA_SizeOfSetPropertyId) == options.expected_size,
            "SizeOfSet is not " + std::to_string(options.expected_size));

    auto scroll = requirePattern<IUIAutomationScrollPattern>(
        list.Get(), UIA_ScrollPatternId, IID_IUIAutomationScrollPattern, "Scroll(Lesson list)"
    );
    BOOL horizontally_scrollable = TRUE;
    BOOL vertically_scrollable = FALSE;
    double horizontal_percent = 0;
    double horizontal_view_size = 0;
    double initial_vertical_percent = 0;
    double vertical_view_size = 0;
    requireHr(scroll->get_CurrentHorizontallyScrollable(&horizontally_scrollable), "Scroll.get_CurrentHorizontallyScrollable");
    requireHr(scroll->get_CurrentVerticallyScrollable(&vertically_scrollable), "Scroll.get_CurrentVerticallyScrollable");
    requireHr(scroll->get_CurrentHorizontalScrollPercent(&horizontal_percent), "Scroll.get_CurrentHorizontalScrollPercent");
    requireHr(scroll->get_CurrentHorizontalViewSize(&horizontal_view_size), "Scroll.get_CurrentHorizontalViewSize");
    requireHr(scroll->get_CurrentVerticalScrollPercent(&initial_vertical_percent), "Scroll.get_CurrentVerticalScrollPercent");
    requireHr(scroll->get_CurrentVerticalViewSize(&vertical_view_size), "Scroll.get_CurrentVerticalViewSize");
    const double expected_initial_percent = (1320.0 / (32000.0 - 80.0)) * 100.0;
    const double expected_view_size = (80.0 / 32000.0) * 100.0;
    require(horizontally_scrollable == FALSE, "Lesson list incorrectly reports horizontal scrolling");
    require(vertically_scrollable != FALSE, "Lesson list does not report vertical scrolling");
    require(approximately(horizontal_percent, kNoScrollPercent),
            "Lesson list horizontal scroll percent is not UIA_ScrollPatternNoScroll");
    require(approximately(horizontal_view_size, 100.0), "Lesson list horizontal view size is not 100 percent");
    require(approximately(initial_vertical_percent, expected_initial_percent, 0.01),
            "Lesson list initial vertical scroll percent is " + std::to_string(initial_vertical_percent) +
            ", expected " + std::to_string(expected_initial_percent));
    require(approximately(vertical_view_size, expected_view_size, 0.01),
            "Lesson list vertical view size is " + std::to_string(vertical_view_size) +
            ", expected " + std::to_string(expected_view_size));

    VARIANT control_type;
    VariantInit(&control_type);
    control_type.vt = VT_I4;
    control_type.lVal = UIA_ListItemControlTypeId;
    ComPtr<IUIAutomationCondition> condition;
    requireHr(connection.automation->CreatePropertyCondition(UIA_ControlTypePropertyId, control_type, &condition), "CreatePropertyCondition(ListItem)");
    auto materializedCount = [&]() {
        ComPtr<IUIAutomationElementArray> items;
        requireHr(list->FindAll(TreeScope_Descendants, condition.Get(), &items), "FindAll(list items)");
        int count = 0;
        requireHr(items->get_Length(&count), "list item array length");
        return count;
    };
    auto requireBoundedMaterialization = [&]() {
        const int count = materializedCount();
        require(count <= options.max_materialized,
                "virtual list materialized " + std::to_string(count) + " rows; cap is " + std::to_string(options.max_materialized));
        require(count > 0, "virtual list exposed no materialized rows");
    };
    requireBoundedMaterialization();

    for (int attempt = 0; attempt < 4; ++attempt) {
        auto current = requireElement(connection.automation.Get(), list.Get(), options.item, options.timeout_ms);
        require(runtimeId(current.Get()) == initial_item_runtime_id, "Lesson 42 runtime ID changed between queries");
        require(intProperty(current.Get(), UIA_PositionInSetPropertyId) == options.expected_position,
                "Lesson 42 PositionInSet changed between queries");
        require(intProperty(current.Get(), UIA_SizeOfSetPropertyId) == options.expected_size,
                "Lesson 42 SizeOfSet changed between queries");
    }

    requireHr(scroll->Scroll(ScrollAmount_NoAmount, ScrollAmount_LargeIncrement), "Scroll.Scroll(LargeIncrement)");
    double scrolled_vertical_percent = initial_vertical_percent;
    ComPtr<IUIAutomationElement> replacement;
    const bool scrolled = waitUntil(options.timeout_ms, [&] {
        try {
            if (FAILED(scroll->get_CurrentVerticalScrollPercent(&scrolled_vertical_percent)) ||
                scrolled_vertical_percent <= initial_vertical_percent) return false;
            if (findElement(connection.automation.Get(), list.Get(), options.above)) return false;
            replacement = findElement(connection.automation.Get(), list.Get(), options.replacement);
            auto current_item = findElement(connection.automation.Get(), list.Get(), options.item);
            return replacement && current_item && runtimeId(current_item.Get()) == initial_item_runtime_id;
        } catch (...) {
            return false;
        }
    });
    require(scrolled, "large vertical scroll did not replace Lesson 40 with Lesson 48 while retaining Lesson 42");
    require(runtimeId(list.Get()) == initial_list_runtime_id, "Lesson list runtime ID changed after scrolling");
    require(intProperty(replacement.Get(), UIA_PositionInSetPropertyId) == 48,
            "Lesson 48 PositionInSet is not 48");
    require(intProperty(replacement.Get(), UIA_SizeOfSetPropertyId) == options.expected_size,
            "Lesson 48 SizeOfSet changed after scrolling");
    requireBoundedMaterialization();

    BSTR stale_name = nullptr;
    HRESULT stale_hr = above->get_CurrentName(&stale_name);
    SysFreeString(stale_name);
    require(unavailable(stale_hr),
            "retained Lesson 40 returned " + hexHr(stale_hr) + " instead of becoming unavailable");

    requireHr(scroll->SetScrollPercent(kNoScrollPercent, initial_vertical_percent),
              "Scroll.SetScrollPercent(initial)");
    ComPtr<IUIAutomationElement> restored_above;
    const bool restored = waitUntil(options.timeout_ms, [&] {
        try {
            double vertical_percent = 0;
            if (FAILED(scroll->get_CurrentVerticalScrollPercent(&vertical_percent)) ||
                !approximately(vertical_percent, initial_vertical_percent, 0.01)) return false;
            restored_above = findElement(connection.automation.Get(), list.Get(), options.above);
            if (!restored_above || findElement(connection.automation.Get(), list.Get(), options.replacement)) return false;
            auto current_item = findElement(connection.automation.Get(), list.Get(), options.item);
            return current_item && runtimeId(current_item.Get()) == initial_item_runtime_id;
        } catch (...) {
            return false;
        }
    });
    require(restored, "restoring the initial scroll percent did not rematerialize Lesson 40 and remove Lesson 48");
    require(runtimeId(list.Get()) == initial_list_runtime_id, "Lesson list runtime ID changed after restoring its scroll position");
    require(currentAutomationId(restored_above.Get()) == initial_above_automation_id,
            "rematerialized Lesson 40 did not reuse its semantic AutomationId");
    require(runtimeId(restored_above.Get()) != initial_above_runtime_id,
            "rematerialized Lesson 40 reused its removed provider RuntimeId");
    requireBoundedMaterialization();

    stale_name = nullptr;
    stale_hr = above->get_CurrentName(&stale_name);
    SysFreeString(stale_name);
    require(unavailable(stale_hr),
            "retained Lesson 40 became available again after rematerialization");
}

void checkPatterns(const Options &options, const Connection &connection) {
    auto list = requireElement(connection.automation.Get(), connection.root.Get(), options.list, options.timeout_ms);
    auto item = requireElement(connection.automation.Get(), list.Get(), options.item, options.timeout_ms);
    auto invoke = requireElement(connection.automation.Get(), connection.root.Get(), options.invoke, options.timeout_ms);
    auto toggle = requireElement(connection.automation.Get(), connection.root.Get(), options.toggle, options.timeout_ms);
    auto select = requireElement(connection.automation.Get(), connection.root.Get(), options.select, options.timeout_ms);
    auto text = requireElement(connection.automation.Get(), connection.root.Get(), options.text, options.timeout_ms);
    auto range = requireElement(connection.automation.Get(), connection.root.Get(), options.range_value, options.timeout_ms);
    auto progress = requireElement(connection.automation.Get(), connection.root.Get(), options.progress, options.timeout_ms);
    auto details = requireElement(connection.automation.Get(), connection.root.Get(), options.details, options.timeout_ms);
    auto grid = requireElement(connection.automation.Get(), connection.root.Get(), options.grid, options.timeout_ms);
    auto row = requireElement(connection.automation.Get(), connection.root.Get(), options.grid_row, options.timeout_ms);
    auto cell = requireElement(connection.automation.Get(), connection.root.Get(), options.grid_cell, options.timeout_ms);
    auto state_cell = requireElement(connection.automation.Get(), connection.root.Get(), options.state_cell, options.timeout_ms);

    expectControlType(invoke.Get(), UIA_ButtonControlTypeId, options.invoke.describe());
    expectControlType(toggle.Get(), UIA_CheckBoxControlTypeId, options.toggle.describe());
    expectControlType(select.Get(), UIA_RadioButtonControlTypeId, options.select.describe());
    expectControlType(text.Get(), UIA_EditControlTypeId, options.text.describe());
    expectControlType(range.Get(), UIA_SliderControlTypeId, options.range_value.describe());
    expectControlType(progress.Get(), UIA_ProgressBarControlTypeId, options.progress.describe());
    expectControlType(grid.Get(), UIA_DataGridControlTypeId, options.grid.describe());
    expectControlType(row.Get(), UIA_DataItemControlTypeId, options.grid_row.describe());
    expectControlType(cell.Get(), UIA_DataItemControlTypeId, options.grid_cell.describe());
    expectControlType(state_cell.Get(), UIA_DataItemControlTypeId, options.state_cell.describe());

    (void)requirePattern<IUIAutomationInvokePattern>(invoke.Get(), UIA_InvokePatternId, IID_IUIAutomationInvokePattern, "Invoke");
    (void)requirePattern<IUIAutomationTogglePattern>(toggle.Get(), UIA_TogglePatternId, IID_IUIAutomationTogglePattern, "Toggle");
    auto selection_pattern = requirePattern<IUIAutomationSelectionItemPattern>(select.Get(), UIA_SelectionItemPatternId, IID_IUIAutomationSelectionItemPattern, "SelectionItem");
    (void)requirePattern<IUIAutomationValuePattern>(text.Get(), UIA_ValuePatternId, IID_IUIAutomationValuePattern, "Value");
    (void)requirePattern<IUIAutomationTextPattern>(text.Get(), UIA_TextPatternId, IID_IUIAutomationTextPattern, "Text");
    (void)requirePattern<IUIAutomationTextEditPattern>(text.Get(), UIA_TextEditPatternId, IID_IUIAutomationTextEditPattern, "TextEdit");
    (void)requirePattern<IUIAutomationRangeValuePattern>(range.Get(), UIA_RangeValuePatternId, IID_IUIAutomationRangeValuePattern, "RangeValue");
    (void)requirePattern<IUIAutomationExpandCollapsePattern>(details.Get(), UIA_ExpandCollapsePatternId, IID_IUIAutomationExpandCollapsePattern, "ExpandCollapse");
    (void)requirePattern<IUIAutomationScrollPattern>(list.Get(), UIA_ScrollPatternId, IID_IUIAutomationScrollPattern, "Scroll(Lesson list)");
    (void)requirePattern<IUIAutomationScrollItemPattern>(item.Get(), UIA_ScrollItemPatternId, IID_IUIAutomationScrollItemPattern, "ScrollItem(Lesson 42)");
    require(!hasPattern(select.Get(), UIA_InvokePatternId, IID_IUIAutomationInvokePattern),
            "Reading mode incorrectly exposes Invoke in addition to SelectionItem");
    require(!hasPattern(details.Get(), UIA_TogglePatternId, IID_IUIAutomationTogglePattern),
            "Details incorrectly exposes Toggle in addition to ExpandCollapse");

    ComPtr<IUIAutomationElement> selection_container;
    requireHr(selection_pattern->get_CurrentSelectionContainer(&selection_container), "SelectionItem.get_CurrentSelectionContainer");
    auto selection_parent = rawParent(connection.automation.Get(), select.Get());
    require(selection_container && selection_parent &&
            sameElement(connection.automation.Get(), selection_container.Get(), selection_parent.Get()),
            "Reading mode reports the wrong SelectionContainer");
    (void)requirePattern<IUIAutomationSelectionPattern>(selection_container.Get(), UIA_SelectionPatternId, IID_IUIAutomationSelectionPattern, "Selection container");

    auto progress_pattern = requirePattern<IUIAutomationRangeValuePattern>(progress.Get(), UIA_RangeValuePatternId, IID_IUIAutomationRangeValuePattern, "read-only RangeValue");
    BOOL progress_read_only = FALSE;
    double progress_current = 0;
    requireHr(progress_pattern->get_CurrentIsReadOnly(&progress_read_only), "progress RangeValue.get_CurrentIsReadOnly");
    requireHr(progress_pattern->get_CurrentValue(&progress_current), "progress RangeValue.get_CurrentValue");
    require(progress_read_only != FALSE, "Completion advertises a writable RangeValue pattern");
    require(approximately(progress_current, options.progress_value), "Completion value is not " + std::to_string(options.progress_value));
    const HRESULT write_hr = progress_pattern->SetValue(options.progress_value + 0.01);
    require(write_hr == UIA_E_INVALIDOPERATION || write_hr == E_ACCESSDENIED,
            "writing Completion returned " + hexHr(write_hr) + ", expected a read-only failure");

    auto grid_pattern = requirePattern<IUIAutomationGridPattern>(grid.Get(), UIA_GridPatternId, IID_IUIAutomationGridPattern, "Grid");
    int rows = 0;
    int columns = 0;
    requireHr(grid_pattern->get_CurrentRowCount(&rows), "Grid.get_CurrentRowCount");
    requireHr(grid_pattern->get_CurrentColumnCount(&columns), "Grid.get_CurrentColumnCount");
    require(rows == 1 && columns == 2, "Lesson grid dimensions are " + std::to_string(rows) + "x" + std::to_string(columns) + ", expected 1x2");

    auto cell_pattern = requirePattern<IUIAutomationGridItemPattern>(cell.Get(), UIA_GridItemPatternId, IID_IUIAutomationGridItemPattern, "GridItem(Lesson cell)");
    auto state_pattern = requirePattern<IUIAutomationGridItemPattern>(state_cell.Get(), UIA_GridItemPatternId, IID_IUIAutomationGridItemPattern, "GridItem(State cell)");
    int row_index = -1;
    int column_index = -1;
    requireHr(cell_pattern->get_CurrentRow(&row_index), "Lesson cell GridItem.get_CurrentRow");
    requireHr(cell_pattern->get_CurrentColumn(&column_index), "Lesson cell GridItem.get_CurrentColumn");
    require(row_index == 0 && column_index == 0, "Lesson cell is not at row 0, column 0");
    requireHr(state_pattern->get_CurrentRow(&row_index), "State cell GridItem.get_CurrentRow");
    requireHr(state_pattern->get_CurrentColumn(&column_index), "State cell GridItem.get_CurrentColumn");
    require(row_index == 0 && column_index == 1, "State cell is not at row 0, column 1");
    ComPtr<IUIAutomationElement> containing_grid;
    requireHr(cell_pattern->get_CurrentContainingGrid(&containing_grid), "Lesson cell GridItem.get_CurrentContainingGrid");
    require(containing_grid && sameElement(connection.automation.Get(), containing_grid.Get(), grid.Get()), "Lesson cell reports the wrong containing grid");

    auto row_parent = rawParent(connection.automation.Get(), row.Get());
    auto cell_parent = rawParent(connection.automation.Get(), cell.Get());
    auto state_parent = rawParent(connection.automation.Get(), state_cell.Get());
    require(row_parent && sameElement(connection.automation.Get(), row_parent.Get(), grid.Get()), "Lesson row is not an immediate child of Lesson grid");
    require(cell_parent && sameElement(connection.automation.Get(), cell_parent.Get(), row.Get()), "Lesson cell is not an immediate child of Lesson row");
    require(state_parent && sameElement(connection.automation.Get(), state_parent.Get(), row.Get()), "State cell is not an immediate child of Lesson row");
}

long rectWidth(const RECT &rect) { return rect.right - rect.left; }
long rectHeight(const RECT &rect) { return rect.bottom - rect.top; }

bool positiveIntersection(const RECT &left, const RECT &right) {
    return std::max(left.left, right.left) < std::min(left.right, right.right) &&
           std::max(left.top, right.top) < std::min(left.bottom, right.bottom);
}

void checkBounds(const Options &options, const Connection &connection) {
    auto list = requireElement(connection.automation.Get(), connection.root.Get(), options.list, options.timeout_ms);
    auto above = requireElement(connection.automation.Get(), connection.root.Get(), options.above, options.timeout_ms);
    auto partial = requireElement(connection.automation.Get(), connection.root.Get(), options.item, options.timeout_ms);
    auto full = requireElement(connection.automation.Get(), connection.root.Get(), options.full_row, options.timeout_ms);
    const RECT list_rect = currentRect(list.Get());
    const RECT above_rect = currentRect(above.Get());
    const RECT partial_rect = currentRect(partial.Get());
    const RECT full_rect = currentRect(full.Get());

    require(rectWidth(list_rect) > 0 && rectHeight(list_rect) > 0, "Lesson list has empty bounds");
    require(currentOffscreen(above.Get()), "Lesson 40 is fully clipped but IsOffscreen is false");
    require(!positiveIntersection(above_rect, list_rect), "Lesson 40 bounds intersect the list viewport");
    require(!currentOffscreen(partial.Get()), "Lesson 42 is partially visible but IsOffscreen is true");
    require(rectWidth(partial_rect) > 0 && rectHeight(partial_rect) > 0, "Lesson 42 has empty clipped bounds");
    require(partial_rect.left >= list_rect.left && partial_rect.right <= list_rect.right &&
            partial_rect.top >= list_rect.top && partial_rect.bottom <= list_rect.bottom,
            "Lesson 42 bounds are not clipped to the Lesson list viewport");
    require(std::abs(partial_rect.top - list_rect.top) <= 1, "Lesson 42 clipped top does not align with the list viewport top");
    require(!currentOffscreen(full.Get()), "Lesson 43 should be visible");
    require(rectHeight(full_rect) > rectHeight(partial_rect), "Lesson 42 is not shorter than an unclipped lesson row");
}

void checkFocus(const Options &options, const Connection &connection) {
    auto target = requireElement(connection.automation.Get(), connection.root.Get(), options.text, options.timeout_ms);
    requireHr(target->SetFocus(), "Search lessons SetFocus");
    const bool focused = waitUntil(options.timeout_ms, [&] {
        BOOL has_focus = FALSE;
        if (FAILED(target->get_CurrentHasKeyboardFocus(&has_focus)) || !has_focus) return false;
        ComPtr<IUIAutomationElement> current;
        if (FAILED(connection.automation->GetFocusedElement(&current)) || !current) return false;
        try { return sameElement(connection.automation.Get(), target.Get(), current.Get()); }
        catch (...) { return false; }
    });
    require(focused, "focus did not round-trip to Search lessons");
}

void checkInvoke(const Options &options, const Connection &connection) {
    requireStatus(connection.automation.Get(), connection.root.Get(), options.count_before, options.timeout_ms);
    auto target = requireElement(connection.automation.Get(), connection.root.Get(), options.invoke, options.timeout_ms);
    auto pattern = requirePattern<IUIAutomationInvokePattern>(target.Get(), UIA_InvokePatternId, IID_IUIAutomationInvokePattern, "Invoke");
    requireHr(pattern->Invoke(), "Count action Invoke");
    requireStatus(connection.automation.Get(), connection.root.Get(), options.count_after, options.timeout_ms);
}

void checkToggle(const Options &options, const Connection &connection) {
    requireStatus(connection.automation.Get(), connection.root.Get(), options.toggle_before, options.timeout_ms);
    auto target = requireElement(connection.automation.Get(), connection.root.Get(), options.toggle, options.timeout_ms);
    auto pattern = requirePattern<IUIAutomationTogglePattern>(target.Get(), UIA_TogglePatternId, IID_IUIAutomationTogglePattern, "Toggle");
    ToggleState before = ToggleState_Indeterminate;
    requireHr(pattern->get_CurrentToggleState(&before), "Toggle.get_CurrentToggleState(before)");
    require(before == ToggleState_Off, "Study mode is not initially off");
    requireHr(pattern->Toggle(), "Study mode Toggle");
    const bool toggled = waitUntil(options.timeout_ms, [&] {
        ToggleState state = ToggleState_Indeterminate;
        return SUCCEEDED(pattern->get_CurrentToggleState(&state)) && state == ToggleState_On;
    });
    require(toggled, "Study mode did not become on");
    requireStatus(connection.automation.Get(), connection.root.Get(), options.toggle_after, options.timeout_ms);
}

void checkSelect(const Options &options, const Connection &connection) {
    requireStatus(connection.automation.Get(), connection.root.Get(), options.select_before, options.timeout_ms);
    auto target = requireElement(connection.automation.Get(), connection.root.Get(), options.select, options.timeout_ms);
    auto pattern = requirePattern<IUIAutomationSelectionItemPattern>(target.Get(), UIA_SelectionItemPatternId, IID_IUIAutomationSelectionItemPattern, "SelectionItem");
    BOOL before = TRUE;
    requireHr(pattern->get_CurrentIsSelected(&before), "SelectionItem.get_CurrentIsSelected(before)");
    require(before == FALSE, "Reading mode is initially selected");
    requireHr(pattern->Select(), "Reading mode Select");
    const bool selected = waitUntil(options.timeout_ms, [&] {
        BOOL value = FALSE;
        return SUCCEEDED(pattern->get_CurrentIsSelected(&value)) && value != FALSE;
    });
    require(selected, "Reading mode did not become selected");
    requireStatus(connection.automation.Get(), connection.root.Get(), options.select_after, options.timeout_ms);
}

void checkText(const Options &options, const Connection &connection) {
    requireStatus(connection.automation.Get(), connection.root.Get(), options.text_before, options.timeout_ms);
    auto target = requireElement(connection.automation.Get(), connection.root.Get(), options.text, options.timeout_ms);
    auto value = requirePattern<IUIAutomationValuePattern>(target.Get(), UIA_ValuePatternId, IID_IUIAutomationValuePattern, "Value");
    auto text = requirePattern<IUIAutomationTextPattern>(target.Get(), UIA_TextPatternId, IID_IUIAutomationTextPattern, "Text");
    auto text_edit = requirePattern<IUIAutomationTextEditPattern>(target.Get(), UIA_TextEditPatternId, IID_IUIAutomationTextEditPattern, "TextEdit");
    require(valueText(value.Get()) == options.initial_text, "Search lessons initial Value is not " + narrow(options.initial_text));
    ComPtr<IUIAutomationTextRangeArray> initial_selection_ranges;
    requireHr(text->GetSelection(&initial_selection_ranges), "Text.GetSelection(initial retained range)");
    auto retained_selection = firstTextRange(initial_selection_ranges.Get(), "initial retained text selection");
    require(rangeText(retained_selection.Get()) == substring(options.initial_text, options.initial_selection), "Search lessons initial selection is incorrect");
    ComPtr<IUIAutomationTextRange> composition;
    requireHr(text_edit->GetActiveComposition(&composition), "TextEdit.GetActiveComposition");
    require(composition != nullptr, "Search lessons exposes no active composition");
    require(rangeText(composition.Get()) == substring(options.initial_text, options.initial_composition), "Search lessons composition range is incorrect");

    BSTR replacement = SysAllocStringLen(options.set_text.data(), static_cast<UINT>(options.set_text.size()));
    require(replacement != nullptr || options.set_text.empty(), "could not allocate Search lessons replacement text");
    const HRESULT set_hr = value->SetValue(replacement);
    SysFreeString(replacement);
    requireHr(set_hr, "Search lessons Value.SetValue");
    const bool updated = waitUntil(options.timeout_ms, [&] {
        try { return valueText(value.Get()) == options.set_text; }
        catch (...) { return false; }
    });
    require(updated, "Search lessons Value did not become " + narrow(options.set_text));
    requireStatus(connection.automation.Get(), connection.root.Get(), options.text_after, options.timeout_ms);
    require(wellFormedUtf16(rangeText(retained_selection.Get())), "retained Search lessons range split a UTF-16 surrogate pair after replacement");
    ComPtr<IUIAutomationTextRange> ended_composition;
    requireHr(text_edit->GetActiveComposition(&ended_composition), "TextEdit.GetActiveComposition(after replacement)");
    require(ended_composition == nullptr, "Search lessons composition remained active after replacing its value");
    if (options.default_unicode_text) {
        expectCharacterUnits(text.Get(), {
            L"\U0001f469\u200d\U0001f4bb",
            L"e\u0301",
            L"Z",
        });
    }
    selectText(text.Get(), options.set_selection);
    const std::wstring expected_selection = options.default_unicode_text || options.set_selection_text_explicit
        ? options.set_selection_text
        : substring(options.set_text, options.set_selection);
    const bool selection_updated = waitUntil(options.timeout_ms, [&] {
        try { return selectedText(text.Get()) == expected_selection; }
        catch (...) { return false; }
    });
    require(selection_updated, "Search lessons selection did not become " + narrow(expected_selection));
}

void checkValue(const Options &options, const Connection &connection) {
    requireStatus(connection.automation.Get(), connection.root.Get(), options.value_before, options.timeout_ms);
    auto target = requireElement(connection.automation.Get(), connection.root.Get(), options.range_value, options.timeout_ms);
    auto pattern = requirePattern<IUIAutomationRangeValuePattern>(target.Get(), UIA_RangeValuePatternId, IID_IUIAutomationRangeValuePattern, "RangeValue");
    BOOL read_only = TRUE;
    double current = 0;
    double small_change = 0;
    requireHr(pattern->get_CurrentIsReadOnly(&read_only), "Volume RangeValue.get_CurrentIsReadOnly");
    requireHr(pattern->get_CurrentValue(&current), "Volume RangeValue.get_CurrentValue(before)");
    requireHr(pattern->get_CurrentSmallChange(&small_change), "Volume RangeValue.get_CurrentSmallChange");
    require(read_only == FALSE, "Volume RangeValue is read-only");
    require(approximately(current, options.initial_range_value), "Volume initial value is not " + std::to_string(options.initial_range_value));
    require(small_change > 0, "Volume RangeValue exposes no positive step");
    requireHr(pattern->SetValue(options.set_range_value), "Volume RangeValue.SetValue");
    const bool updated = waitUntil(options.timeout_ms, [&] {
        double value = 0;
        return SUCCEEDED(pattern->get_CurrentValue(&value)) && approximately(value, options.set_range_value);
    });
    require(updated, "Volume did not become " + std::to_string(options.set_range_value));
    requireStatus(connection.automation.Get(), connection.root.Get(), options.value_after, options.timeout_ms);
}

void checkExpand(const Options &options, const Connection &connection) {
    requireStatus(connection.automation.Get(), connection.root.Get(), options.details_before, options.timeout_ms);
    auto target = requireElement(connection.automation.Get(), connection.root.Get(), options.details, options.timeout_ms);
    auto pattern = requirePattern<IUIAutomationExpandCollapsePattern>(target.Get(), UIA_ExpandCollapsePatternId, IID_IUIAutomationExpandCollapsePattern, "ExpandCollapse");
    ExpandCollapseState before = ExpandCollapseState_LeafNode;
    requireHr(pattern->get_CurrentExpandCollapseState(&before), "ExpandCollapse.get_CurrentExpandCollapseState(before)");
    require(before == ExpandCollapseState_Collapsed, "Details is not initially collapsed");
    requireHr(pattern->Expand(), "Details Expand(first)");
    requireHr(pattern->Expand(), "Details Expand(second idempotent request)");
    const bool expanded = waitUntil(options.timeout_ms, [&] {
        ExpandCollapseState state = ExpandCollapseState_LeafNode;
        return SUCCEEDED(pattern->get_CurrentExpandCollapseState(&state)) && state == ExpandCollapseState_Expanded;
    });
    require(expanded, "Details did not become expanded");
    requireStatus(connection.automation.Get(), connection.root.Get(), options.details_after, options.timeout_ms);
}

void checkStale(const Options &options, const Connection &connection) {
    requireStatus(connection.automation.Get(), connection.root.Get(), options.transient_before, options.timeout_ms);
    auto transient = requireElement(connection.automation.Get(), connection.root.Get(), options.transient, options.timeout_ms);
    const std::vector<int> transient_runtime_id = runtimeId(transient.Get());
    const std::wstring transient_automation_id = currentAutomationId(transient.Get());
    auto transient_value = requirePattern<IUIAutomationValuePattern>(
        transient.Get(), UIA_ValuePatternId, IID_IUIAutomationValuePattern, "Value(Transient target)"
    );
    auto transient_text = requirePattern<IUIAutomationTextPattern>(
        transient.Get(), UIA_TextPatternId, IID_IUIAutomationTextPattern, "Text(Transient target)"
    );
    ComPtr<IUIAutomationTextRange> transient_range;
    requireHr(transient_text->get_DocumentRange(&transient_range), "Transient target Text.get_DocumentRange");
    require(transient_range != nullptr, "Transient target has no document range");
    auto transient_parent = rawParent(connection.automation.Get(), transient.Get());
    require(transient_parent != nullptr, "Transient target has no raw-view parent");
    const std::vector<int> transient_parent_runtime_id = runtimeId(transient_parent.Get());
    auto remove = requireElement(connection.automation.Get(), connection.root.Get(), options.remove_transient, options.timeout_ms);
    auto remove_pattern = requirePattern<IUIAutomationInvokePattern>(remove.Get(), UIA_InvokePatternId, IID_IUIAutomationInvokePattern, "Invoke(Remove transient)");
    StructureEventSession structure_events;
    structure_events.start(connection.automation.Get(), connection.root.Get());
    requireHr(remove_pattern->Invoke(), "Remove transient Invoke");
    requireStatus(connection.automation.Get(), connection.root.Get(), options.transient_after, options.timeout_ms);
    const bool absent = waitUntil(options.timeout_ms, [&] {
        try { return !findElement(connection.automation.Get(), connection.root.Get(), options.transient); }
        catch (...) { return false; }
    });
    require(absent, "Transient target remains discoverable after removal");
    BSTR stale_name = nullptr;
    HRESULT stale_hr = transient->get_CurrentName(&stale_name);
    SysFreeString(stale_name);
    require(unavailable(stale_hr), "retained Transient target returned " + hexHr(stale_hr) + " instead of becoming unavailable");
    const bool child_removed = waitUntil(options.timeout_ms, [&] {
        return structure_events.events() && structure_events.events()->hasExactChange(
            StructureChangeType_ChildRemoved,
            transient_parent_runtime_id,
            transient_runtime_id
        );
    });
    require(child_removed, "removing Transient target emitted no exact ChildRemoved event from its parent");
    require(structure_events.events()->count(StructureChangeType_ChildrenInvalidated) == 0,
            "removing Transient target emitted ChildrenInvalidated instead of ChildRemoved");
    require(structure_events.events()->invalidRemovedRuntimeIds() == 0,
            "Transient target ChildRemoved event omitted its runtime ID");
    structure_events.stop();

    HRESULT stale_value_hr = setValue(transient_value.Get(), L"stale");
    require(unavailable(stale_value_hr),
            "retained Transient target Value.SetValue returned " + hexHr(stale_value_hr) + " after removal");
    BSTR stale_text = nullptr;
    HRESULT stale_range_hr = transient_range->GetText(-1, &stale_text);
    SysFreeString(stale_text);
    require(unavailable(stale_range_hr),
            "retained Transient target text range returned " + hexHr(stale_range_hr) + " after removal");

    auto restore = requireElement(connection.automation.Get(), connection.root.Get(), options.restore_transient, options.timeout_ms);
    auto restore_pattern = requirePattern<IUIAutomationInvokePattern>(
        restore.Get(), UIA_InvokePatternId, IID_IUIAutomationInvokePattern, "Invoke(Restore transient)"
    );
    requireHr(restore_pattern->Invoke(), "Restore transient Invoke");
    requireStatus(connection.automation.Get(), connection.root.Get(), options.transient_before, options.timeout_ms);
    auto replacement = requireElement(connection.automation.Get(), connection.root.Get(), options.transient, options.timeout_ms);
    require(currentAutomationId(replacement.Get()) == transient_automation_id,
            "restored Transient target did not reuse its semantic AutomationId");
    require(runtimeId(replacement.Get()) != transient_runtime_id,
            "restored Transient target reused the removed provider RuntimeId");

    stale_name = nullptr;
    stale_hr = transient->get_CurrentName(&stale_name);
    SysFreeString(stale_name);
    require(unavailable(stale_hr),
            "retained Transient target became available again after semantic-ID reuse");
    stale_value_hr = setValue(transient_value.Get(), L"stale replacement");
    require(unavailable(stale_value_hr),
            "retained Transient target Value.SetValue reached its replacement");
    stale_text = nullptr;
    stale_range_hr = transient_range->GetText(-1, &stale_text);
    SysFreeString(stale_text);
    require(unavailable(stale_range_hr),
            "retained Transient target text range reached its replacement");

    auto replacement_value = requirePattern<IUIAutomationValuePattern>(
        replacement.Get(), UIA_ValuePatternId, IID_IUIAutomationValuePattern, "Value(restored Transient target)"
    );
    requireHr(setValue(replacement_value.Get(), L"restored"), "Restored Transient target Value.SetValue");
    const bool replacement_updated = waitUntil(options.timeout_ms, [&] {
        try { return valueText(replacement_value.Get()) == L"restored"; }
        catch (...) { return false; }
    });
    require(replacement_updated, "restored Transient target Value did not update");

    if (!options.close_host) return;
    auto retained_root = connection.root;
    auto retained_action = requireElement(connection.automation.Get(), connection.root.Get(), options.invoke, options.timeout_ms);
    auto retained_invoke = requirePattern<IUIAutomationInvokePattern>(retained_action.Get(), UIA_InvokePatternId, IID_IUIAutomationInvokePattern, "retained Invoke");
    require(PostMessageW(connection.hwnd, WM_CLOSE, 0, 0) != FALSE, "PostMessage(WM_CLOSE) failed");
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, options.pid);
    const bool closed = waitUntil(options.timeout_ms, [&] {
        if (!IsWindow(connection.hwnd)) return true;
        return process && WaitForSingleObject(process, 0) == WAIT_OBJECT_0;
    });
    if (process) CloseHandle(process);
    require(closed, "host did not close within " + std::to_string(options.timeout_ms) + " ms");

    HRESULT root_hr = S_OK;
    HRESULT invoke_hr = S_OK;
    const bool disconnected = waitUntil(options.timeout_ms, [&] {
        BSTR root_name = nullptr;
        root_hr = retained_root->get_CurrentName(&root_name);
        SysFreeString(root_name);
        invoke_hr = retained_invoke->Invoke();
        return unavailable(root_hr) && unavailable(invoke_hr);
    });
    require(disconnected, "retained providers did not disconnect within " + std::to_string(options.timeout_ms) + " ms");
    require(unavailable(root_hr), "retained root returned " + hexHr(root_hr) + " after host close");
    require(unavailable(invoke_hr), "retained action Invoke returned " + hexHr(invoke_hr) + " after host close");
}

void dumpTreeNode(IUIAutomation *automation, IUIAutomationTreeWalker *walker, IUIAutomationElement *element,
                  int depth, int &remaining) {
    if (remaining <= 0 || depth > 12) return;
    --remaining;
    try {
        const RECT rect = currentRect(element);
        std::cerr << "UIA " << std::string(static_cast<size_t>(depth * 2), ' ')
                  << controlTypeName(currentControlType(element))
                  << " name=\"" << narrow(currentName(element)) << "\""
                  << " id=\"" << narrow(currentAutomationId(element)) << "\""
                  << " offscreen=" << (currentOffscreen(element) ? "true" : "false")
                  << " rect=" << rect.left << ',' << rect.top << ',' << rect.right << ',' << rect.bottom << '\n';
    } catch (const std::exception &error) {
        std::cerr << "UIA " << std::string(static_cast<size_t>(depth * 2), ' ') << "<query failed: " << error.what() << ">\n";
    }
    ComPtr<IUIAutomationElement> child;
    if (FAILED(walker->GetFirstChildElement(element, &child))) return;
    while (child && remaining > 0) {
        dumpTreeNode(automation, walker, child.Get(), depth + 1, remaining);
        ComPtr<IUIAutomationElement> next;
        if (FAILED(walker->GetNextSiblingElement(child.Get(), &next))) break;
        child = std::move(next);
    }
}

void dumpTree(const Connection &connection, int limit) {
    if (!connection.automation || !connection.window || limit <= 0) return;
    ComPtr<IUIAutomationTreeWalker> walker;
    if (FAILED(connection.automation->get_RawViewWalker(&walker)) || !walker) return;
    int remaining = limit;
    std::cerr << "--- UI Automation raw tree (limit " << limit << ") ---\n";
    dumpTreeNode(connection.automation.Get(), walker.Get(), connection.window.Get(), 0, remaining);
}

int execute(const Options &options) {
    Connection connection = connect(options);
    Results results;
    const auto selected = [&](const std::string &name) { return options.checks.count(name) != 0; };
    const bool events_selected = selected("events");

    if (selected("discovery")) {
        results.run("discovery", [&] {
            require(!currentName(connection.root.Get()).empty(), "semantic root has no accessible name");
            require(currentControlType(connection.root.Get()) == UIA_GroupControlTypeId, "semantic root is not a Group");
        });
    }
    if (selected("hierarchy")) results.run("hierarchy", [&] { checkHierarchy(options, connection); });
    if (selected("virtualization")) results.run("virtualization", [&] { checkVirtualization(options, connection); });
    if (selected("patterns")) results.run("patterns", [&] { checkPatterns(options, connection); });
    if (selected("bounds")) results.run("bounds", [&] { checkBounds(options, connection); });

    PreciseEventSession events;
    bool events_started = false;
    if (events_selected) {
        results.run("events/setup", [&] {
            auto focus = requireElement(connection.automation.Get(), connection.root.Get(), options.text, options.timeout_ms);
            auto invoke = requireElement(connection.automation.Get(), connection.root.Get(), options.invoke, options.timeout_ms);
            auto toggle = requireElement(connection.automation.Get(), connection.root.Get(), options.toggle, options.timeout_ms);
            auto select = requireElement(connection.automation.Get(), connection.root.Get(), options.select, options.timeout_ms);
            auto range = requireElement(connection.automation.Get(), connection.root.Get(), options.range_value, options.timeout_ms);
            auto details = requireElement(connection.automation.Get(), connection.root.Get(), options.details, options.timeout_ms);
            events.start(connection.automation.Get(), connection.root.Get(), focus.Get(), invoke.Get(), toggle.Get(), select.Get(), focus.Get(), range.Get(), details.Get());
            events_started = true;
        });
    }

    auto actionName = [&](const std::string &name) {
        return selected(name) ? name : "events/" + name;
    };
    if (selected("focus") || events_selected) results.run(actionName("focus"), [&] { checkFocus(options, connection); });
    if (selected("invoke") || events_selected) results.run(actionName("invoke"), [&] { checkInvoke(options, connection); });
    if (selected("toggle") || events_selected) results.run(actionName("toggle"), [&] { checkToggle(options, connection); });
    if (selected("select") || events_selected) results.run(actionName("select"), [&] { checkSelect(options, connection); });
    if (selected("text") || events_selected) results.run(actionName("text"), [&] { checkText(options, connection); });
    if (selected("value") || events_selected) results.run(actionName("value"), [&] { checkValue(options, connection); });
    if (events_selected) results.run("events/non-structural-deltas", [&] { events.verifyNoStructureEvents(); });
    if (selected("expand") || events_selected) results.run(actionName("expand"), [&] { checkExpand(options, connection); });
    if (events_selected) {
        results.run("events/expanded-structure", [&] {
            auto child = requireElement(connection.automation.Get(), connection.root.Get(), options.expanded_child, options.timeout_ms);
            events.verifyChildAdded(child.Get(), options.timeout_ms);
        });
        results.run("events/precise-deltas", [&] {
            require(events_started, "event setup failed");
            events.verify(options.timeout_ms);
        });
        events.stop();
    }

    if (selected("stale")) results.run("stale", [&] { checkStale(options, connection); });

    std::cout << "SUMMARY passed=" << results.passed << " failed=" << results.failed << '\n';
    if (results.failed != 0) dumpTree(connection, options.dump_limit);
    return results.failed == 0 ? 0 : 1;
}

} // namespace

int wmain(int argc, wchar_t **argv) {
    const HRESULT com_hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(com_hr)) {
        std::cerr << "FAIL setup: CoInitializeEx failed with " << hexHr(com_hr) << '\n';
        return 2;
    }
    int result = 2;
    try {
        result = execute(parseOptions(argc, argv));
    } catch (const std::exception &error) {
        std::cerr << "FAIL setup: " << error.what() << '\n';
    } catch (...) {
        std::cerr << "FAIL setup: unknown exception\n";
    }
    CoUninitialize();
    return result;
}
