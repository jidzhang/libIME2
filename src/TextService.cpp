//
//    Copyright (C) 2013 - 2020 Hong Jen Yee (PCMan) <pcman.tw@gmail.com>
//
//    This library is free software; you can redistribute it and/or
//    modify it under the terms of the GNU Library General Public
//    License as published by the Free Software Foundation; either
//    version 2 of the License, or (at your option) any later version.
//
//    This library is distributed in the hope that it will be useful,
//    but WITHOUT ANY WARRANTY; without even the implied warranty of
//    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
//    Library General Public License for more details.
//
//    You should have received a copy of the GNU Library General Public
//    License along with this library; if not, write to the
//    Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
//    Boston, MA  02110-1301, USA.
//

#include "TextService.h"
#include "DebugLogConfig.h"
#include "DebugLogFile.h"
#include "EditSession.h"
#include "CandidateWindow.h"
#include "LangBarButton.h"
#include "DisplayAttributeInfoEnum.h"
#include "ImeModule.h"

#include <assert.h>
#include <fstream>
#include <sstream>
#include <string>
#include <algorithm>
#include <imm.h>

using namespace std;

namespace Ime {

namespace {

constexpr wchar_t kDummyAnchorText[] = L"\x200B";

std::wstring timestampNow() {
    SYSTEMTIME st{};
    ::GetLocalTime(&st);
    wchar_t buffer[64] = {0};
    _snwprintf_s(buffer, _countof(buffer), _TRUNCATE,
        L"%04u-%02u-%02u %02u:%02u:%02u.%03u",
        st.wYear, st.wMonth, st.wDay,
        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    return buffer;
}

std::wstring currentProcessPath() {
    std::wstring buffer(MAX_PATH, L'\0');
    DWORD len = ::GetModuleFileNameW(nullptr, &buffer[0], static_cast<DWORD>(buffer.size()));
    if (len == 0) {
        return L"";
    }
    while (len >= buffer.size() - 1) {
        buffer.resize(buffer.size() * 2);
        len = ::GetModuleFileNameW(nullptr, &buffer[0], static_cast<DWORD>(buffer.size()));
        if (len == 0) {
            return L"";
        }
    }
    buffer.resize(len);
    return buffer;
}

std::wstring processBaseName(const std::wstring& imagePath) {
    const size_t pos = imagePath.find_last_of(L"\\/");
    return pos == std::wstring::npos ? imagePath : imagePath.substr(pos + 1);
}

std::wstring formatTextServiceDebugLogLine(const std::wstring& message) {
    const std::wstring exeName = processBaseName(currentProcessPath());
    std::wostringstream line;
    line << L"[" << timestampNow() << L"]"
         << L"[pid=" << ::GetCurrentProcessId() << L"]"
         << L"[tid=" << ::GetCurrentThreadId() << L"]"
         << L"[exe=" << (exeName.empty() ? L"<unknown>" : exeName) << L"] "
         << message;
    return line.str();
}

void appendTextServiceDebugLog(const std::wstring& message) {
    if (!isTraceLoggingEnabled()) {
        return;
    }

    const wchar_t* localAppData = _wgetenv(L"LOCALAPPDATA");
    if (!localAppData || !*localAppData) {
        return;
    }

    std::wstring logDir = std::wstring(localAppData) + L"\\MoqiIM\\Log";
    std::wstring logPath = DebugLogFile::prepareDailyLogFilePath(
        logDir, L"tsf-debug.log");
    if (logPath.empty()) {
        return;
    }

    std::wofstream stream(logPath, std::ios::app);
    if (!stream.is_open()) {
        return;
    }
    stream << message << L"\n";
}

void logTextServiceDebug(const std::wstring& message) {
    if (!isTraceLoggingEnabled()) {
        return;
    }
    const std::wstring formatted = formatTextServiceDebugLogLine(message);
    ::OutputDebugStringW((formatted + L"\n").c_str());
    appendTextServiceDebugLog(formatted);
}

std::wstring boolText(bool value) {
    return value ? L"true" : L"false";
}

std::wstring rectToString(const RECT& rect) {
    std::wostringstream stream;
    stream << L"(" << rect.left << L"," << rect.top << L"," << rect.right << L"," << rect.bottom << L")";
    return stream.str();
}

bool tryAdjustRectWithForegroundCaret(RECT* rect) {
    if (!rect) {
        return false;
    }

    const HWND foregroundWindow = ::GetForegroundWindow();
    if (!foregroundWindow) {
        return false;
    }

    RECT foregroundRect = {};
    if (!::GetWindowRect(foregroundWindow, &foregroundRect)) {
        return false;
    }

    if (rect->left >= foregroundRect.left && rect->left <= foregroundRect.right &&
        rect->top >= foregroundRect.top && rect->top <= foregroundRect.bottom) {
        return false;
    }

    POINT caret = {};
    const bool hasCaret = ::GetCaretPos(&caret) != FALSE;
    const int offsetX = foregroundRect.left - rect->left + (hasCaret ? caret.x : 0);
    const int offsetY = foregroundRect.top - rect->top + (hasCaret ? caret.y : 0);
    ::OffsetRect(rect, offsetX, offsetY);
    return true;
}

bool getRawTextExtRect(ITfContext* context, TfEditCookie editCookie, ITfRange* range, RECT* rect) {
    if (!context || !range || !rect) {
        return false;
    }

    ComPtr<ITfContextView> view;
    if (context->GetActiveView(&view) != S_OK) {
        return false;
    }

    BOOL clipped = FALSE;
    if (view->GetTextExt(editCookie, range, rect, &clipped) != S_OK) {
        return false;
    }
    return true;
}

bool getTextExtRect(ITfContext* context, TfEditCookie editCookie, ITfRange* range, RECT* rect) {
    if (!getRawTextExtRect(context, editCookie, range, rect)) {
        return false;
    }

    // Some fullscreen/game hosts report coordinates relative to the foreground
    // window instead of screen coordinates. Follow Weasel and correct them using
    // the foreground window origin and caret position when the rect is clearly out
    // of bounds.
    tryAdjustRectWithForegroundCaret(rect);
    return true;
}

bool getForegroundWindowRect(RECT* rect) {
    if (!rect) {
        return false;
    }

    const HWND foregroundWindow = ::GetForegroundWindow();
    return foregroundWindow && ::GetWindowRect(foregroundWindow, rect);
}

int absoluteDiff(int lhs, int rhs) {
    return lhs > rhs ? lhs - rhs : rhs - lhs;
}

// 系统 UI 字体（菜单/消息框字体）行高，供"宿主字体取不到"时的参照上界。
// 真实插入符高度 = 其所在行高，不会比系统 UI 行高还高 3 倍；自绘宿主把
// caret 高度报成整个输入框高度时（内网通实测 100px = 框高），用这个值兜底。
int systemUiLineHeight() {
    static int cachedLineHeight = 0;
    if (cachedLineHeight > 0) {
        return cachedLineHeight;
    }

    int lineHeight = 0;
    NONCLIENTMETRICSW metrics{};
    metrics.cbSize = sizeof(metrics);
    if (::SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0)) {
        const HDC dc = ::GetDC(nullptr);
        if (dc != nullptr) {
            const HFONT font = ::CreateFontIndirectW(&metrics.lfMessageFont);
            if (font != nullptr) {
                const HFONT oldFont = (HFONT)::SelectObject(dc, font);
                TEXTMETRIC textMetrics{};
                if (::GetTextMetrics(dc, &textMetrics)) {
                    lineHeight = textMetrics.tmHeight + textMetrics.tmExternalLeading;
                }
                ::SelectObject(dc, oldFont);
                ::DeleteObject(font);
            }
            ::ReleaseDC(nullptr, dc);
        }
    }
    if (lineHeight <= 0) {
        lineHeight = 16;
    }
    cachedLineHeight = lineHeight;
    return cachedLineHeight;
}

// 矩形高度是否"像整个输入框"而不是"像一行文字"。只在插入符高度已经可疑
// 时用于交叉校验 TSF 矩形，正常宿主不受影响。
bool looksLikeHostBox(const RECT& rect) {
    return (rect.bottom - rect.top) > systemUiLineHeight() * 3;
}

// 纯 IMM 宿主（wezterm/Alacritty/kitty 类终端）的光标锚点：这类宿主既无系统
// 插入符也无 TSF 文档，光标位置只通过 IMM32 旧通道告知系统——
// ImmSetCandidateWindow（wezterm：CFS_EXCLUDE + ptCurrentPos=光标格原点 +
// rcArea=光标格矩形，客户区坐标）/ ImmSetCompositionWindow。CUAS 不把这些
// 位置映射进 GetTextExt（实测 composition=none、selection=屏幕右下角 1x0
// 废矩形），只能由我们在宿主进程内直接读回。前提：TSF 组字会话与宿主同
// UI 线程，ImmGetContext 恰好只对本线程窗口有效，两者天然成立。
// 只接受宿主明确声明过位置的形态（CFS_CANDIDATEPOS/CFS_EXCLUDE、
// CFS_POINT/CFS_FORCE_POSITION）；从未设置过的上下文是 CFS_DEFAULT，不采用。
bool immAnchorScreenRect(RECT* rect, std::wstring* source) {
    if (!rect) {
        return false;
    }
    const HWND focus = ::GetFocus();
    if (!focus) {
        return false;
    }
    const HIMC himc = ::ImmGetContext(focus);
    if (!himc) {
        return false;
    }

    bool ok = false;
    CANDIDATEFORM cf{};
    cf.dwIndex = 0;
    if (::ImmGetCandidateWindow(himc, 0, &cf) &&
        (cf.dwStyle & (CFS_CANDIDATEPOS | CFS_EXCLUDE)) != 0) {
        if ((cf.dwStyle & CFS_EXCLUDE) != 0 &&
            cf.rcArea.right > cf.rcArea.left && cf.rcArea.bottom > cf.rcArea.top) {
            POINT topLeft{cf.rcArea.left, cf.rcArea.top};
            POINT bottomRight{cf.rcArea.right, cf.rcArea.bottom};
            if (::ClientToScreen(focus, &topLeft) && ::ClientToScreen(focus, &bottomRight)) {
                rect->left = topLeft.x;
                rect->top = topLeft.y;
                rect->right = bottomRight.x;
                rect->bottom = bottomRight.y;
                ok = true;
                if (source) {
                    *source = L"candidate-exclude";
                }
            }
        } else {
            POINT pt{cf.ptCurrentPos.x, cf.ptCurrentPos.y};
            if (::ClientToScreen(focus, &pt)) {
                rect->left = pt.x;
                rect->top = pt.y;
                rect->right = pt.x;
                rect->bottom = pt.y + systemUiLineHeight();
                ok = true;
                if (source) {
                    *source = L"candidate-pos";
                }
            }
        }
    }
    if (!ok) {
        COMPOSITIONFORM compositionForm{};
        if (::ImmGetCompositionWindow(himc, &compositionForm) &&
            (compositionForm.dwStyle & (CFS_POINT | CFS_FORCE_POSITION | CFS_RECT)) != 0) {
            if ((compositionForm.dwStyle & CFS_RECT) != 0 &&
                compositionForm.rcArea.right > compositionForm.rcArea.left &&
                compositionForm.rcArea.bottom > compositionForm.rcArea.top) {
                // CFS_RECT：组字窗约束在 rcArea 内，锚点取其首行
                POINT topLeft{compositionForm.rcArea.left, compositionForm.rcArea.top};
                POINT bottomRight{compositionForm.rcArea.right, compositionForm.rcArea.bottom};
                if (::ClientToScreen(focus, &topLeft) && ::ClientToScreen(focus, &bottomRight)) {
                    rect->left = topLeft.x;
                    rect->top = topLeft.y;
                    rect->right = bottomRight.x;
                    rect->bottom = bottomRight.y;
                    ok = true;
                    if (source) {
                        *source = L"composition-rect";
                    }
                }
            } else {
                POINT pt{compositionForm.ptCurrentPos.x, compositionForm.ptCurrentPos.y};
                if (::ClientToScreen(focus, &pt)) {
                    rect->left = pt.x;
                    rect->top = pt.y;
                    rect->right = pt.x;
                    rect->bottom = pt.y + systemUiLineHeight();
                    ok = true;
                    if (source) {
                        *source = L"composition-point";
                    }
                }
            }
        }
    }
    ::ImmReleaseContext(focus, himc);
    return ok;
}

// 宿主 UI 线程内的真实插入符屏幕矩形，同时给出不可用原因。调用方约定
// rect.bottom=行底=候选窗顶边（"输入行下方"），否则候选窗压住当前输入行。
// GUITHREADINFO 自带插入符高度（=字体行高）；拿不到时用 caret 宿主窗口的
// 字体行高，最后退 16px。插入符位于客户区 (0,0)（文档起始）是合法值。
//
// 只有本线程真有系统插入符（hwndCaret != null）才返回 true；Electron 等
// 自绘 caret 宿主无系统插入符，调用方走 TSF 矩形路径。hwndCaret 归属不在
// 聚焦/前景链、或换算坐标落到工作区之外时同样拒绝——ClientToScreen 对已
// 销毁/隐藏宿主能"成功"吐出废坐标，采用它就是候选窗钉左上角的来源。
//
// heightSuspicious（可空）：宿主把 caret 高度报成整个输入框高度时为 true，
// 此时返回的 rect 已按系统 UI 行高折行处理，调用方应优先改用 TSF 行矩形
// （见 inputRect）。内网通实测：宽 1px 打字列正确、高 100px = 框高、窗口
// 未设字体（WM_GETFONT 空），只有这条兜底能吃到。
bool realCaretScreenRect(RECT* rect, const wchar_t** whyNot, bool* heightSuspicious) {
    static const wchar_t kNoCaret[] = L"no system caret";
    static const wchar_t kForeignOwner[] = L"caret owned by another window";
    static const wchar_t kOffScreen[] = L"caret outside work area";
    if (whyNot) {
        *whyNot = kNoCaret;
    }
    if (heightSuspicious) {
        *heightSuspicious = false;
    }
    if (!rect) {
        return false;
    }

    const DWORD currentThreadId = ::GetCurrentThreadId();
    GUITHREADINFO guiInfo{};
    guiInfo.cbSize = sizeof(guiInfo);
    HWND caretOwner = nullptr;
    POINT caretTop{};
    int caretHeight = 0;
    if (::GetGUIThreadInfo(currentThreadId, &guiInfo)) {
        caretOwner = guiInfo.hwndCaret;
        if (caretOwner != nullptr) {
            caretTop = {guiInfo.rcCaret.left, guiInfo.rcCaret.top};
            caretHeight = guiInfo.rcCaret.bottom - guiInfo.rcCaret.top;
        }
    }
    if (caretOwner == nullptr) {
        // 本 UI 线程没有系统插入符（自绘 caret 宿主）：不猜，交给 TSF 路径。
        return false;
    }

    // 归属校验：插入符必须属于当前聚焦/前景窗口，或其父链上有本线程的可见
    // 顶层窗（TSF 按键/edit session 与焦点窗同线程的典型情况）。否则就是别
    // 处残留的插入符，其 ClientToScreen 坐标是废的，拒绝采用。
    const HWND focus = ::GetFocus();
    const HWND foreground = ::GetForegroundWindow();
    bool owned = caretOwner == focus || caretOwner == foreground;
    for (HWND candidate = caretOwner; !owned && candidate != nullptr;
         candidate = ::GetParent(candidate)) {
        DWORD windowThreadId = 0;
        ::GetWindowThreadProcessId(candidate, &windowThreadId);
        if (windowThreadId == currentThreadId && ::IsWindowVisible(candidate)) {
            owned = true;
        }
    }
    if (!owned) {
        if (whyNot) {
            *whyNot = kForeignOwner;
        }
        return false;
    }

    if (caretHeight <= 0) {
        caretHeight = 16;
    }
    {
        // 自绘宿主可能把 caret 高度报成整个输入框的高度（内网通实测：
        // 宽 1px 打字列正确、高 100px = 框高）。真插入符绝不会高于其行高，
        // 用窗口字体实测行高做上界钳制；取不到字体时不动（零回归：
        // 记事本类宿主 caret 高度本就等于行高，钳制是空操作）。
        int lineHeight = 0;
        const HFONT font = (HFONT)::SendMessage(caretOwner, WM_GETFONT, 0, 0);
        if (font != nullptr) {
            const HDC dc = ::GetDC(caretOwner);
            if (dc != nullptr) {
                const HFONT oldFont = (HFONT)::SelectObject(dc, font);
                TEXTMETRIC metrics{};
                if (::GetTextMetrics(dc, &metrics) && metrics.tmHeight > 0) {
                    lineHeight = metrics.tmHeight + metrics.tmExternalLeading;
                }
                ::SelectObject(dc, oldFont);
                ::ReleaseDC(caretOwner, dc);
            }
        }
        if (lineHeight > 0) {
            if (caretHeight <= 0 || caretHeight > lineHeight) {
                caretHeight = lineHeight;
            }
        } else if (caretHeight <= 0) {
            caretHeight = 16;
        }
    }
    {
        // 第二道（不依赖宿主字体）：真插入符绝不会比系统 UI 行高还高 3 倍。
        // 命中即判定"宿主报的是整个输入框"，按系统行高折行，并把可疑标记
        // 交给调用方去优先采用 TSF 行矩形。
        const int referenceLineHeight = systemUiLineHeight();
        if (caretHeight > referenceLineHeight * 3) {
            caretHeight = referenceLineHeight;
            if (heightSuspicious) {
                *heightSuspicious = true;
            }
        }
    }

    if (!::ClientToScreen(caretOwner, &caretTop)) {
        return false;
    }

    // 成功换算后必须仍在某显示器工作区内，否则视为无 caret，回 TSF 路径，
    // 不污染 last-good。
    const HMONITOR caretMonitor = ::MonitorFromPoint(caretTop, MONITOR_DEFAULTTONULL);
    if (caretMonitor == nullptr) {
        if (whyNot) {
            *whyNot = kOffScreen;
        }
        return false;
    }
    MONITORINFO monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (::GetMonitorInfo(caretMonitor, &monitorInfo)) {
        const RECT work = monitorInfo.rcWork;
        if (caretTop.x < work.left || caretTop.x > work.right ||
            caretTop.y < work.top || caretTop.y > work.bottom) {
            if (whyNot) {
                *whyNot = kOffScreen;
            }
            return false;
        }
    }

    rect->left = caretTop.x;
    rect->right = caretTop.x;
    rect->top = caretTop.y;
    rect->bottom = caretTop.y + caretHeight;
    return true;
}

int inputRectScore(const RECT& rect, const RECT* foregroundRect, bool preferSelection) {
    int score = preferSelection ? 1 : 0;
    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;

    if (width < 0 || height < 0) {
        score -= 100;
    } else {
        if (width == 0) {
            score -= 10;
        }
        if (height == 0) {
            score -= 30;
        } else if (height < 8) {
            score -= 10;
        }
    }

    if (!foregroundRect) {
        return score;
    }

    const bool insideForeground =
        rect.left >= foregroundRect->left && rect.left <= foregroundRect->right &&
        rect.top >= foregroundRect->top && rect.top <= foregroundRect->bottom;
    if (insideForeground) {
        score += 40;
    } else {
        score -= 20;
    }

    const bool tinyTopLeftMarker =
        absoluteDiff(rect.left, foregroundRect->left) <= 48 &&
        absoluteDiff(rect.top, foregroundRect->top) <= 48 &&
        width <= 4 &&
        height > 0 && height <= 48;
    if (tinyTopLeftMarker) {
        score -= 80;
    }

    return score;
}

} // namespace

TextService::TextService(ImeModule* module):
    module_(module),
    displayAttributeProvider_{ComPtr<DisplayAttributeProvider>::make(module)},
    threadMgr_(NULL),
    clientId_(TF_CLIENTID_NULL),
    activateFlags_(0),
    isKeyboardOpened_(false),
    testKeyDownPending_(false),
    testKeyUpPending_(false),
    dummyCompositionActive_(false),
    autoDummyCompositionAnchorEnabled_(false),
    lastKnownGoodInputRect_{},
    hasLastKnownGoodInputRect_(false),
    langBarSinkCookie_(TF_INVALID_COOKIE) {

}

TextService::~TextService(void) {
    if(langBarMgr_) {
        langBarMgr_->UnadviseEventSink(langBarSinkCookie_);
    }
}

// public methods

// language bar
DWORD TextService::langBarStatus() const {
    if(langBarMgr_) {
        DWORD status;
        if(langBarMgr_->GetShowFloatingStatus(&status) == S_OK) {
            return status;
        }
    }
    return 0;
}

void TextService::addButton(LangBarButton* button) {
    if(button) {
        langBarButtons_.emplace_back(button);
        if(isActivated()) {
            if(auto langBarItemMgr = threadMgr_.query<ITfLangBarItemMgr>()) {
                langBarItemMgr->AddItem(button);
            }
        }
    }
}

void TextService::removeButton(LangBarButton* button) {
    if(button) {
        auto it = find(langBarButtons_.begin(), langBarButtons_.end(), button);
        if(it != langBarButtons_.end()) {
            // 调用方之一是 Client 析构（发生在失活进行中，isActivated 已为
            // false）：只要还能拿到 ITfLangBarItemMgr 就必须 RemoveItem，
            // 否则 msctf 持有的引用永不释放（2026-10-24 语言栏按钮泄漏）。
            if(auto langBarItemMgr = threadMgr_.query<ITfLangBarItemMgr>()) {
                langBarItemMgr->RemoveItem(button);
            }
            langBarButtons_.erase(it);
        }
    }
}

// preserved key
void TextService::addPreservedKey(UINT keyCode, UINT modifiers, const GUID& guid) {
    PreservedKey preservedKey;
    preservedKey.guid = guid;
    preservedKey.uVKey = keyCode;
    preservedKey.uModifiers = modifiers;
    preservedKeys_.push_back(preservedKey);
    if(threadMgr_) { // our text service is activated
        if (auto keystrokeMgr = threadMgr_.query<ITfKeystrokeMgr>()) {
            keystrokeMgr->PreserveKey(clientId_, guid, &preservedKey, NULL, 0);
        }
    }
}

void TextService::removePreservedKey(const GUID& guid) {
    auto it = std::find_if(preservedKeys_.begin(), preservedKeys_.end(),
        [&](const auto& preservedKey) {
            return ::IsEqualIID(preservedKey.guid, guid);
        }
    );
    if (it != preservedKeys_.end()) {
        if (auto keystrokeMgr = threadMgr_.query<ITfKeystrokeMgr>()) {
            auto& preservedKey = *it;
            keystrokeMgr->UnpreserveKey(preservedKey.guid, &preservedKey);
        }
        preservedKeys_.erase(it);
    }
}


// text composition

bool TextService::isComposing() const {
    return (composition_ != nullptr);
}

// is keyboard disabled for the context (NULL means current context)
bool TextService::isKeyboardDisabled(ITfContext* context) const {
    return (contextCompartmentValue(GUID_COMPARTMENT_KEYBOARD_DISABLED, context)
        || contextCompartmentValue(GUID_COMPARTMENT_EMPTYCONTEXT, context));
}

// is keyboard opened for the whole thread
bool TextService::isKeyboardOpened() const {
    return isKeyboardOpened_;
}

void TextService::setKeyboardOpen(bool open) {
    if(open != isKeyboardOpened_) {
        // The event handler triggered by the compartment value will update isKeyboardOpened_.
        setThreadCompartmentValue(GUID_COMPARTMENT_KEYBOARD_OPENCLOSE, DWORD(open));
    }
}

// check if current insertion point is in the range of composition.
// if not in range, insertion is now allowed
bool TextService::isInsertionAllowed(EditSession* session) const {
    TfEditCookie cookie = session->editCookie();
    ULONG selectionNum;
    if(isComposing()) {
        TF_SELECTION selection;
        if(session->context()->GetSelection(cookie, TF_DEFAULT_SELECTION, 1, &selection, &selectionNum) == S_OK) {
            ComPtr<ITfRange> compositionRange;
            if(composition_->GetRange(&compositionRange) == S_OK) {
                bool allowed = false;
                // check if current selection is covered by composition range
                LONG compareResult1;
                LONG compareResult2;
                if(selection.range->CompareStart(cookie, compositionRange, TF_ANCHOR_START, &compareResult1) == S_OK
                    && selection.range->CompareStart(cookie, compositionRange, TF_ANCHOR_END, &compareResult2) == S_OK) {
                    if(compareResult1 == -1 && compareResult2 == +1)
                        allowed = true;
                }
            }
            selection.range->Release();
        }
    }
    return false;
}

void TextService::startComposition(ITfContext* context) {
    assert(context);
    HRESULT sessionResult;
    auto editSession = ComPtr<EditSession>::make(
        context,
        [=](EditSession* session, TfEditCookie cookie) {
            if (auto contextComposition = ComPtr<ITfContextComposition>::queryFrom(context)) {
                const bool inlinePreeditEnabled = inlinePreeditEnabledForComposition();
                const bool useDummyAnchor = !inlinePreeditEnabled || effectiveDummyCompositionAnchor();
                // get current insertion point in the current context
                ComPtr<ITfRange> range;
                if (auto insertAtSelection = ComPtr<ITfInsertAtSelection>::queryFrom(context)) {
                    // get current selection range & insertion position (query only, did not insert any text)
                    insertAtSelection->InsertTextAtSelection(cookie, TF_IAS_QUERYONLY, NULL, 0, &range);
                }

                if (range) {
                    composition_ = nullptr;
                    dummyCompositionActive_ = false;
                    hasLastKnownGoodInputRect_ = false;
                    hasCompositionAnchor_ = false;
                    if (contextComposition->StartComposition(cookie, range, (ITfCompositionSink*)this, &composition_) == S_OK) {
                        ComPtr<ITfRange> compositionRange;
                        if (composition_->GetRange(&compositionRange) != S_OK) {
                            compositionRange = range;
                        }

                        if (useDummyAnchor && compositionRange) {
                            // Some hosts do not provide a useful GetTextExt()
                            // anchor until the composition contains at least one
                            // character. Use a zero-width placeholder so the host
                            // creates an anchor without visible inline preedit.
                            if (compositionRange->SetText(
                                    cookie, TF_ST_CORRECTION, kDummyAnchorText, 1) == S_OK) {
                                dummyCompositionActive_ = true;
                                logTextServiceDebug(
                                    L"[startComposition] enabled zero-width dummy anchor");
                            }
                        }

                        // according to the official TSF samples, we need to reset the current
                        // selection here. (maybe the range is altered by StartComposition()?
                        TF_SELECTION selection;
                        if (compositionRange) {
                            if (inlinePreeditEnabled) {
                                compositionRange->Collapse(cookie, TF_ANCHOR_END);
                            } else {
                                compositionRange->Collapse(cookie, TF_ANCHOR_START);
                            }
                            selection.range = compositionRange;
                        } else {
                            selection.range = range;
                        }
                        selection.style.ase = TF_AE_NONE;
                        selection.style.fInterimChar = FALSE;
                        context->SetSelection(cookie, 1, &selection);
                        logTextServiceDebug(
                            L"[startComposition] started dummy_anchor=" +
                            boolText(dummyCompositionActive_) +
                            L" inline_preedit=" + boolText(inlinePreeditEnabled) +
                            L" dummy_requested=" + boolText(useDummyAnchor));
                    }
                }
            }
        }
    );
    context->RequestEditSession(clientId_, editSession, TF_ES_SYNC|TF_ES_READWRITE, &sessionResult);
}

static void logSelectionTrace(const wchar_t* tag, ITfContext* context, TfEditCookie cookie);

void TextService::endComposition(ITfContext* context, bool clearText) {
    assert(context);
    HRESULT sessionResult;
    auto editSession = ComPtr<EditSession>::make(        context,
        [=](EditSession* session, TfEditCookie cookie) {
            if (composition_) {
                // move current insertion point to end of the composition string
                ComPtr<ITfRange> compositionRange;
                if (composition_->GetRange(&compositionRange) == S_OK) {
                    if (dummyCompositionActive_) {
                        logTextServiceDebug(L"[endComposition] clearing dummy anchor");
                        compositionRange->SetText(cookie, 0, L"", 0);
                        dummyCompositionActive_ = false;
                    } else if (clearText) {
                        // "清空+普通插入"上屏路径：组字以空文本结束，
                        // 最终文本由 insertTextAtSelection 普通插入。
                        compositionRange->SetText(cookie, 0, L"", 0);
                        logTextServiceDebug(L"[endComposition] cleared composition text");
                    }

                    // clear display attribute for the composition range
                    ComPtr<ITfProperty> dispAttrProp;
                    if (context->GetProperty(GUID_PROP_ATTRIBUTE, &dispAttrProp) == S_OK) {
                        dispAttrProp->Clear(cookie, compositionRange);
                    }
                }

                // EndComposition must happen before the selection is collapsed:
                // XAML hosts (Sticky Notes, ...) reset the selection on the
                // end-composition notification and re-select the committed text,
                // overriding a selection that was set earlier in this edit
                // session. The composition range object stays valid after
                // EndComposition, so collapse afterwards instead.
                // end composition and clean up
                composition_->EndComposition(cookie);
                logSelectionTrace(L"endcomp-after-endcomp", context, cookie);

                if (compositionRange) {
                    TF_SELECTION selection;
                    ULONG selectionNum;
                    if (context->GetSelection(cookie, TF_DEFAULT_SELECTION, 1, &selection, &selectionNum) == S_OK) {
                        selection.range->ShiftEndToRange(cookie, compositionRange, TF_ANCHOR_END);
                        selection.range->Collapse(cookie, TF_ANCHOR_END);
                        context->SetSelection(cookie, 1, &selection);
                        selection.range->Release();
                    }
                }
                logSelectionTrace(L"endcomp-after-setselection", context, cookie);
                // do some cleanup in the derived class here
                onCompositionTerminated(false);
                composition_ = nullptr;
                dummyCompositionActive_ = false;
                hasLastKnownGoodInputRect_ = false;
                hasCompositionAnchor_ = false;
                logTextServiceDebug(L"[endComposition] ended dummy_anchor=false");
            }
        }
    );
    context->RequestEditSession(clientId_, editSession, TF_ES_SYNC|TF_ES_READWRITE, &sessionResult);
    collapseSelectionAfterCompositionEnd(context);
}

// 上屏选区追踪：dump 当前 TSF 选区（ACP 位置），定位宿主（便笺/XAML）把
// 选区改回"整段选中"发生在哪一步。trace 档才输出。
static void logSelectionTrace(const wchar_t* tag, ITfContext* context, TfEditCookie cookie) {
    if (!context) {
        logTextServiceDebug(std::wstring(L"[selTrace] ") + tag + L" no-context");
        return;
    }
    TF_SELECTION sel;
    ULONG num = 0;
    const HRESULT hr = context->GetSelection(cookie, TF_DEFAULT_SELECTION, 1, &sel, &num);
    if (hr != S_OK) {
        logTextServiceDebug(std::wstring(L"[selTrace] ") + tag +
                            L" GetSelection-failed hr=0x" + std::to_wstring(static_cast<unsigned long>(hr)));
        return;
    }
    std::wstring line = std::wstring(L"[selTrace] ") + tag + L" num=" + std::to_wstring(num);
    if (num >= 1 && sel.range) {
        ComPtr<ITfRangeACP> acp = ComPtr<ITfRangeACP>::queryFrom(sel.range);
        if (acp) {
            LONG start = 0, length = 0;
            if (acp->GetExtent(&start, &length) == S_OK) {
                line += L" extent=[" + std::to_wstring(start) + L"," + std::to_wstring(start + length) + L"]";
            }
        } else {
            line += L" non-acp-range";
        }
        sel.range->Release();
    }
    logTextServiceDebug(line);
}

// 折叠非退化选区的独立编辑会话。tag 仅用于日志。
static void requestSelectionCollapseSession(ITfContext* context, TfClientId clientId, DWORD flags, const wchar_t* tag) {
    HRESULT sessionResult;
    auto editSession = ComPtr<EditSession>::make(
        context,
        [=](EditSession* session, TfEditCookie cookie) {
            ITfContext* ctx = session->context();
            logSelectionTrace(tag, ctx, cookie);
            TF_SELECTION selection;
            ULONG selectionNum = 0;
            if (ctx->GetSelection(cookie, TF_DEFAULT_SELECTION, 1, &selection, &selectionNum) != S_OK) {
                logTextServiceDebug(std::wstring(L"[selTrace] collapse-") + tag + L" GetSelection-failed");
                return;
            }
            if (selectionNum != 1 || !selection.range) {
                if (selection.range) {
                    selection.range->Release();
                }
                logTextServiceDebug(std::wstring(L"[selTrace] collapse-") + tag +
                                    L" unexpected-num=" + std::to_wstring(selectionNum));
                return;
            }
            // 选区退化（已是插入点）就不用再设
            LONG degenerate = 1;
            if (selection.range->CompareStart(cookie, selection.range, TF_ANCHOR_END, &degenerate) == S_OK && degenerate == 0) {
                selection.range->Release();
                return;
            }
            // 折叠到右端（反向选区时右端是 start 锚）
            LONG reversed = 0;
            selection.range->CompareStart(cookie, selection.range, TF_ANCHOR_END, &reversed);
            selection.range->Collapse(cookie, reversed > 0 ? TF_ANCHOR_START : TF_ANCHOR_END);
            const HRESULT hr = ctx->SetSelection(cookie, 1, &selection);
            selection.range->Release();
            logTextServiceDebug(std::wstring(L"[selTrace] collapse-") + tag +
                                L" collapsed hr=0x" + std::to_wstring(static_cast<unsigned long>(hr)));
        }
    );
    context->RequestEditSession(clientId, editSession, flags, &sessionResult);
}

// XAML 宿主（便笺等）会把刚上屏的组字范围重新选中，同一会话内 SetSelection
// 的先后都会被覆盖。这里在 endComposition 的会话之后再开独立编辑会话兜底：
// 一个同步档（上一个会话的全部完成通知之后立即跑），一个异步档（等宿主
// 消息循环空闲再跑——覆盖宿主在 dispatcher 任务里异步重选的情形）。
// 正常宿主选区已是插入点，两个档都只做一次 GetSelection 即返回。
void TextService::collapseSelectionAfterCompositionEnd(ITfContext* context) {
    assert(context);
    requestSelectionCollapseSession(context, clientId_, TF_ES_SYNC | TF_ES_READWRITE, L"sync");
    requestSelectionCollapseSession(context, clientId_, TF_ES_ASYNCDONTCARE | TF_ES_READWRITE, L"async");
}

// 不经组字的普通文本插入：XAML 宿主（便笺等）对"组字带文本定稿"会把定稿
// 范围整段重新选中（用户 2026-10-23 实测，14 个输入法里仅墨奇触发）。
// 上屏改走 weasel#107 的老修法：组字清空后结束，最终文本用
// InsertTextAtSelection 普通插入——应用侧没有"组字定稿"事件可挑，选区由
// 我们在插入后显式设置为插入点。
void TextService::insertTextAtSelection(ITfContext* context, const wchar_t* str, int len, int caretFromEnd) {
    assert(context);
    HRESULT sessionResult;
    auto editSession = ComPtr<EditSession>::make(
        context,
        [=](EditSession* session, TfEditCookie cookie) {
            ITfContext* ctx = session->context();
            auto insertAtSelection = ComPtr<ITfInsertAtSelection>::queryFrom(ctx);
            if (!insertAtSelection) {
                logTextServiceDebug(L"[insertTextAtSelection] no ITfInsertAtSelection");
                return;
            }
            ComPtr<ITfRange> range;
            if (insertAtSelection->InsertTextAtSelection(cookie, 0, str, len, &range) != S_OK || !range) {
                logTextServiceDebug(L"[insertTextAtSelection] InsertTextAtSelection failed");
                return;
            }
            range->Collapse(cookie, TF_ANCHOR_END);
            if (caretFromEnd > 0) {
                LONG moved = 0;
                range->ShiftStart(cookie, -static_cast<LONG>(caretFromEnd), &moved, nullptr);
                range->Collapse(cookie, TF_ANCHOR_START);
            }
            TF_SELECTION selection;
            selection.range = range;
            selection.style.ase = TF_AE_NONE;
            selection.style.fInterimChar = FALSE;
            const HRESULT hr = ctx->SetSelection(cookie, 1, &selection);
            logTextServiceDebug(std::wstring(L"[insertTextAtSelection] done len=") +
                                std::to_wstring(len) + L" caret_from_end=" + std::to_wstring(caretFromEnd) +
                                L" set_selection_hr=0x" + std::to_wstring(static_cast<unsigned long>(hr)));
            logSelectionTrace(L"insert-done", ctx, cookie);
        }
    );
    context->RequestEditSession(clientId_, editSession, TF_ES_SYNC | TF_ES_READWRITE, &sessionResult);
}

std::wstring TextService::compositionString(EditSession* session) const {
    std::wstring result;
    if (dummyCompositionActive_) {
        return result;
    }
    if (composition_) {
        ComPtr<ITfRange> compositionRange;
        if (composition_->GetRange(&compositionRange) == S_OK) {
            auto rangeAcp = compositionRange.query<ITfRangeACP>();
            if (rangeAcp) {
                LONG anchor, bufLen;
                rangeAcp->GetExtent(&anchor, &bufLen);  // get length of the text.
                auto buf = std::make_unique<wchar_t[]>(size_t(bufLen) + 1);

                ULONG textLen = 0;
                if (compositionRange->GetText(session->editCookie(), 0, buf.get(), bufLen, &textLen) == S_OK) {
                    buf[textLen] = '\0';
                    result = buf.get();
                }
            }
        }
    }
    return result;
}

void TextService::setCompositionString(EditSession* session, const wchar_t* str, int len, bool isFinalCommit) const {
    ITfContext* context = session->context();
    if(context) {
        TfEditCookie editCookie = session->editCookie();
        TF_SELECTION selection;
        ULONG selectionNum;
        // get current selection/insertion point
        if(context->GetSelection(editCookie, TF_DEFAULT_SELECTION, 1, &selection, &selectionNum) == S_OK) {
            ComPtr<ITfRange> compositionRange;
            if(composition_->GetRange(&compositionRange) == S_OK) {
                bool selPosInComposition = true;
                // if current insertion point is not covered by composition, we cannot insert text here.
                if(selPosInComposition) {
                    const bool preserveDummyAnchor = dummyCompositionActive_ && len == 0;

                    // replace context of composion area with the new string.
                    if (!preserveDummyAnchor) {
                        // 组字中间更新用 TF_ST_CORRECTION（不打断撤销链）；最终上屏
                        // 文本必须用 0——XAML 宿主会把"correction"改动的范围重新
                        // 选中（便笺上屏后全选的来源之一）。
                        compositionRange->SetText(editCookie, isFinalCommit ? 0 : TF_ST_CORRECTION, str, len);
                        dummyCompositionActive_ = false;
                        // 外显文本落定后 dummy 锚无意义但 last-good 应保留——
                        // 上面 startComposition 处清零保证每轮组字都从新鲜矩形开始；
                        // 此处不清，保证长组字里 TSF 偶发坏矩形时有 last-good 可回退。
                        logTextServiceDebug(
                            L"[setCompositionString] updated len=" + std::to_wstring(len) +
                            L" dummy_anchor=false");
                    } else {
                        logTextServiceDebug(
                            L"[setCompositionString] preserved zero-width dummy anchor");
                    }

                    // move the insertion point to end of the composition string
                    if (!preserveDummyAnchor) {
                        selection.range->Collapse(editCookie, TF_ANCHOR_END);
                        context->SetSelection(editCookie, 1, &selection);
                        if (isFinalCommit) {
                            logSelectionTrace(L"commit-settext-done", context, editCookie);
                        }
                    }
                }

                // set display attribute to the composition range
                ComPtr<ITfProperty> dispAttrProp;
                if(context->GetProperty(GUID_PROP_ATTRIBUTE, &dispAttrProp) == S_OK) {
                    VARIANT val;
                    val.vt = VT_I4;
                    val.lVal = module_->inputAttrib()->atom();
                    dispAttrProp->SetValue(editCookie, compositionRange, &val);
                }
            }
            selection.range->Release();
        }
    }
}

bool TextService::inlinePreeditEnabledForComposition() const {
    return true;
}

bool TextService::shouldUseDummyCompositionAnchor() const {
    return false;
}

bool TextService::effectiveDummyCompositionAnchor() const {
    return dummyCompositionActive_ || autoDummyCompositionAnchorEnabled_ || shouldUseDummyCompositionAnchor();
}

bool TextService::ensureDummyCompositionAnchor(EditSession* session, const wchar_t* reason) const {
    if (!session || !composition_ || dummyCompositionActive_) {
        return false;
    }

    ComPtr<ITfRange> compositionRange;
    if (composition_->GetRange(&compositionRange) != S_OK || !compositionRange) {
        return false;
    }

    bool hasVisibleCompositionText = false;
    if (auto rangeAcp = compositionRange.query<ITfRangeACP>()) {
        LONG anchor = 0;
        LONG textLen = 0;
        if (rangeAcp->GetExtent(&anchor, &textLen) == S_OK) {
            hasVisibleCompositionText = textLen > 0;
        }
    }
    if (!hasVisibleCompositionText) {
        wchar_t buf[2] = {};
        ULONG textLen = 0;
        if (compositionRange->GetText(session->editCookie(), 0, buf, 1, &textLen) == S_OK) {
            hasVisibleCompositionText = textLen > 0;
        }
    }
    if (hasVisibleCompositionText) {
        return false;
    }

    if (compositionRange->SetText(session->editCookie(), TF_ST_CORRECTION, kDummyAnchorText, 1) != S_OK) {
        return false;
    }

    dummyCompositionActive_ = true;
    autoDummyCompositionAnchorEnabled_ = true;

    TF_SELECTION selection;
    if (inlinePreeditEnabledForComposition()) {
        compositionRange->Collapse(session->editCookie(), TF_ANCHOR_END);
    }
    else {
        compositionRange->Collapse(session->editCookie(), TF_ANCHOR_START);
    }
    selection.range = compositionRange;
    selection.style.ase = TF_AE_NONE;
    selection.style.fInterimChar = FALSE;
    session->context()->SetSelection(session->editCookie(), 1, &selection);

    logTextServiceDebug(
        L"[dummyAnchor] auto-enabled reason=" +
        std::wstring(reason ? reason : L"unknown") +
        L" inline_preedit=" + boolText(inlinePreeditEnabledForComposition()));
    return true;
}

// set cursor position in the composition area
// 0 means the start pos of composition string
void TextService::setCompositionCursor(EditSession* session, int pos) const {
    TF_SELECTION selection;
    ULONG selectionNum;
    // get current selection
    if(session->context()->GetSelection(session->editCookie(), TF_DEFAULT_SELECTION, 1, &selection, &selectionNum) == S_OK) {
        // get composition range
        ComPtr<ITfRange> compositionRange;
        if(composition_->GetRange(&compositionRange) == S_OK) {
            // make the start of selectionRange the same as that of compositionRange
            selection.range->ShiftStartToRange(session->editCookie(), compositionRange, TF_ANCHOR_START);
            selection.range->Collapse(session->editCookie(), TF_ANCHOR_START);
            LONG moved;
            // move the start anchor to right
            selection.range->ShiftStart(session->editCookie(), (LONG)pos, &moved, NULL);
            selection.range->Collapse(session->editCookie(), TF_ANCHOR_START);
            // set the new selection to the context
            session->context()->SetSelection(session->editCookie(), 1, &selection);
        }
        selection.range->Release();
    }
}

// compartment handling
ComPtr<ITfCompartment> TextService::globalCompartment(const GUID& key) const {
    ComPtr<ITfCompartment> compartment;
    auto threadMgr = threadMgr_;
    if (threadMgr == nullptr) {
        // if we don't have a thread manager (this is possible when we try to access
        // a global compartment while the text service is not activated)
        ::CoCreateInstance(CLSID_TF_ThreadMgr, NULL, CLSCTX_INPROC_SERVER, IID_ITfThreadMgr, (void**)&threadMgr);
    }
    if(threadMgr) {
        ComPtr<ITfCompartmentMgr> compartmentMgr;
        if(threadMgr_->GetGlobalCompartment(&compartmentMgr) == S_OK) {
            compartmentMgr->GetCompartment(key, &compartment);
        }
    }
    return compartment;
}

ComPtr<ITfCompartment> TextService::threadCompartment(const GUID& key) const {
    if(threadMgr_) {
        auto compartmentMgr = threadMgr_.query<ITfCompartmentMgr>();
        if(compartmentMgr) {
            ComPtr<ITfCompartment> compartment;
            compartmentMgr->GetCompartment(key, &compartment);
            return compartment;
        }
    }
    return nullptr;
}

ComPtr<ITfCompartment> TextService::contextCompartment(const GUID& key, ITfContext* context) const {
    ComPtr<ITfContext> curContext;
    if(!context) {
        curContext = currentContext();
        context = curContext;
    }
    if(context) {
        auto compartmentMgr = ComPtr<ITfCompartmentMgr>::queryFrom(context);
        if(compartmentMgr) {
            ComPtr<ITfCompartment> compartment;
            compartmentMgr->GetCompartment(key, &compartment);
            return compartment;
        }
    }
    return nullptr;
}

DWORD TextService::globalCompartmentValue(const GUID& key) const{
    if (auto compartment = globalCompartment(key)) {
        return compartmentValue(compartment);
    }
    return 0;
}

void TextService::setGlobalCompartmentValue(const GUID& key, DWORD value) const {
    if (auto compartment = globalCompartment(key)) {
        setCompartmentValue(compartment, value);
    };
}

DWORD TextService::contextCompartmentValue(const GUID& key, ITfContext* context) const {
    if (auto compartment = contextCompartment(key)) {
        return compartmentValue(compartment);
    }
    return 0;
}

void TextService::setContextCompartmentValue(const GUID& key, DWORD value, ITfContext* context) const {
    if (auto compartment = contextCompartment(key, context)) {
        setCompartmentValue(compartment, value);
    }
}

DWORD TextService::threadCompartmentValue(const GUID& key) const {
    if (auto compartment = threadCompartment(key)) {
        return compartmentValue(compartment);
    }
    return 0;
}

void TextService::setThreadCompartmentValue(const GUID& key, DWORD value) const {
    if (auto compartment = threadCompartment(key)) {
        setCompartmentValue(compartment, value);
    }
}

DWORD TextService::compartmentValue(ITfCompartment* compartment) const {
    VARIANT var;
    if (compartment->GetValue(&var) == S_OK && var.vt == VT_I4) {
        return (DWORD)var.lVal;
    }
    return 0;
}

void TextService::setCompartmentValue(ITfCompartment* compartment, DWORD value) const {
    VARIANT var;
    ::VariantInit(&var);
    var.vt = VT_I4;
    var.lVal = value;
    compartment->SetValue(clientId_, &var);
}

// virtual
void TextService::onActivate() {
}

// virtual
void TextService::onDeactivate() {
}

// virtual
bool TextService::filterKeyDown(KeyEvent& keyEvent) {
    return false;
}

// virtual
bool TextService::onKeyDown(KeyEvent& keyEvent, EditSession* session) {
    return false;
}

// virtual
bool TextService::filterKeyUp(KeyEvent& keyEvent) {
    return false;
}

// virtual
bool TextService::onKeyUp(KeyEvent& keyEvent, EditSession* session) {
    return false;
}

// virtual
bool TextService::onPreservedKey(const GUID& guid) {
    return false;
}

// virtual
void TextService::onSetFocus() {
}

// virtual
void TextService::onKillFocus() {
}

// virtual
void TextService::onSetThreadFocus() {
}

// virtual
void TextService::onKillThreadFocus() {
}

bool TextService::onCommand(UINT id, CommandType type) {
    return false;
}

// virtual
void TextService::onCompartmentChanged(const GUID& key) {
    // keyboard status changed, this is threadMgr specific
    // See explanations on TSF aware blog:
    // http://blogs.msdn.com/b/tsfaware/archive/2007/05/30/what-is-a-keyboard.aspx
    if(::IsEqualGUID(key, GUID_COMPARTMENT_KEYBOARD_OPENCLOSE)) {
        isKeyboardOpened_ = threadCompartmentValue(key) ? true : false;
        onKeyboardStatusChanged(isKeyboardOpened_);
    }
}

// virtual
void TextService::onLangBarStatusChanged(int newStatus) {
}

// called when the keyboard is opened or closed
// virtual
void TextService::onKeyboardStatusChanged(bool opened) {
}

// called just before current composition is terminated for doing cleanup.
// if forced is true, the composition is terminated by others, such as
// the input focus is grabbed by another application.
// if forced is false, the composition is terminated gracefully by endComposition().
// virtual
void TextService::onCompositionTerminated(bool forced) {
}

// called when a language profile is activated (only useful for text services that supports multiple language profiles)
void TextService::onLangProfileActivated(REFGUID guidProfile) {
}

// called when a language profile is deactivated
void TextService::onLangProfileDeactivated(REFGUID guidProfile) {
}

void TextService::onLayoutChange(ITfContext* context, TfLayoutCode code, ITfContextView* view) {
}

void TextService::initKeyboardState() {
    // get current keyboard state
    isKeyboardOpened_ = threadCompartmentValue(GUID_COMPARTMENT_KEYBOARD_OPENCLOSE) != 0;
    // FIXME: under Windows 7, it seems that the keyboard is closed every time
    // our text service is activated. The value in the compartment is always empty. :-(
    // So, we open the keyboard manually here, but I'm not sure if this is the correct behavior.
    if (!isKeyboardOpened_) {
        setKeyboardOpen(true);
    }
}

void TextService::installEventListeners() {
    // ITfThreadMgrEventSink, ITfActiveLanguageProfileNotifySink, and ITfThreadFocusSink
    if (auto source = threadMgr_.query<ITfSource>()) {
        threadMgrEventSink_ = SinkAdvice{ source, IID_ITfThreadMgrEventSink, static_cast<ITfThreadMgrEventSink*>(this) };
        activateLanguageProfileNotifySink_ = SinkAdvice{ source, IID_ITfActiveLanguageProfileNotifySink, static_cast<ITfActiveLanguageProfileNotifySink*>(this) };
        threadFocusSink_ = SinkAdvice{ source, IID_ITfThreadFocusSink, static_cast<ITfThreadFocusSink*>(this) };
    }

    // ITfKeyEventSink
    if (auto keystrokeMgr = threadMgr_.query<ITfKeystrokeMgr>()) {
        keystrokeMgr->AdviseKeyEventSink(clientId_, (ITfKeyEventSink*)this, TRUE);

        // register preserved keys
        for (const auto& preservedKey : preservedKeys_) {
            keystrokeMgr->PreserveKey(clientId_, preservedKey.guid, &preservedKey, NULL, 0);
        }
    }

    // Keyboard open status change.
    if (auto source = threadCompartment(GUID_COMPARTMENT_KEYBOARD_OPENCLOSE).query<ITfSource>()) {
        keyboardOPenCloseSink_ = SinkAdvice(source, IID_ITfCompartmentEventSink, (ITfCompartmentEventSink*)this);
    }

    ComPtr<ITfDocumentMgr> documentMgr;
    if (threadMgr_ && threadMgr_->GetFocus(&documentMgr) == S_OK) {
        initTextEditSink(documentMgr);
    }
}

void TextService::uninstallEventListeners() {
    // ITfThreadMgrEventSink, ITfActiveLanguageProfileNotifySink, and ITfThreadFocusSink
    threadMgrEventSink_.unadvise();
    activateLanguageProfileNotifySink_.unadvise();
    textEditSink_.unadvise();
    textLayoutSink_.unadvise();
    textEditSinkContext_ = nullptr;
    threadFocusSink_.unadvise();

    // ITfKeyEventSink
    if (auto keystrokeMgr = threadMgr_.query<ITfKeystrokeMgr>()) {
        keystrokeMgr->UnadviseKeyEventSink(clientId_);
        // unregister preserved keys
        for (const auto& preservedKey : preservedKeys_) {
            keystrokeMgr->UnpreserveKey(preservedKey.guid, &preservedKey);
        }
    }

    // Keyboard open status change.
    keyboardOPenCloseSink_.unadvise();
}

void TextService::initTextEditSink(ITfDocumentMgr* documentMgr) {
    textEditSink_.unadvise();
    textLayoutSink_.unadvise();
    textEditSinkContext_ = nullptr;

    if (!documentMgr) {
        return;
    }

    ComPtr<ITfContext> context;
    if (documentMgr->GetTop(&context) != S_OK || !context) {
        return;
    }

    if (auto source = context.query<ITfSource>()) {
        textEditSink_ = SinkAdvice{ source, IID_ITfTextEditSink, static_cast<ITfTextEditSink*>(this) };
        textLayoutSink_ = SinkAdvice{ source, IID_ITfTextLayoutSink, static_cast<ITfTextLayoutSink*>(this) };
        textEditSinkContext_ = context;
    }
}

void TextService::activateLanguageButtons() {
    ::CoCreateInstance(CLSID_TF_LangBarMgr, NULL, CLSCTX_INPROC_SERVER,
        IID_ITfLangBarMgr, (void**)&langBarMgr_);
    if (langBarMgr_) {
        langBarMgr_->AdviseEventSink(this, NULL, 0, &langBarSinkCookie_);
    }
    // Note: language bar has no effects in Win 8 immersive mode
    if (!langBarButtons_.empty()) {
        if (auto langBarItemMgr = threadMgr_.query<ITfLangBarItemMgr>()) {
            for (auto& button : langBarButtons_) {
                langBarItemMgr->AddItem(button);
            }
        }
    }
}

void TextService::deactivateLanguageButtons() {
    if (!langBarButtons_.empty()) {
        if (auto langBarItemMgr = threadMgr_.query<ITfLangBarItemMgr>()) {
            for (auto& button : langBarButtons_) {
                langBarItemMgr->RemoveItem(button);
            }
        }
    }
    if (langBarMgr_) {
        langBarMgr_->UnadviseEventSink(langBarSinkCookie_);
        langBarSinkCookie_ = TF_INVALID_COOKIE;
        langBarMgr_ = NULL;
    }
}

// COM stuff

// ITfTextInputProcessor
STDMETHODIMP TextService::Activate(ITfThreadMgr *pThreadMgr, TfClientId tfClientId) {
    return ActivateEx(pThreadMgr, tfClientId, 0);
}

STDMETHODIMP TextService::Deactivate() {
    // terminate composition properly
    if(isComposing()) {
        if(auto context = currentContext()) {
            endComposition(context);
        }
    }

    onDeactivate();

    deactivateLanguageButtons();
    uninstallEventListeners();

    threadMgr_ = nullptr;
    clientId_ = TF_CLIENTID_NULL;
    activateFlags_ = 0;
    return S_OK;
}

// ITfTextInputProcessorEx
STDMETHODIMP TextService::ActivateEx(ITfThreadMgr *ptim, TfClientId tid, DWORD dwFlags) {
    // store tsf manager & client id
    threadMgr_ = ptim;
    clientId_ = tid;

    activateFlags_ = dwFlags;
    if (activateFlags_ == 0) {
        if(auto threadMgrEx = threadMgr_.query<ITfThreadMgrEx>()) {
            threadMgrEx->GetActiveFlags(&activateFlags_);
        }
    }

    installEventListeners();
    initKeyboardState();
    activateLanguageButtons();

    onActivate();
    return S_OK;
}

// ITfDisplayAttributeProvider
STDMETHODIMP TextService::EnumDisplayAttributeInfo(IEnumTfDisplayAttributeInfo** ppEnum) {
    return displayAttributeProvider_->EnumDisplayAttributeInfo(ppEnum);
}

STDMETHODIMP TextService::GetDisplayAttributeInfo(REFGUID guid, ITfDisplayAttributeInfo** ppInfo) {
    return displayAttributeProvider_->GetDisplayAttributeInfo(guid, ppInfo);
}

// ITfThreadMgrEventSink
STDMETHODIMP TextService::OnInitDocumentMgr(ITfDocumentMgr *pDocMgr) {
    return S_OK;
}

STDMETHODIMP TextService::OnUninitDocumentMgr(ITfDocumentMgr *pDocMgr) {
    return S_OK;
}

STDMETHODIMP TextService::OnSetFocus(ITfDocumentMgr *pDocMgrFocus, ITfDocumentMgr *pDocMgrPrevFocus) {
    initTextEditSink(pDocMgrFocus);
    return S_OK;
}

STDMETHODIMP TextService::OnPushContext(ITfContext *pContext) {
    return S_OK;
}

STDMETHODIMP TextService::OnPopContext(ITfContext *pContext) {
    return S_OK;
}


// ITfTextEditSink
STDMETHODIMP TextService::OnEndEdit(ITfContext *pContext, TfEditCookie ecReadOnly, ITfEditRecord *pEditRecord) {
    // This method is called by the TSF whenever an edit operation ends.
    // It's possible for a document to have multiple composition strings at the
    // same time and it's possible for other text services to edit the same
    // document. Though such a complicated senario rarely exist, it indeed happen.

    // NOTE: I don't really know why this is needed and tests yielded no obvious effect
    // of this piece of code, but from MS TSF samples, this is needed.
    BOOL selChanged;
    if(pEditRecord->GetSelectionStatus(&selChanged) == S_OK) {
        if(selChanged && isComposing()) {
            // we need to check if current selection is in our composition string.
            // if after others' editing the selection (insertion point) has been changed and
            // fell outside our composition area, terminate the composition.
            TF_SELECTION selection;
            ULONG selectionNum;
            if(pContext->GetSelection(ecReadOnly, TF_DEFAULT_SELECTION, 1, &selection, &selectionNum) == S_OK) {
                ComPtr<ITfRange> compRange;
                if(composition_->GetRange(&compRange) == S_OK) {
                    // check if two ranges overlaps
                    // check if current selection is covered by composition range
                    LONG compareResult1;
                    LONG compareResult2;
                    if(compRange->CompareStart(ecReadOnly, selection.range, TF_ANCHOR_START, &compareResult1) == S_OK
                        && compRange->CompareEnd(ecReadOnly, selection.range, TF_ANCHOR_END, &compareResult2) == S_OK) {
                        if(compareResult1 == +1 || compareResult2 == -1) {
                            // the selection is not entirely in composion
                            // end compositon here
                            endComposition(pContext);
                        }
                    }
                }
                selection.range->Release();
            }
        }
    }

    return S_OK;
}

STDMETHODIMP TextService::OnLayoutChange(ITfContext *pContext, TfLayoutCode lcode, ITfContextView *pContextView) {
    if (pContext != textEditSinkContext_) {
        return S_OK;
    }
    onLayoutChange(pContext, lcode, pContextView);
    return S_OK;
}


// ITfKeyEventSink
STDMETHODIMP TextService::OnSetFocus(BOOL fForeground) {
    if (fForeground) {
        onSetFocus();
    }
    else {
        onKillFocus();
    }
    return S_OK;
}

STDMETHODIMP TextService::OnTestKeyDown(ITfContext *pContext, WPARAM wParam, LPARAM lParam, BOOL *pfEaten) {
    testKeyUpPending_ = false;
    if (testKeyDownPending_) {
        *pfEaten = TRUE;
        return S_OK;
    }
    if (isKeyboardDisabled(pContext) || !isKeyboardOpened()) {
        *pfEaten = FALSE;
    }
    else {
        KeyEvent keyEvent(WM_KEYDOWN, wParam, lParam);
        *pfEaten = (BOOL)filterKeyDown(keyEvent);
        if (*pfEaten) {
            HRESULT sessionResult;
            // Some hosts only deliver OnTestKeyDown, so process the key here too.
            auto session = ComPtr<EditSession>::make(
                pContext,
                [&](EditSession* session, TfEditCookie cookie) {
                    *pfEaten = onKeyDown(keyEvent, session);
                }
            );
            pContext->RequestEditSession(clientId_, session, TF_ES_SYNC | TF_ES_READWRITE, &sessionResult);
        }
    }
    if (*pfEaten) {
        testKeyDownPending_ = true;
    }
    return S_OK;
}

STDMETHODIMP TextService::OnKeyDown(ITfContext *pContext, WPARAM wParam, LPARAM lParam, BOOL *pfEaten) {
    testKeyUpPending_ = false;
    if (testKeyDownPending_) {
        testKeyDownPending_ = false;
        *pfEaten = TRUE;
        return S_OK;
    }
    if (isKeyboardDisabled(pContext) || !isKeyboardOpened()) {
        *pfEaten = FALSE;
    }
    else {
        KeyEvent keyEvent(WM_KEYDOWN, wParam, lParam);
        *pfEaten = (BOOL)filterKeyDown(keyEvent);
        if(*pfEaten) { // we want to eat the key
            HRESULT sessionResult;
            // ask TSF for an edit session. If editing is approved by TSF,
            // KeyEditSession::DoEditSession will be called, which in turns
            // call back to TextService::doKeyEditSession().
            // So the real key handling is relayed to TextService::doKeyEditSession().
            auto session = ComPtr<EditSession>::make(
                pContext,
                [&](EditSession* session, TfEditCookie cookie) {
                    *pfEaten = onKeyDown(keyEvent, session);
                }
            );
            // We use TF_ES_SYNC here, so the request becomes synchronus and blocking.
            // KeyEditSession::DoEditSession() and TextService::doKeyEditSession() will be
            // called before RequestEditSession() returns.
            pContext->RequestEditSession(clientId_, session, TF_ES_SYNC|TF_ES_READWRITE, &sessionResult);
        }
    }
    return S_OK;
}

STDMETHODIMP TextService::OnTestKeyUp(ITfContext *pContext, WPARAM wParam, LPARAM lParam, BOOL *pfEaten) {
    testKeyDownPending_ = false;
    if (testKeyUpPending_) {
        *pfEaten = TRUE;
        return S_OK;
    }
    if (isKeyboardDisabled(pContext) || !isKeyboardOpened()) {
        *pfEaten = FALSE;
    }
    else {
        KeyEvent keyEvent(WM_KEYUP, wParam, lParam);
        *pfEaten = (BOOL)filterKeyUp(keyEvent);
        if (*pfEaten) {
            HRESULT sessionResult;
            // Mirror OnTestKeyDown for hosts that skip the real OnKeyUp callback.
            auto session = ComPtr<EditSession>::make(
                pContext,
                [&](EditSession* session, TfEditCookie cookie) {
                    *pfEaten = onKeyUp(keyEvent, session);
                }
            );
            pContext->RequestEditSession(clientId_, session, TF_ES_SYNC | TF_ES_READWRITE, &sessionResult);
        }
    }
    if (*pfEaten) {
        testKeyUpPending_ = true;
    }
    return S_OK;
}

STDMETHODIMP TextService::OnKeyUp(ITfContext *pContext, WPARAM wParam, LPARAM lParam, BOOL *pfEaten) {
    testKeyDownPending_ = false;
    if (testKeyUpPending_) {
        testKeyUpPending_ = false;
        *pfEaten = TRUE;
        return S_OK;
    }
    if (isKeyboardDisabled(pContext) || !isKeyboardOpened()) {
        *pfEaten = FALSE;
    }
    else {
        KeyEvent keyEvent(WM_KEYUP, wParam, lParam);
        *pfEaten = (BOOL)filterKeyUp(keyEvent);
        if(*pfEaten) {
            HRESULT sessionResult;
            auto session = ComPtr<EditSession>::make(
                pContext,
                [&](EditSession* session, TfEditCookie cookie) {
                    *pfEaten = onKeyUp(keyEvent, session);
                }
            );
            pContext->RequestEditSession(clientId_, session, TF_ES_SYNC|TF_ES_READWRITE, &sessionResult);
        }
    }
    return S_OK;
}

STDMETHODIMP TextService::OnPreservedKey(ITfContext *pContext, REFGUID rguid, BOOL *pfEaten) {
    *pfEaten = (BOOL)onPreservedKey(rguid);
    return S_OK;
}

// ITfThreadFocusSink
STDMETHODIMP TextService::OnSetThreadFocus() {
    onSetThreadFocus();
    return S_OK;
}

STDMETHODIMP TextService::OnKillThreadFocus() {
    onKillThreadFocus();
    return S_OK;
}


// ITfCompositionSink
STDMETHODIMP TextService::OnCompositionTerminated(TfEditCookie ecWrite, ITfComposition *pComposition) {
    // This is called by TSF when our composition is terminated by others.
    // For example, when the user click on another text editor and the input focus is 
    // grabbed by others, we're ``forced'' to terminate current composition.
    // If we end the composition by calling ITfComposition::EndComposition() ourselves,
    // this event is not triggered.
    onCompositionTerminated(true);
    composition_ = nullptr;
    dummyCompositionActive_ = false;
    hasLastKnownGoodInputRect_ = false;
    hasCompositionAnchor_ = false;
    logTextServiceDebug(L"[OnCompositionTerminated] forced termination dummy_anchor=false");
    return S_OK;
}

// ITfCompartmentEventSink
STDMETHODIMP TextService::OnChange(REFGUID rguid) {
    // The TSF ITfCompartment is kind of key-value storage
    // It uses GUIDs as keys and stores integer and string values.
    // Global compartment is a storage for cross-process key-value pairs.
    // However, it only handles integers. String values cannot be stored.
    // The thread manager specific compartment, however, handles strings.
    // Every value stored in the storage has an key, which is a GUID.
    // global keyboard states and some other values are in the compartments
    // So we need to monitor for their changes and do some handling.

    // For more detailed introduction, see TSF aware blog:
    // http://blogs.msdn.com/b/tsfaware/archive/2007/05/30/what-is-a-keyboard.aspx

    onCompartmentChanged(rguid);
    return S_OK;
}

// ITfLangBarEventSink 
STDMETHODIMP TextService::OnSetFocus(DWORD dwThreadId) {
    return E_NOTIMPL;
}

STDMETHODIMP TextService::OnThreadTerminate(DWORD dwThreadId) {
    return E_NOTIMPL;
}

STDMETHODIMP TextService::OnThreadItemChange(DWORD dwThreadId) {
    return E_NOTIMPL;
}

STDMETHODIMP TextService::OnModalInput(DWORD dwThreadId, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    return E_NOTIMPL;
}

STDMETHODIMP TextService::ShowFloating(DWORD dwFlags) {
    onLangBarStatusChanged(dwFlags);
    return S_OK;
}

STDMETHODIMP TextService::GetItemFloatingRect(DWORD dwThreadId, REFGUID rguid, RECT *prc) {
    return E_NOTIMPL;
}


// ITfActiveLanguageProfileNotifySink
STDMETHODIMP TextService::OnActivated(REFCLSID clsid, REFGUID guidProfile, BOOL fActivated) {
    // we only support one text service, so clsid must be the same as that of our text service.
    // otherwise it's not the notification for our text service, just ignore the event.
    if(clsid == module_->textServiceClsid()) {
        if (fActivated) {
            onLangProfileActivated(guidProfile);
        }
        else {
            onLangProfileDeactivated(guidProfile);
        }
    }
    return S_OK;
}

ComPtr<ITfContext> TextService::currentContext() const {
    ComPtr<ITfContext> context;
    ComPtr<ITfDocumentMgr>  docMgr;
    if(threadMgr_->GetFocus(&docMgr) == S_OK) {
        docMgr->GetTop(&context);
    }
    return context;
}

bool TextService::compositionRect(EditSession* session, RECT* rect) const {
    bool ret = false;
    if(isComposing()) {
        ComPtr<ITfRange> range;
        if(composition_->GetRange(&range) == S_OK) {
            ret = getTextExtRect(session->context(), session->editCookie(), range, rect);
        }
    }
    return ret;
}

bool TextService::inputRect(EditSession* session, RECT* rect) const {
    if (!session || !rect) {
        return false;
    }
    // 组字期间锚点冻结：首键快照、同组字内恒定（微软拼音行为——组字时
    // 候选窗固定在首键处，不随打字机光标右移）。对插入符本就恒定的宿主
    // （记事本/终端）快照值与逐键值一致，行为不变；Sublime 这类"系统插入符
    // 跟随组字末端移动"的宿主由此恢复锚定。
    if (isComposing() && hasCompositionAnchor_) {
        *rect = compositionAnchorRect_;
        logTextServiceDebug(
            L"[inputRect] source=composition-anchor(frozen) rect=" + rectToString(*rect));
        return true;
    }
    const bool ok = inputRectFresh(session, rect);
    if (ok && isComposing() && !hasCompositionAnchor_) {
        compositionAnchorRect_ = *rect;
        hasCompositionAnchor_ = true;
        logTextServiceDebug(
            L"[inputRect] composition anchor recorded rect=" + rectToString(*rect));
    }
    return ok;
}

bool TextService::inputRectFresh(EditSession* session, RECT* rect) const {
    if (!session || !rect) {
        return false;
    }
    // 位置取数顺序（每一轮组字都从新鲜证据开始）：
    //   1. 本 UI 线程的真实系统插入符（hwndCaret 存在 + 归属校验通过）——
    //      换行/挪光标后立即正确，组字期间光标不动、天然无抖动；这是记事本
    //      / 终端类宿主的快路。
    //   2. TSF GetTextExt 矩形——Electron 等自绘 caret 宿主（无系统插入符）
    //      的主路。组字起点优先：composition 矩形取的是 Collapse 到
    //      TF_ANCHOR_START 的起点行，组字期间恒定不动（微软拼音行为：组字
    //      期间候选固定在首键处）；selection 是跟随键入的"打字机光标"，只在
    //      composition 缺失/不可信时用——用它才会逐键右偏抖动。
    //   3. 同组字 last-good：TSF 偶发坏矩形时保持候选窗稳定，不跳左上角。
    //      bad_rect 且 isComposing 时 dummy 锚写入零宽串让 host 有锚可查，
    //      重采后记 last-good。
    RECT caretRect{};
    const wchar_t* caretWhyNot = nullptr;
    bool caretHeightSuspicious = false;
    const bool hasCaretRect = realCaretScreenRect(&caretRect, &caretWhyNot, &caretHeightSuspicious);
    if (hasCaretRect && !caretHeightSuspicious) {
        *rect = caretRect;
        lastKnownGoodInputRect_ = caretRect;
        hasLastKnownGoodInputRect_ = true;
        logTextServiceDebug(
            L"[inputRect] source=real-caret rect=" + rectToString(*rect));
        return true;
    }
    if (hasCaretRect) {
        // 插入符高度=整个输入框（自绘宿主病态行为）：caretRect 已按系统行高
        // 折行，作为兜底；先看 TSF 给出的行矩形，它才是文字行的真位置。
        logTextServiceDebug(
            L"[inputRect] caret height looks like host box; caret fallback rect=" +
            rectToString(caretRect));
    } else if (caretWhyNot != nullptr) {
        logTextServiceDebug(
            L"[inputRect] real caret unavailable: " + std::wstring(caretWhyNot));
    }

    // 病态插入符宿主：TSF 矩形若同样"像整个输入框"（部分自绘宿主 GetTextExt
    // 也报框高），一并判不可用，最终回退到折行后的插入符矩形。
    const auto tsfRectUsable = [&](const RECT& candidate) {
        return !(caretHeightSuspicious && looksLikeHostBox(candidate));
    };
    const auto useCaretFallback = [&](const wchar_t* reason) {
        if (!hasCaretRect) {
            return false;
        }
        *rect = caretRect;
        lastKnownGoodInputRect_ = caretRect;
        hasLastKnownGoodInputRect_ = true;
        logTextServiceDebug(
            L"[inputRect] source=caret-line-fallback(" + std::wstring(reason) +
            L") rect=" + rectToString(*rect));
        return true;
    };

    RECT foregroundRect = {};
    const RECT* foregroundRectPtr = getForegroundWindowRect(&foregroundRect) ? &foregroundRect : nullptr;
    struct InputRectSnapshot {
        RECT compositionStartRect{};
        bool hasCompositionStartRect = false;
        RECT selectionRectValue{};
        bool hasSelectionRectValue = false;
        int compositionScore = -1000;
        int selectionScore = -1000;
    };

    auto collectSnapshot = [&](InputRectSnapshot* snapshot) {
        snapshot->hasCompositionStartRect = false;
        snapshot->hasSelectionRectValue = false;
        snapshot->compositionScore = -1000;
        snapshot->selectionScore = -1000;

        if (isComposing()) {
            ComPtr<ITfRange> compositionRange;
            if (composition_->GetRange(&compositionRange) == S_OK) {
                compositionRange->Collapse(session->editCookie(), TF_ANCHOR_START);
                snapshot->hasCompositionStartRect = getRawTextExtRect(
                    session->context(), session->editCookie(), compositionRange, &snapshot->compositionStartRect);
            }
        }

        TF_SELECTION selection;
        ULONG selectionNum;
        if(session->context()->GetSelection(session->editCookie(), TF_DEFAULT_SELECTION, 1, &selection, &selectionNum) == S_OK ) {
            snapshot->hasSelectionRectValue = getRawTextExtRect(
                session->context(), session->editCookie(), selection.range, &snapshot->selectionRectValue);
            selection.range->Release();
        } else {
            logTextServiceDebug(
                L"[inputRect] GetSelection failed dummy_anchor=" + boolText(dummyCompositionActive_));
        }

        if (snapshot->hasCompositionStartRect) {
            snapshot->compositionScore = inputRectScore(
                snapshot->compositionStartRect, foregroundRectPtr, false);
        }
        if (snapshot->hasSelectionRectValue) {
            snapshot->selectionScore = inputRectScore(
                snapshot->selectionRectValue, foregroundRectPtr, true);
        }
    };

    InputRectSnapshot snapshot;
    collectSnapshot(&snapshot);

    const auto bestScore = [&]() {
        return (std::max)(snapshot.compositionScore, snapshot.selectionScore);
    };

    // 纯 IMM 宿主救援：两个 TSF 矩形都不可信（缺失/前景窗外废矩形）时，
    // 旧行为会被 tryAdjustRectWithForegroundCaret 钉到前景窗左上角（wezterm
    // 实测即用户报的"候选窗一直在左上角"）。先试 IMM32 旧通道里宿主报过的
    // 光标位置。TSF 矩形可用的宿主（记事本/Electron/WT）bestScore >= 0，
    // 到不了这里，零回归；IMM 数据也要过前景窗打分才采用。
    if (bestScore() < 0) {
        RECT immRect{};
        std::wstring immSource;
        if (immAnchorScreenRect(&immRect, &immSource) &&
            inputRectScore(immRect, foregroundRectPtr, false) >= 0) {
            *rect = immRect;
            lastKnownGoodInputRect_ = immRect;
            hasLastKnownGoodInputRect_ = true;
            logTextServiceDebug(
                L"[inputRect] source=imm32-" + immSource + L" rect=" + rectToString(immRect));
            return true;
        }
    }

    if ((!snapshot.hasCompositionStartRect && !snapshot.hasSelectionRectValue) ||
        bestScore() < 0) {
        const wchar_t* reason =
            (!snapshot.hasCompositionStartRect && !snapshot.hasSelectionRectValue)
            ? L"missing_rect"
            : L"bad_rect";
        if (ensureDummyCompositionAnchor(session, reason)) {
            collectSnapshot(&snapshot);
        }
    }

    if (!snapshot.hasCompositionStartRect && !snapshot.hasSelectionRectValue) {
        if (hasLastKnownGoodInputRect_) {
            // TSF 本轮无可用矩形：同组字内复用 last-good，保持候选窗稳定，
            // 不跳到 (0,0) / 左上角。换行/挪光标后的首个有效矩形会自动接管。
            *rect = lastKnownGoodInputRect_;
            logTextServiceDebug(
                L"[inputRect] source=last-good rect=" + rectToString(*rect));
            return true;
        }
        if (useCaretFallback(L"no-tsf-rect")) {
            return true;
        }
        logTextServiceDebug(
            L"[inputRect] failed dummy_anchor=" + boolText(dummyCompositionActive_) +
            L" auto_dummy=" + boolText(autoDummyCompositionAnchorEnabled_) +
            L" composition_start=false selection=false");
        return false;
    }

    // 组字起点优先：composition 矩形取自 Collapse(TF_ANCHOR_START) 的起点
    // 行，组字期间恒定——候选固定在首键处（微软拼音行为）。selection 是
    // 跟随键入的"打字机光标"，只在 composition 缺失/不可信时用。
    // tryAdjustRectWithForegroundCaret 只修正"明显越界"的矩形（游戏/全屏
    // host 报相对坐标），正常宿主返回 false，不改变择优结果。
    //
    // 可信门槛（score >= 0）复用 inputRectScore：前景窗外 -20、左上
    // 4px 宽"伪锚点" -80——这两类是 TSF 宿主的典型废矩形，直接判不可信，
    // 走 selection 回退或同组字 last-good，不送出去。
    const bool compositionUsable = snapshot.hasCompositionStartRect &&
        snapshot.compositionScore >= 0 &&
        tsfRectUsable(snapshot.compositionStartRect);
    const bool chooseSelection = !compositionUsable && snapshot.hasSelectionRectValue &&
        tsfRectUsable(snapshot.selectionRectValue);
    if (!compositionUsable && !chooseSelection) {
        // 两个 TSF 矩形都不可用（缺失、打分不过、或"像整框"）：病态插入符
        // 宿主优先用折行后的插入符矩形，别把整框矩形送出去。
        if (useCaretFallback(L"tsf-rect-unusable")) {
            return true;
        }
    }
    *rect = chooseSelection ? snapshot.selectionRectValue : snapshot.compositionStartRect;
    const bool adjusted = tryAdjustRectWithForegroundCaret(rect);

    // bad_rect（两个候选都没过前景窗打分）且非 adjusted：矩形不可信——
    // 同组字内回 last-good，不把脏矩形记下来也不送出去。注意最后真正送
    // 出去的若是 selection（composition 不可用时的回退），它每键跟随光标，
    // 仍可能逐键右移——此时记 last-good 会把"移动错觉"带进后续回退；
    // 只有恒定的 composition-start 才值得记忆。
    if (bestScore() < 0 && !adjusted && hasLastKnownGoodInputRect_) {
        *rect = lastKnownGoodInputRect_;
        logTextServiceDebug(
            L"[inputRect] source=last-good(fallback-from-bad-rect) rect=" +
            rectToString(*rect));
        return true;
    }
    if (bestScore() < 0 && !adjusted && useCaretFallback(L"bad-tsf-rect")) {
        return true;
    }
    if (!chooseSelection) {
        lastKnownGoodInputRect_ = *rect;
        hasLastKnownGoodInputRect_ = true;
    }

    logTextServiceDebug(
        L"[inputRect] source=" + std::wstring(chooseSelection ? L"selection" : L"composition-start") +
        L" dummy_anchor=" + boolText(dummyCompositionActive_) +
        L" auto_dummy=" + boolText(autoDummyCompositionAnchorEnabled_) +
        L" composition_start=" +
        (snapshot.hasCompositionStartRect ? rectToString(snapshot.compositionStartRect) : std::wstring(L"<none>")) +
        L" selection=" +
        (snapshot.hasSelectionRectValue ? rectToString(snapshot.selectionRectValue) : std::wstring(L"<none>")) +
        L" composition_score=" + std::to_wstring(snapshot.compositionScore) +
        L" selection_score=" + std::to_wstring(snapshot.selectionScore) +
        L" adjusted=" + boolText(adjusted) +
        L" final=" + rectToString(*rect));
    return true;
}

bool TextService::selectionRect(EditSession* session, RECT* rect) const {
    bool ret = false;
    if(isComposing()) {
        TF_SELECTION selection;
        ULONG selectionNum;
        if(session->context()->GetSelection(session->editCookie(), TF_DEFAULT_SELECTION, 1, &selection, &selectionNum) == S_OK ) {
            ret = getTextExtRect(session->context(), session->editCookie(), selection.range, rect);
            selection.range->Release();
        }
    }
    return ret;
}

HWND TextService::compositionWindow(EditSession* session) const {
    HWND hwnd = NULL;
    ComPtr<ITfContextView> view;
    if(session->context()->GetActiveView(&view) == S_OK) {
        // get current composition window
        view->GetWnd(&hwnd);
    }
    if (hwnd == NULL)
        hwnd = ::GetFocus();
    return hwnd;
}

} // namespace Ime

