#include "DebugLogConfig.h"

#include <Windows.h>

#include <algorithm>
#include <cstdio>
#include <string>

namespace Ime {

namespace {

enum class DebugLogLevel {
    Off,
    Debug,
    Trace,
};

std::wstring configFilePath() {
    const wchar_t* localAppData = _wgetenv(L"LOCALAPPDATA");
    if (!localAppData || !*localAppData) {
        return L"";
    }
    return std::wstring(localAppData) + L"\\MoqiIM\\MoqiLaunch.json";
}

bool readFileText(const std::wstring& path, std::string* out) {
    if (!out || path.empty()) {
        return false;
    }

    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"rb") != 0 || !file) {
        return false;
    }

    std::string content;
    char buffer[512];
    size_t read = 0;
    while ((read = fread(buffer, 1, sizeof(buffer), file)) != 0) {
        content.append(buffer, read);
    }
    fclose(file);
    *out = std::move(content);
    return true;
}

DebugLogLevel parseDebugLogLevel(const std::string& raw) {
    std::string text = raw;
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });

    // 配置文件里可能同时存在 "logLevel"（启动器侧）与 "loglevel"（前端侧）
    // 两个键（大小写不同、语义相同，小写化后同名），且书写顺序不定。旧实现
    // 只取第一个匹配：`{"logLevel":"debug","loglevel":"trace"}` 会先命中
    // "logLevel"→"debug"，前端 trace 埋点（tsf-debug.log / candidate-window.log）
    // 永久失效。改为扫描全部匹配取最高档。
    DebugLogLevel best = DebugLogLevel::Off;
    size_t searchFrom = 0;
    while (true) {
        const size_t keyPos = text.find("\"loglevel\"", searchFrom);
        if (keyPos == std::string::npos) {
            break;
        }
        searchFrom = keyPos + 1;

        const size_t colonPos = text.find(':', keyPos);
        if (colonPos == std::string::npos) {
            break;
        }
        const size_t firstQuote = text.find('"', colonPos);
        if (firstQuote == std::string::npos) {
            break;
        }
        const size_t secondQuote = text.find('"', firstQuote + 1);
        if (secondQuote == std::string::npos) {
            break;
        }

        const std::string value = text.substr(firstQuote + 1, secondQuote - firstQuote - 1);
        if (value == "trace" && best < DebugLogLevel::Trace) {
            best = DebugLogLevel::Trace;
        } else if (value == "debug" && best < DebugLogLevel::Debug) {
            best = DebugLogLevel::Debug;
        }
        searchFrom = secondQuote + 1;
    }
    return best;
}

} // namespace

bool isDebugLoggingEnabled() {
    static ULONGLONG lastRefreshTick = 0;
    static DebugLogLevel cachedLevel = DebugLogLevel::Off;

    const ULONGLONG now = ::GetTickCount64();
    if (lastRefreshTick != 0 && now - lastRefreshTick < 1000) {
        return cachedLevel >= DebugLogLevel::Debug;
    }

    cachedLevel = DebugLogLevel::Off;
    std::string configText;
    if (readFileText(configFilePath(), &configText)) {
        cachedLevel = parseDebugLogLevel(configText);
    }

    lastRefreshTick = now;
    return cachedLevel >= DebugLogLevel::Debug;
}

bool isTraceLoggingEnabled() {
    static ULONGLONG lastRefreshTick = 0;
    static DebugLogLevel cachedLevel = DebugLogLevel::Off;

    const ULONGLONG now = ::GetTickCount64();
    if (lastRefreshTick != 0 && now - lastRefreshTick < 1000) {
        return cachedLevel >= DebugLogLevel::Trace;
    }

    cachedLevel = DebugLogLevel::Off;
    std::string configText;
    if (readFileText(configFilePath(), &configText)) {
        cachedLevel = parseDebugLogLevel(configText);
    }

    lastRefreshTick = now;
    return cachedLevel >= DebugLogLevel::Trace;
}

} // namespace Ime
