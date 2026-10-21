#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <conio.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <ksmedia.h>
#include <winhttp.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "winhttp.lib")

static std::atomic_bool g_stop{false};
static std::atomic_uint g_volumeUpRequests{0};
static std::atomic_uint g_volumeDownRequests{0};
static std::atomic_bool g_reconnectRequested{false};
static HHOOK g_keyboardHook = nullptr;
static HANDLE g_keyboardHookReadyEvent = nullptr;
static HANDLE g_keyboardHookStopEvent = nullptr;
static HANDLE g_keyboardHookThread = nullptr;
static DWORD g_keyboardHookError = ERROR_SUCCESS;
static bool g_volumeUpPressed = false;
static bool g_volumeDownPressed = false;
static bool g_volumeMutePressed = false;

// 单次写入超过 1 ms，计入写入耗时回退估算。
static constexpr double kBlockedWriteMs = 1.0;

// 每个发送线程独立的写入统计。多路发送后不能再共用全局变量。
struct WriteStats {
    double blockedMs = 0.0;
    uint64_t writeCalls = 0;
    uint64_t writeOver1MsCalls = 0;
    double writeMaxMs = 0.0;
    DWORD lastWinHttpError = ERROR_SUCCESS;
};

// 允许的最大音频年龄（毫秒）。用 WASAPI 时间戳算出的“这段音频现在有多旧”，
// 超过就从这段音频开头丢弃，只发最新的部分，保证下位机收到的一定是新的。
// 实测正常播放时 age 在 -8~5ms 之间波动，所以 10ms 足够宽松，
// 一旦明显超出就说明链路或进程卡了，必须丢。
static double kDropBacklogMs = 20.0;

// 发送水位（毫秒）：队列里攒够这么多音频才写出去。
// 调大就等于额外增加延迟，用来吸收网络抖动，让播放更稳；
// 代价是端到端延迟变大，多设备同步时也能用更大的缓冲。
static double kSendWaterMs = 5.0;

// 队列硬上限（毫秒音频），只用来兜住服务端彻底卡死的病态情况。
static double kDropAllBacklogMs = 500.0;

// 发送节拍：采集到的音频先入发送队列，再由发送循环每 kSendChunkMs 写一块。
// 以前是“来一个 WASAPI 包就写一次”，socket 发送缓冲一满写调用就阻塞，
// 阻塞期间采集侧继续积压，socket 一空又把积压一次性灌进去，
// 于是内核发送缓冲里常年堆着 100~300ms 的音频，那才是延迟主体。
static double kSendChunkMs = 10.0;

// HTTP/网络重连最多自动尝试 3 次；只有真正写出音频数据后才清零。
static constexpr int kMaxAutoReconnectAttempts = 3;

// 使用命令行 URL 启动时，3 次自动重连全部失败后继续后台运行，
// 每 10 秒重新尝试一次，适合开机启动；交互模式仍等待 Enter。
static constexpr auto kServiceRetryDelay = std::chrono::seconds(10);

// 拥塞/手动重连：先停采集并清空捕获缓冲，等待服务端释放旧的 aplay 后再恢复采集。
static constexpr auto kCongestionReconnectDelay = std::chrono::milliseconds(200);


// 队列里的一段 PCM（已转换成 s16le），带它在系统时钟上的位置。
//
// 时间戳是关键：它来自 WASAPI 的 qpcPosition，表示这段音频在系统时钟上
// 的起点。用它可以直接算出这段音频“现在有多旧”，从而只发新鲜的音频，
// 而不必靠字节数水位去猜——那个猜法看不到已经发出去、堵在链路里的数据。
struct PcmBlock {
    std::vector<BYTE> data;
    UINT64 qpcStart = 0;   // 这段音频起点（QPC 100ns）
    UINT32 frames = 0;     // 这段音频包含多少帧
    bool hasTimestamp = false;
};

// 采集线程与发送线程之间的 PCM 队列。
// 采集线程只负责入队，绝不等网络；发送循环只管尽快写出。
struct PcmQueue {
    std::mutex mutex;
    std::deque<PcmBlock> blocks;
    size_t queuedBytes = 0;
};

static void PrintHr(const wchar_t* where, HRESULT hr);

// Configure short-period, event-driven shared-mode loopback capture.
static bool InitializeLowLatencyLoopback(IMMDevice* device,
                                         IAudioClient** audioClient,
                                         const WAVEFORMATEX* format,
                                         HANDLE audioEvent)
{
    constexpr REFERENCE_TIME fallbackDuration = 50000; // 5 ms
    REFERENCE_TIME bufferDuration = fallbackDuration;
    UINT32 minPeriodFrames = 0;
    UINT32 defaultPeriodFrames = 0;

    IAudioClient3* audioClient3 = nullptr;
    HRESULT hr = (*audioClient)->QueryInterface(
        __uuidof(IAudioClient3),
        reinterpret_cast<void**>(&audioClient3));
    if (SUCCEEDED(hr) && audioClient3) {
        UINT32 fundamentalPeriodFrames = 0;
        UINT32 maxPeriodFrames = 0;
        hr = audioClient3->GetSharedModeEnginePeriod(
            format,
            &defaultPeriodFrames,
            &fundamentalPeriodFrames,
            &minPeriodFrames,
            &maxPeriodFrames);
        if (SUCCEEDED(hr) && minPeriodFrames > 0 &&
            format->nSamplesPerSec > 0) {
            bufferDuration = static_cast<REFERENCE_TIME>(
                (static_cast<uint64_t>(minPeriodFrames) * 10000000ULL +
                 format->nSamplesPerSec - 1) /
                format->nSamplesPerSec);
        } else if (FAILED(hr)) {
            PrintHr(L"IAudioClient3::GetSharedModeEnginePeriod", hr);
        }
        audioClient3->Release();
    } else if (hr != E_NOINTERFACE) {
        PrintHr(L"QueryInterface(IAudioClient3)", hr);
    }

    const auto reactivateAudioClient = [&]() -> bool {
        (*audioClient)->Release();
        *audioClient = nullptr;
        const HRESULT activateHr = device->Activate(
            __uuidof(IAudioClient), CLSCTX_ALL, nullptr,
            reinterpret_cast<void**>(audioClient));
        if (FAILED(activateHr)) {
            PrintHr(L"IMMDevice::Activate(IAudioClient,polling fallback)", activateHr);
            return false;
        }
        return true;
    };

    hr = (*audioClient)->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_LOOPBACK |
            AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
        bufferDuration,
        0,
        format,
        nullptr);
    if (FAILED(hr)) {
        PrintHr(L"IAudioClient::Initialize(loopback,event-driven)", hr);
        if (!reactivateAudioClient())
            return false;
        hr = (*audioClient)->Initialize(
            AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_LOOPBACK,
            bufferDuration,
            0,
            format,
            nullptr);
        if (FAILED(hr)) {
            PrintHr(L"IAudioClient::Initialize(loopback,polling fallback)", hr);
            return false;
        }
        std::wcout << L"Event-driven capture unavailable; using polling fallback.\n";
        return true;
    }
    hr = (*audioClient)->SetEventHandle(audioEvent);
    if (FAILED(hr)) {
        PrintHr(L"IAudioClient::SetEventHandle(loopback fallback)", hr);
        if (!reactivateAudioClient())
            return false;
        hr = (*audioClient)->Initialize(
            AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_LOOPBACK,
            bufferDuration,
            0,
            format,
            nullptr);
        if (FAILED(hr)) {
            PrintHr(L"IAudioClient::Initialize(loopback,polling fallback)", hr);
            return false;
        }
        std::wcout << L"Event handle unavailable; using polling fallback.\n";
        return true;
    }
    std::wcout << L"Event-driven loopback capture; ";
    if (minPeriodFrames > 0) {
        std::wcout << L"IAudioClient3 minimum shared period: "
                   << (static_cast<double>(minPeriodFrames) * 1000.0 /
                       format->nSamplesPerSec)
                   << L" ms, requested buffer: ";
    } else {
        std::wcout << L"IAudioClient3 period query unavailable, requesting ";
    }
    std::wcout << (static_cast<double>(bufferDuration) / 10000.0)
               << L" ms\n";
    return true;
}

BOOL WINAPI ConsoleHandler(DWORD type)
{
    switch (type) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        g_stop = true;
        return TRUE;
    default:
        return FALSE;
    }
}

static void PrintWinError(const wchar_t* where, DWORD err = GetLastError())
{
    std::wcerr << where << L" failed, error=" << err << L" (0x"
               << std::hex << err << std::dec << L")\n";
}

static void PrintHr(const wchar_t* where, HRESULT hr)
{
    std::wcerr << where << L" failed, HRESULT=0x"
               << std::hex << static_cast<unsigned long>(hr) << std::dec << L"\n";
}

// 实现媒体按键音量键的拦截,实际控制音箱的音量
static LRESULT CALLBACK LowLevelKeyboardProc(int code,
                                              WPARAM message,
                                              LPARAM data)
{
    if (code == HC_ACTION) {
        const auto* key =
            reinterpret_cast<const KBDLLHOOKSTRUCT*>(data);

        if (key->vkCode == VK_VOLUME_UP) {
            if (message == WM_KEYDOWN || message == WM_SYSKEYDOWN) {
                if (!g_volumeUpPressed) {
                    g_volumeUpRequests.fetch_add(1);
                    g_volumeUpPressed = true;
                }
                return 1;
            }

            if (message == WM_KEYUP || message == WM_SYSKEYUP) {
                g_volumeUpPressed = false;
                return 1;
            }
        } else if (key->vkCode == VK_VOLUME_DOWN) {
            if (message == WM_KEYDOWN || message == WM_SYSKEYDOWN) {
                if (!g_volumeDownPressed) {
                    g_volumeDownRequests.fetch_add(1);
                    g_volumeDownPressed = true;
                }
                return 1;
            }

            if (message == WM_KEYUP || message == WM_SYSKEYUP) {
                g_volumeDownPressed = false;
                return 1;
            }
        } else if (key->vkCode == VK_VOLUME_MUTE) {
            if (message == WM_KEYDOWN || message == WM_SYSKEYDOWN) {
                if (!g_volumeMutePressed) {
                    g_reconnectRequested.store(true);
                    g_volumeMutePressed = true;
                }
                return 1;
            }

            if (message == WM_KEYUP || message == WM_SYSKEYUP) {
                g_volumeMutePressed = false;
                return 1;
            }
        }
    }

    return CallNextHookEx(g_keyboardHook, code, message, data);
}

static DWORD WINAPI KeyboardHookThreadProc(void*)
{
    MSG message{};
    PeekMessageW(&message, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
    g_keyboardHook = SetWindowsHookExW(
        WH_KEYBOARD_LL,
        LowLevelKeyboardProc,
        GetModuleHandleW(nullptr),
        0);

    if (!g_keyboardHook)
        g_keyboardHookError = GetLastError();

    SetEvent(g_keyboardHookReadyEvent);

    if (!g_keyboardHook)
        return 1;

    for (;;) {
        const DWORD waitResult = MsgWaitForMultipleObjects(
            1,
            &g_keyboardHookStopEvent,
            FALSE,
            INFINITE,
            QS_ALLINPUT);

        if (waitResult == WAIT_OBJECT_0)
            break;

        if (waitResult == WAIT_OBJECT_0 + 1) {
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                if (message.message == WM_QUIT)
                    goto finished;

                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            continue;
        }

        PrintWinError(L"MsgWaitForMultipleObjects(keyboard hook)");
        break;
    }

finished:
    if (!UnhookWindowsHookEx(g_keyboardHook))
        PrintWinError(L"UnhookWindowsHookEx");
    g_keyboardHook = nullptr;
    return 0;
}

static bool StartKeyboardHook()
{
    g_keyboardHookReadyEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_keyboardHookReadyEvent) {
        PrintWinError(L"CreateEvent(keyboard hook ready)");
        return false;
    }

    g_keyboardHookStopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_keyboardHookStopEvent) {
        PrintWinError(L"CreateEvent(keyboard hook stop)");
        CloseHandle(g_keyboardHookReadyEvent);
        g_keyboardHookReadyEvent = nullptr;
        return false;
    }

    g_keyboardHookThread = CreateThread(
        nullptr,
        0,
        KeyboardHookThreadProc,
        nullptr,
        0,
        nullptr);

    if (!g_keyboardHookThread) {
        PrintWinError(L"CreateThread(keyboard hook)");
        CloseHandle(g_keyboardHookStopEvent);
        CloseHandle(g_keyboardHookReadyEvent);
        g_keyboardHookStopEvent = nullptr;
        g_keyboardHookReadyEvent = nullptr;
        return false;
    }

    const DWORD waitResult =
        WaitForSingleObject(g_keyboardHookReadyEvent, INFINITE);
    if (waitResult != WAIT_OBJECT_0) {
        if (waitResult == WAIT_FAILED)
            PrintWinError(L"WaitForSingleObject(keyboard hook ready)");
        else
            std::wcerr << L"Unexpected keyboard hook startup wait result.\n";
    } else if (g_keyboardHook) {
        std::wcout
            << L"Media volume keys are captured; system volume will not change.\n";
        return true;
    } else {
        PrintWinError(L"SetWindowsHookEx(WH_KEYBOARD_LL)", g_keyboardHookError);
    }

    SetEvent(g_keyboardHookStopEvent);
    if (WaitForSingleObject(g_keyboardHookThread, INFINITE) != WAIT_OBJECT_0)
        PrintWinError(L"WaitForSingleObject(keyboard hook thread)");
    CloseHandle(g_keyboardHookThread);
    CloseHandle(g_keyboardHookStopEvent);
    CloseHandle(g_keyboardHookReadyEvent);
    g_keyboardHookThread = nullptr;
    g_keyboardHookStopEvent = nullptr;
    g_keyboardHookReadyEvent = nullptr;
    return false;
}

static void StopKeyboardHook()
{
    if (!g_keyboardHookThread)
        return;

    if (!SetEvent(g_keyboardHookStopEvent))
        PrintWinError(L"SetEvent(keyboard hook stop)");

    if (WaitForSingleObject(g_keyboardHookThread, INFINITE) != WAIT_OBJECT_0)
        PrintWinError(L"WaitForSingleObject(keyboard hook thread)");
    CloseHandle(g_keyboardHookThread);
    CloseHandle(g_keyboardHookStopEvent);
    CloseHandle(g_keyboardHookReadyEvent);
    g_keyboardHookThread = nullptr;
    g_keyboardHookStopEvent = nullptr;
    g_keyboardHookReadyEvent = nullptr;
}

struct UrlParts {
    std::wstring host;
    std::wstring path;
    INTERNET_PORT port = 0;
    bool https = false;
    bool portSpecified = false;
};

// 一路目标：普通立体声，或只发送左/右声道。
enum class ChannelMode {
    Stereo,
    Left,
    Right
};

struct DestinationSpec {
    std::wstring url;
    ChannelMode channelMode = ChannelMode::Stereo;
};

// 每路目标独立的发送队列、连接状态和音量请求地址。
// 采集只做一次，数据按路复制；每路由自己的线程负责 HTTP 发送和重连。
struct Destination {
    std::wstring url;
    ChannelMode channelMode = ChannelMode::Stereo;
    size_t index = 0;

    PcmQueue queue;

    std::mutex partsMutex;
    UrlParts activeParts;

    std::atomic_bool stop{false};
    std::atomic_bool reconnectRequested{false};

    // 仅采集线程访问的日志状态，每路独立，避免互相影响。
    bool captureDropActive = false;
    bool queueTrimActive = false;

    // 由 ConfigureDestination 按采集格式和本路输出声道数预先算好。
    UINT32 outChannels = 0;
    size_t sendBytesPerFrame = 0;
    size_t sendChunkBytes = 0;
    double sendBytesPerMs = 0.0;
    size_t maxQueuedBytes = 0;
    size_t minSendBytes = 0;
};

static void ConfigureDestination(Destination& destination,
                                 DWORD sampleRate,
                                 WORD sourceChannels)
{
    const UINT32 outChannels =
        destination.channelMode == ChannelMode::Stereo
            ? sourceChannels
            : 1;

    destination.outChannels = outChannels;
    destination.sendBytesPerFrame =
        static_cast<size_t>(outChannels) * sizeof(int16_t);
    destination.sendChunkBytes = std::max<size_t>(
        1,
        static_cast<size_t>(
            static_cast<double>(sampleRate) * kSendChunkMs / 1000.0) *
            destination.sendBytesPerFrame);
    destination.sendBytesPerMs =
        static_cast<double>(sampleRate) *
        static_cast<double>(destination.sendBytesPerFrame) / 1000.0;
    destination.maxQueuedBytes = std::max<size_t>(
        destination.sendChunkBytes * 8,
        static_cast<size_t>(
            destination.sendBytesPerMs * kDropAllBacklogMs));
    destination.minSendBytes = std::max<size_t>(
        1,
        static_cast<size_t>(
            destination.sendBytesPerMs * kSendWaterMs));
}

static std::wstring TrimSpec(const std::wstring& text)
{
    const size_t begin = text.find_first_not_of(L" \t\r\n");
    if (begin == std::wstring::npos)
        return L"";

    const size_t end = text.find_last_not_of(L" \t\r\n");
    return text.substr(begin, end - begin + 1);
}

// 解析单个目标：可选的 L/R 前缀 + URL。
// 前缀只认大写 L/R，避免把 localhost 之类的地址误当成 R 声道。
static bool ParseDestinationSpec(const std::wstring& text,
                                 DestinationSpec& spec)
{
    std::wstring token = TrimSpec(text);
    if (token.empty())
        return false;

    spec.channelMode = ChannelMode::Stereo;

    // 只有前缀没有地址，视为无效项。
    if (token == L"L" || token == L"R")
        return false;

    if (token.size() >= 2 && token[0] == L'L') {
        spec.channelMode = ChannelMode::Left;
        token.erase(0, 1);
    } else if (token.size() >= 2 && token[0] == L'R') {
        spec.channelMode = ChannelMode::Right;
        token.erase(0, 1);
    }

    token = TrimSpec(token);
    if (token.empty())
        return false;

    spec.url = token;
    return true;
}

// 第一个参数用半角逗号分割多个目标，空项自动忽略。
static std::vector<DestinationSpec> ParseDestinationSpecs(
    const std::wstring& input)
{
    std::vector<DestinationSpec> specs;
    size_t start = 0;

    while (start <= input.size()) {
        size_t comma = input.find(L',', start);
        if (comma == std::wstring::npos)
            comma = input.size();

        DestinationSpec spec;
        if (ParseDestinationSpec(
                input.substr(start, comma - start), spec)) {
            specs.push_back(std::move(spec));
        }

        if (comma == input.size())
            break;

        start = comma + 1;
    }

    return specs;
}

static bool HasExplicitPort(const std::wstring& url) {
    const size_t schemeEnd = url.find(L"://");
    const size_t authorityBegin =
        schemeEnd == std::wstring::npos ? 0 : schemeEnd + 3;
    const size_t authorityEnd =
        url.find_first_of(L"/?#", authorityBegin);
    std::wstring authority = url.substr(
        authorityBegin,
        authorityEnd == std::wstring::npos
            ? std::wstring::npos
            : authorityEnd - authorityBegin);

    const size_t userInfoEnd = authority.rfind(L'@');
    if (userInfoEnd != std::wstring::npos) {
        authority.erase(0, userInfoEnd + 1);
    }

    size_t portSeparator = std::wstring::npos;
    if (!authority.empty() && authority.front() == L'[') {
        const size_t hostEnd = authority.find(L']');
        if (hostEnd != std::wstring::npos &&
            hostEnd + 1 < authority.size() &&
            authority[hostEnd + 1] == L':') {
            portSeparator = hostEnd + 1;
        }
    } else {
        portSeparator = authority.rfind(L':');
    }

    if (portSeparator == std::wstring::npos ||
        portSeparator + 1 == authority.size()) {
        return false;
    }

    return std::all_of(
        authority.begin() + portSeparator + 1,
        authority.end(),
        [](wchar_t c) { return c >= L'0' && c <= L'9'; });
}

static bool IsConnectionFailure(DWORD error) {
    return error == ERROR_WINHTTP_CANNOT_CONNECT ||
           error == ERROR_WINHTTP_CONNECTION_ERROR ||
           error == ERROR_WINHTTP_TIMEOUT;
}

static bool AdvancePortFallback(UrlParts& parts) {
    if (parts.https) {
        return false;
    }

    if (!parts.portSpecified && parts.port == INTERNET_DEFAULT_HTTP_PORT) {
        parts.port = 8080;
    } else if (parts.port == 8080) {
        parts.port = 8880;
    } else {
        return false;
    }

    return true;
}

static bool CrackUrl(const std::wstring& url, UrlParts& out)
{
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);

    wchar_t host[512] = {};
    wchar_t path[4096] = {};
    wchar_t extra[4096] = {};

    uc.lpszHostName = host;
    uc.dwHostNameLength = _countof(host);

    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = _countof(path);

    uc.lpszExtraInfo = extra;
    uc.dwExtraInfoLength = _countof(extra);

    std::wstring fullUrl = url;

    // 没有 scheme 时按 HTTP 处理，路径则在解析后单独补齐。
    if (fullUrl.rfind(L"http", 0) != 0) {
        fullUrl = L"http://" + fullUrl;
    }

    if (!WinHttpCrackUrl(fullUrl.c_str(), 0, 0, &uc)) {
        PrintWinError(L"WinHttpCrackUrl");
        return false;
    }

    out.https = (uc.nScheme == INTERNET_SCHEME_HTTPS);
    out.port = uc.nPort;
    out.portSpecified = HasExplicitPort(fullUrl);

    if (uc.dwHostNameLength == 0) {
        std::wcerr << L"Invalid URL: missing host.\n";
        return false;
    }

    out.host.assign(host, uc.dwHostNameLength);

    if (uc.dwUrlPathLength > 0)
        out.path.assign(path, uc.dwUrlPathLength);
    else
        out.path = L"/aplay";

    if (out.path == L"/")
        out.path = L"/aplay";

    if (uc.dwExtraInfoLength > 0)
        out.path.append(extra, uc.dwExtraInfoLength);

    return true;
}

static void SetQueryParameter(std::wstring& path,
                              const std::wstring& name,
                              const std::wstring& value)
{
    const size_t q = path.find(L'?');

    if (q == std::wstring::npos) {
        path += L"?" + name + L"=" + value;
        return;
    }

    const size_t queryBegin = q + 1;
    const size_t fragment = path.find(L'#', queryBegin);
    const size_t queryEnd =
        (fragment == std::wstring::npos) ? path.size() : fragment;

    std::wstring query = path.substr(queryBegin, queryEnd - queryBegin);

    std::wstring out;
    bool found = false;
    size_t pos = 0;

    while (pos <= query.size()) {
        size_t amp = query.find(L'&', pos);

        if (amp == std::wstring::npos)
            amp = query.size();

        const std::wstring item = query.substr(pos, amp - pos);

        const size_t eq = item.find(L'=');
        const std::wstring key =
            (eq == std::wstring::npos) ? item : item.substr(0, eq);

        if (key == name) {
            if (!found) {
                if (!out.empty())
                    out += L'&';

                out += name + L"=" + value;
                found = true;
            }
        } else if (!item.empty()) {
            if (!out.empty())
                out += L'&';

            out += item;
        }

        if (amp == query.size())
            break;

        pos = amp + 1;
    }

    if (!found) {
        if (!out.empty())
            out += L'&';

        out += name + L"=" + value;
    }

    path = path.substr(0, queryBegin) + out +
           ((fragment == std::wstring::npos)
                ? L""
                : path.substr(fragment));
}

static std::wstring BuildAudioUrl(const std::wstring& originalUrl,
                                   DWORD sampleRate,
                                   WORD channels)
{
    UrlParts parts;

    if (!CrackUrl(originalUrl, parts))
        return L"";

    SetQueryParameter(
        parts.path,
        L"rate",
        std::to_wstring(sampleRate));

    SetQueryParameter(
        parts.path,
        L"channels",
        std::to_wstring(channels));

    const std::wstring scheme =
        parts.https ? L"https://" : L"http://";

    std::wstring result = scheme + parts.host;

    const bool defaultPort =
        (parts.https &&
         parts.port == INTERNET_DEFAULT_HTTPS_PORT) ||
        (!parts.https &&
         parts.port == INTERNET_DEFAULT_HTTP_PORT);

    if (!defaultPort || parts.portSpecified)
        result += L":" + std::to_wstring(parts.port);

    result += parts.path;

    return result;
}

static std::wstring BuildVolumeUrl(const UrlParts& source,
                                   const wchar_t* volumePath)
{
    UrlParts parts = source;

    if (parts.port == 8880)
        parts.port = 8080;

    parts.path = volumePath;

    const std::wstring scheme =
        parts.https ? L"https://" : L"http://";

    std::wstring result = scheme + parts.host;

    const bool defaultPort =
        (parts.https &&
         parts.port == INTERNET_DEFAULT_HTTPS_PORT) ||
        (!parts.https &&
         parts.port == INTERNET_DEFAULT_HTTP_PORT);

    if (!defaultPort)
        result += L":" + std::to_wstring(parts.port);

    result += parts.path;
    return result;
}

static bool SendVolumeRequest(const UrlParts& source,
                              const wchar_t* volumePath)
{
    UrlParts parts = source;

    if (parts.port == 8880)
        parts.port = 8080;

    parts.path = volumePath;

    HINTERNET session = WinHttpOpen(
        L"workdayAlarmClockGo/1.0",
        WINHTTP_ACCESS_TYPE_NO_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0);

    if (!session) {
        PrintWinError(L"WinHttpOpen(volume)");
        return false;
    }

    WinHttpSetTimeouts(session, 1000, 1000, 1000, 1000);

    HINTERNET connect = WinHttpConnect(
        session,
        parts.host.c_str(),
        parts.port,
        0);

    if (!connect) {
        PrintWinError(L"WinHttpConnect(volume)");
        WinHttpCloseHandle(session);
        return false;
    }

    HINTERNET request = WinHttpOpenRequest(
        connect,
        L"GET",
        parts.path.c_str(),
        nullptr,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        parts.https ? WINHTTP_FLAG_SECURE : 0);

    if (!request) {
        PrintWinError(L"WinHttpOpenRequest(volume)");
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return false;
    }

    bool ok = false;

    if (WinHttpSendRequest(
            request,
            WINHTTP_NO_ADDITIONAL_HEADERS,
            0,
            WINHTTP_NO_REQUEST_DATA,
            0,
            0,
            0) &&
        WinHttpReceiveResponse(request, nullptr)) {
        DWORD status = 0;
        DWORD statusSize = sizeof(status);

        if (WinHttpQueryHeaders(
                request,
                WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX,
                &status,
                &statusSize,
                WINHTTP_NO_HEADER_INDEX)) {
            ok = (status >= 200 && status < 300);

            if (!ok) {
                std::wcerr
                    << L"Volume request failed, HTTP status="
                    << status
                    << L"\n";
            }
        }
    } else {
        PrintWinError(L"Volume request");
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);

    return ok;
}

static void HandlePendingVolumeRequests(
    const std::vector<std::unique_ptr<Destination>>& destinations)
{
    const unsigned int upRequests = g_volumeUpRequests.exchange(0);
    const unsigned int downRequests = g_volumeDownRequests.exchange(0);

    if (upRequests == 0 && downRequests == 0)
        return;

    // 音量键对所有目标广播；每一路使用自己当前生效的地址。
    for (const auto& destinationPtr : destinations) {
        Destination& destination = *destinationPtr;

        UrlParts parts;
        {
            std::lock_guard<std::mutex> lock(destination.partsMutex);
            parts = destination.activeParts;
        }

        for (unsigned int i = 0; i < upRequests; ++i) {
            if (SendVolumeRequest(parts, L"/volp"))
                std::wcout << L"Volume up.\n";
        }

        for (unsigned int i = 0; i < downRequests; ++i) {
            if (SendVolumeRequest(parts, L"/volm"))
                std::wcout << L"Volume down.\n";
        }
    }
}

static bool ReadHttpResponse(HINTERNET request,
                              DWORD& status,
                              std::string& body,
                              std::wstring& location)
{
    status = 0;
    body.clear();
    location.clear();

    if (!WinHttpReceiveResponse(request, nullptr))
        return false;

    DWORD statusSize = sizeof(status);
    if (!WinHttpQueryHeaders(
            request,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX,
            &status,
            &statusSize,
            WINHTTP_NO_HEADER_INDEX)) {
        status = 0;
    }

    DWORD locationSize = 0;
    WinHttpQueryHeaders(
        request,
        WINHTTP_QUERY_LOCATION,
        WINHTTP_HEADER_NAME_BY_INDEX,
        nullptr,
        &locationSize,
        WINHTTP_NO_HEADER_INDEX);

    if (GetLastError() == ERROR_INSUFFICIENT_BUFFER && locationSize > 0) {
        std::vector<wchar_t> buffer(locationSize / sizeof(wchar_t) + 1);
        if (WinHttpQueryHeaders(
                request,
                WINHTTP_QUERY_LOCATION,
                WINHTTP_HEADER_NAME_BY_INDEX,
                buffer.data(),
                &locationSize,
                WINHTTP_NO_HEADER_INDEX)) {
            location.assign(buffer.data(), locationSize / sizeof(wchar_t));
        }
    }

    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available))
            break;

        if (available == 0)
            break;

        std::vector<char> buffer(available);
        DWORD read = 0;
        if (!WinHttpReadData(
                request,
                buffer.data(),
                available,
                &read)) {
            break;
        }

        if (read == 0)
            break;

        body.append(buffer.data(), read);
    }

    return true;
}

static void PrintHttpResponse(DWORD status,
                              const std::string& body,
                              const std::wstring& location)
{
    if (status != 0)
        std::wcerr << L"HTTP status: " << status << L"\n";

    if (!location.empty())
        std::wcerr << L"HTTP Location: " << location << L"\n";

    if (!body.empty()) {
        std::wcerr << L"HTTP response body: ";
        std::cerr.write(body.data(), static_cast<std::streamsize>(body.size()));
        std::cerr << "\n";
    }
}

static bool WriteRaw(HINTERNET request,
                     const BYTE* data,
                     DWORD bytes,
                     WriteStats& stats)
{
    while (bytes > 0) {
        DWORD written = 0;

        const auto writeStart = std::chrono::steady_clock::now();

        if (!WinHttpWriteData(
                request,
                data,
                bytes,
                &written)) {
            stats.lastWinHttpError = GetLastError();
            PrintWinError(L"WinHttpWriteData", stats.lastWinHttpError);
            return false;
        }

        const double writeMs =
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - writeStart).count();

        ++stats.writeCalls;
        if (writeMs >= 1.0)
            ++stats.writeOver1MsCalls;
        stats.writeMaxMs = std::max(stats.writeMaxMs, writeMs);

        if (writeMs > kBlockedWriteMs)
            stats.blockedMs += writeMs;

        stats.lastWinHttpError = ERROR_SUCCESS;

        if (written == 0) {
            std::wcerr
                << L"WinHttpWriteData wrote 0 bytes.\n";
            return false;
        }

        data += written;
        bytes -= written;
    }

    return true;
}

static bool WritePcm(HINTERNET request,
                     const BYTE* data,
                     DWORD bytes,
                     WriteStats& stats)
{
    return WriteRaw(request, data, bytes, stats);
}

static int16_t FloatToS16(float value)
{
    if (!std::isfinite(value))
        value = 0.0f;

    value = std::clamp(value, -1.0f, 1.0f);

    if (value >= 1.0f)
        return 32767;

    if (value <= -1.0f)
        return -32768;

    const float scaled = value * 32768.0f;

    long v = std::lround(scaled);

    if (v > 32767)
        v = 32767;

    if (v < -32768)
        v = -32768;

    return static_cast<int16_t>(v);
}

static int16_t S24ToS16(const BYTE* p)
{
    int32_t value =
        static_cast<int32_t>(p[0]) |
        (static_cast<int32_t>(p[1]) << 8) |
        (static_cast<int32_t>(p[2]) << 16);

    if (value & 0x00800000)
        value |= 0xFF000000;

    // 24-bit signed -> 16-bit signed
    value >>= 8;

    if (value > 32767)
        value = 32767;

    if (value < -32768)
        value = -32768;

    return static_cast<int16_t>(value);
}

static int16_t S32ToS16(const BYTE* p)
{
    int32_t value = 0;

    std::memcpy(&value, p, sizeof(value));

    // 32-bit signed -> 16-bit signed
    value >>= 16;

    if (value > 32767)
        value = 32767;

    if (value < -32768)
        value = -32768;

    return static_cast<int16_t>(value);
}

enum class SampleType {
    S16,
    S24,
    S32,
    FLOAT32,
    UNSUPPORTED
};

static SampleType GetSampleType(const WAVEFORMATEX* format)
{
    if (!format)
        return SampleType::UNSUPPORTED;

    if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        const auto* ext =
            reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format);

        if (IsEqualGUID(
                ext->SubFormat,
                KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)) {
            if (format->wBitsPerSample == 32)
                return SampleType::FLOAT32;

            return SampleType::UNSUPPORTED;
        }

        if (IsEqualGUID(
                ext->SubFormat,
                KSDATAFORMAT_SUBTYPE_PCM)) {
            switch (format->wBitsPerSample) {
            case 16:
                return SampleType::S16;
            case 24:
                return SampleType::S24;
            case 32:
                return SampleType::S32;
            default:
                return SampleType::UNSUPPORTED;
            }
        }

        return SampleType::UNSUPPORTED;
    }

    if (format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        if (format->wBitsPerSample == 32)
            return SampleType::FLOAT32;

        return SampleType::UNSUPPORTED;
    }

    if (format->wFormatTag == WAVE_FORMAT_PCM) {
        switch (format->wBitsPerSample) {
        case 16:
            return SampleType::S16;
        case 24:
            return SampleType::S24;
        case 32:
            return SampleType::S32;
        default:
            return SampleType::UNSUPPORTED;
        }
    }

    return SampleType::UNSUPPORTED;
}

static const wchar_t* SampleTypeName(SampleType type)
{
    switch (type) {
    case SampleType::S16:
        return L"s16";
    case SampleType::S24:
        return L"s24";
    case SampleType::S32:
        return L"s32";
    case SampleType::FLOAT32:
        return L"float32";
    default:
        return L"unsupported";
    }
}

static bool ConvertToS16(
    const BYTE* input,
    UINT32 frames,
    const WAVEFORMATEX* format,
    SampleType type,
    std::vector<int16_t>& output)
{
    if (!input || !format)
        return false;

    const UINT32 channels = format->nChannels;

    if (channels == 0)
        return false;

    const size_t sampleCount =
        static_cast<size_t>(frames) * channels;

    output.resize(sampleCount);

    switch (type) {
    case SampleType::S16:
        {
            std::memcpy(
                output.data(),
                input,
                sampleCount * sizeof(int16_t));
        }
        return true;

    case SampleType::FLOAT32:
        {
            const float* src =
                reinterpret_cast<const float*>(input);

            for (size_t i = 0; i < sampleCount; ++i)
                output[i] = FloatToS16(src[i]);
        }
        return true;

    case SampleType::S24:
        {
            for (size_t i = 0; i < sampleCount; ++i)
                output[i] = S24ToS16(input + i * 3);
        }
        return true;

    case SampleType::S32:
        {
            for (size_t i = 0; i < sampleCount; ++i)
                output[i] = S32ToS16(input + i * 4);
        }
        return true;

    default:
        return false;
    }
}

int wmain(int argc, wchar_t* argv[])
{
    // 设置成高优先级
    if (!SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS))
        PrintWinError(L"SetPriorityClass(HIGH_PRIORITY_CLASS)");

    std::wstring input;

    if (argc >= 2) {
        input = argv[1];
    } else {
        std::wcout
            << L"workdayAlarmClockGo URL end with /aplay: ";

        std::getline(std::wcin, input);
    }

    // 命令行参数：
    //   argv[1] 目标地址，可用半角逗号分隔多个；L/R 前缀表示只发左/右声道
    //   argv[2] 发送水位 ms（多攒这么多音频才发，等于额外增加延迟）
    //   argv[3] 允许的最大音频年龄 ms，超过就丢旧音频
    //   argv[4] 队列硬上限 ms（兜底，正常不该触发）

    if (argc >= 3) {
        kSendWaterMs = std::wcstod(argv[2], nullptr);
        if (kSendWaterMs < 0.0)
            kSendWaterMs = 0.0;
    }

    if (argc >= 4) {
        kDropBacklogMs = std::wcstod(argv[3], nullptr);
    }

    if (argc >= 5) {
        kDropAllBacklogMs = std::wcstod(argv[4], nullptr);
    }

    if (input.empty()) {
        std::wcerr << L"No URL.\n";
        return 1;
    }

    const std::vector<DestinationSpec> specs =
        ParseDestinationSpecs(input);

    if (specs.empty()) {
        std::wcerr << L"No valid URL.\n";
        return 1;
    }

    // 先验证每个地址，避免线程启动后才发现 URL 写错。
    for (const DestinationSpec& spec : specs) {
        UrlParts parts;
        if (!CrackUrl(spec.url, parts))
            return 1;
    }

    if (!SetConsoleCtrlHandler(
            ConsoleHandler,
            TRUE)) {
        PrintWinError(L"SetConsoleCtrlHandler");
        return 1;
    }

    HRESULT hr =
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    if (FAILED(hr)) {
        PrintHr(L"CoInitializeEx", hr);
        return 1;
    }

    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* device = nullptr;
    IAudioClient* audioClient = nullptr;
    IAudioCaptureClient* captureClient = nullptr;
    WAVEFORMATEX* format = nullptr;
    HANDLE audioCaptureEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!audioCaptureEvent) {
        PrintWinError(L"CreateEventW(audio capture)");
        CoUninitialize();
        SetConsoleCtrlHandler(ConsoleHandler, FALSE);
        return 1;
    }

    bool ok = false;
    bool keyboardHookStarted = false;
    const bool serviceMode = (argc >= 2);

    const auto run = [&]() -> bool {
        std::vector<std::unique_ptr<Destination>> destinations;
        std::thread captureThread;
        std::thread volumeThread;
        std::vector<std::thread> senderThreads;

        std::atomic_bool captureStop{false};
        std::atomic_bool captureFault{false};
        std::atomic_bool volumeThreadStop{false};
        std::atomic_bool qpcOffsetValid{false};
        std::atomic<double> qpcOffset100ns{0.0};

        LARGE_INTEGER qpcFrequency{};

        const auto stopThreads = [&]() {
            // 先通知所有线程退出，再逐个 join，避免串行等待。
            volumeThreadStop.store(true);
            captureStop.store(true);
            for (auto& destination : destinations)
                destination->stop.store(true);

            if (volumeThread.joinable())
                volumeThread.join();
            if (captureThread.joinable())
                captureThread.join();
            for (auto& thread : senderThreads) {
                if (thread.joinable())
                    thread.join();
            }
            senderThreads.clear();
        };

        // 初始化默认输出设备的 loopback 采集。
        hr = CoCreateInstance(
            __uuidof(MMDeviceEnumerator),
            nullptr,
            CLSCTX_ALL,
            __uuidof(IMMDeviceEnumerator),
            reinterpret_cast<void**>(&enumerator));

        if (FAILED(hr)) {
            PrintHr(L"CoCreateInstance(MMDeviceEnumerator)", hr);
            stopThreads();
            return false;
        }

        hr = enumerator->GetDefaultAudioEndpoint(
            eRender,
            eMultimedia,
            &device);

        if (FAILED(hr)) {
            PrintHr(L"GetDefaultAudioEndpoint", hr);
            stopThreads();
            return false;
        }

        hr = device->Activate(
            __uuidof(IAudioClient),
            CLSCTX_ALL,
            nullptr,
            reinterpret_cast<void**>(&audioClient));

        if (FAILED(hr)) {
            PrintHr(L"IMMDevice::Activate(IAudioClient)", hr);
            stopThreads();
            return false;
        }

        hr = audioClient->GetMixFormat(&format);

        if (FAILED(hr)) {
            PrintHr(L"IAudioClient::GetMixFormat", hr);
            stopThreads();
            return false;
        }

        const SampleType sampleType = GetSampleType(format);

        if (sampleType == SampleType::UNSUPPORTED) {
            std::wcerr
                << L"Unsupported WASAPI sample format.\n";
            stopThreads();
            return false;
        }

        std::wcout
            << L"Capture format: "
            << format->nSamplesPerSec
            << L" Hz, "
            << format->nChannels
            << L" ch, "
            << format->wBitsPerSample
            << L" bit, block="
            << format->nBlockAlign;

        if (format->wFormatTag == WAVE_FORMAT_PCM)
            std::wcout << L", PCM";
        else if (format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT)
            std::wcout << L", IEEE float";
        else if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
            std::wcout << L", WAVE_FORMAT_EXTENSIBLE";

        std::wcout
            << L", source="
            << SampleTypeName(sampleType)
            << L"\n";

        // 建立每一路目标。Destination 内含 mutex，用 unique_ptr 保证地址稳定。
        destinations.reserve(specs.size());
        for (size_t i = 0; i < specs.size(); ++i) {
            auto destination = std::make_unique<Destination>();
            destination->url = specs[i].url;
            destination->channelMode = specs[i].channelMode;
            destination->index = i;
            ConfigureDestination(
                *destination,
                format->nSamplesPerSec,
                format->nChannels);
            destinations.push_back(std::move(destination));
        }

        if (!InitializeLowLatencyLoopback(
                device, &audioClient, format, audioCaptureEvent)) {
            stopThreads();
            return false;
        }

        // 共享模式下申请值不一定被采纳，把真实缓冲大小打出来。
        UINT32 actualBufferFrames = 0;

        if (SUCCEEDED(audioClient->GetBufferSize(&actualBufferFrames)) &&
            format->nSamplesPerSec > 0) {
            std::wcout
                << L"Actual capture buffer: "
                << actualBufferFrames
                << L" frames ("
                << (actualBufferFrames * 1000ULL) /
                       format->nSamplesPerSec
                << L" ms)\n";
        }

        hr = audioClient->GetService(
            __uuidof(IAudioCaptureClient),
            reinterpret_cast<void**>(&captureClient));

        if (FAILED(hr)) {
            PrintHr(L"IAudioClient::GetService(IAudioCaptureClient)", hr);
            stopThreads();
            return false;
        }

        if (!QueryPerformanceFrequency(&qpcFrequency) ||
            qpcFrequency.QuadPart <= 0) {
            PrintWinError(L"QueryPerformanceFrequency");
            stopThreads();
            return false;
        }

        std::wcout
            << L"Output format: "
            << format->nSamplesPerSec
            << L" Hz, s16le\n";

        for (const auto& destination : destinations) {
            const wchar_t* modeName =
                destination->channelMode == ChannelMode::Left
                    ? L"left"
                    : (destination->channelMode == ChannelMode::Right
                           ? L"right"
                           : L"stereo");

            std::wcout
                << L"Destination "
                << (destination->index + 1)
                << L": "
                << modeName
                << L", "
                << destination->outChannels
                << L" ch -> "
                << BuildAudioUrl(
                       destination->url,
                       format->nSamplesPerSec,
                       static_cast<WORD>(destination->outChannels))
                << L"\n";
        }

        std::wcout
            << L"Send watermark: " << kSendWaterMs
            << L" ms (extra latency), drop above "
            << std::max(kDropBacklogMs, kSendWaterMs + 10.0)
            << L" ms\n";

        if (!StartKeyboardHook()) {
            stopThreads();
            return false;
        }
        keyboardHookStarted = true;

        // 音量请求走独立线程：SendVolumeRequest 是同步 HTTP 调用，
        // 超时 1 秒，跑在发送线程上会把音频卡住最多 1 秒。
        volumeThread = std::thread([&]() {
            while (!g_stop.load() && !volumeThreadStop.load()) {
                HandlePendingVolumeRequests(destinations);
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(20));
            }
        });

        const double effectiveDropMs =
            std::max(kDropBacklogMs, kSendWaterMs + 10.0);

        // 把 WASAPI 时间戳换算成“这段音频现在有多旧”（毫秒）。
        // 返回值已经扣掉校准偏移；偏移尚未建立时返回 0（不丢）。
        const auto computeAgeMs = [&](UINT64 qpcStart,
                                      UINT32 frames) -> double {
            LARGE_INTEGER nowQpc{};
            if (!QueryPerformanceCounter(&nowQpc))
                return 0.0;

            const double now100ns =
                static_cast<double>(nowQpc.QuadPart) * 10000000.0 /
                static_cast<double>(qpcFrequency.QuadPart);
            const double end100ns =
                static_cast<double>(qpcStart) +
                static_cast<double>(frames) * 10000000.0 /
                    static_cast<double>(format->nSamplesPerSec);

            if (!qpcOffsetValid.load(std::memory_order_relaxed)) {
                qpcOffset100ns.store(
                    end100ns - now100ns,
                    std::memory_order_relaxed);
                qpcOffsetValid.store(true, std::memory_order_release);
            }

            const double offset =
                qpcOffset100ns.load(std::memory_order_relaxed);
            return (now100ns + offset - end100ns) / 10000.0;
        };

        // 把 PCM 入队到某一路。采集线程只做这一步，永远不等网络。
        // 时间戳判断和丢弃逻辑与原单路版本一致，只是按本路输出声道数换算字节。
        const auto enqueuePcm = [&](Destination& destination,
                                    const std::vector<int16_t>& pcm,
                                    UINT64 qpcStart,
                                    UINT32 frames,
                                    bool hasTimestamp) {
            if (pcm.empty())
                return;

            const BYTE* bytes = reinterpret_cast<const BYTE*>(pcm.data());
            size_t size = pcm.size() * sizeof(int16_t);
            UINT32 keepFrames = frames;
            UINT64 keepQpcStart = qpcStart;

            if (hasTimestamp && format->nSamplesPerSec > 0) {
                const double ageMs = computeAgeMs(qpcStart, frames);
                if (ageMs > effectiveDropMs) {
                    const double excessMs = ageMs - effectiveDropMs;
                    uint64_t dropFrames = static_cast<uint64_t>(
                        std::ceil(excessMs * format->nSamplesPerSec /
                                  1000.0));

                    if (dropFrames >= frames) {
                        std::wcout
                            << L"[dest " << (destination.index + 1)
                            << L"] Stale packet ("
                            << static_cast<long long>(ageMs)
                            << L" ms old); dropped entirely.\n";
                        return;
                    }

                    const size_t dropBytes =
                        static_cast<size_t>(dropFrames) *
                        static_cast<size_t>(destination.outChannels) *
                        sizeof(int16_t);
                    bytes += dropBytes;
                    size -= dropBytes;
                    keepFrames = frames - static_cast<UINT32>(dropFrames);
                    keepQpcStart =
                        qpcStart +
                        static_cast<UINT64>(
                            static_cast<double>(dropFrames) * 10000000.0 /
                            static_cast<double>(format->nSamplesPerSec));

                    if (!destination.captureDropActive) {
                        std::wcout
                            << L"[dest " << (destination.index + 1)
                            << L"] Audio "
                            << static_cast<long long>(ageMs)
                            << L" ms old; dropped "
                            << static_cast<long long>(excessMs)
                            << L" ms to catch up.\n";
                        destination.captureDropActive = true;
                    }
                } else if (destination.captureDropActive) {
                    std::wcout
                        << L"[dest " << (destination.index + 1)
                        << L"] Audio age back to normal.\n";
                    destination.captureDropActive = false;
                }
            }

            double staleDroppedMs = 0.0;
            {
                std::lock_guard<std::mutex> lock(destination.queue.mutex);

                while (destination.queue.queuedBytes + size >
                           destination.maxQueuedBytes &&
                       !destination.queue.blocks.empty()) {
                    destination.queue.queuedBytes -=
                        destination.queue.blocks.front().data.size();
                    destination.queue.blocks.pop_front();
                }

                PcmBlock block;
                block.data.assign(bytes, bytes + size);
                block.qpcStart = keepQpcStart;
                block.frames = keepFrames;
                block.hasTimestamp = hasTimestamp;

                destination.queue.queuedBytes += size;
                destination.queue.blocks.push_back(std::move(block));

                if (format->nSamplesPerSec > 0 &&
                    qpcOffsetValid.load(std::memory_order_relaxed)) {
                    LARGE_INTEGER nowQpc{};
                    if (QueryPerformanceCounter(&nowQpc)) {
                        const double now100ns =
                            static_cast<double>(nowQpc.QuadPart) *
                                10000000.0 /
                                static_cast<double>(
                                    qpcFrequency.QuadPart) +
                            qpcOffset100ns.load(std::memory_order_relaxed);

                        size_t staleDropped = 0;
                        while (!destination.queue.blocks.empty() &&
                               destination.queue.blocks.front().hasTimestamp) {
                            const PcmBlock& front =
                                destination.queue.blocks.front();
                            const double end100ns =
                                static_cast<double>(front.qpcStart) +
                                static_cast<double>(front.frames) *
                                    10000000.0 /
                                    static_cast<double>(
                                        format->nSamplesPerSec);
                            const double frontAgeMs =
                                (now100ns - end100ns) / 10000.0;
                            if (frontAgeMs <= effectiveDropMs)
                                break;

                            staleDropped += front.data.size();
                            destination.queue.queuedBytes -=
                                front.data.size();
                            destination.queue.blocks.pop_front();
                        }

                        if (staleDropped > 0) {
                            staleDroppedMs =
                                static_cast<double>(staleDropped) /
                                destination.sendBytesPerMs;
                        }
                    }
                }
            }

            if (staleDroppedMs > 0.0) {
                if (!destination.queueTrimActive) {
                    std::wcout
                        << L"[dest " << (destination.index + 1)
                        << L"] Queue backlog; dropped "
                        << static_cast<long long>(staleDroppedMs)
                        << L" ms of stale audio.\n";
                    destination.queueTrimActive = true;
                }
            } else if (destination.queueTrimActive) {
                std::wcout
                    << L"[dest " << (destination.index + 1)
                    << L"] Queue is fresh again.\n";
                destination.queueTrimActive = false;
            }
        };

        const auto clearSendQueue = [&](Destination& destination) {
            std::lock_guard<std::mutex> lock(destination.queue.mutex);
            destination.queue.blocks.clear();
            destination.queue.queuedBytes = 0;
        };

        // 从队头取走当前所有可用数据（最多本路的 sendChunkBytes）。
        const auto takeSendBlock = [&](Destination& destination) {
            std::vector<BYTE> block;
            std::lock_guard<std::mutex> lock(destination.queue.mutex);

            size_t taken = 0;
            while (taken < destination.sendChunkBytes &&
                   !destination.queue.blocks.empty()) {
                PcmBlock& front = destination.queue.blocks.front();
                const size_t need = destination.sendChunkBytes - taken;
                if (front.data.size() <= need) {
                    block.insert(block.end(), front.data.begin(),
                                 front.data.end());
                    taken += front.data.size();
                    destination.queue.queuedBytes -= front.data.size();
                    destination.queue.blocks.pop_front();
                } else {
                    block.insert(block.end(), front.data.begin(),
                                 front.data.begin() + need);
                    front.data.erase(front.data.begin(),
                                     front.data.begin() + need);
                    taken += need;
                    destination.queue.queuedBytes -= need;
                }
            }

            return block;
        };

        // 采集线程：一次 loopback 采集，按路复制到各自的发送队列。
        const auto startCaptureThread = [&]() {
            captureStop.store(false);
            captureFault.store(false);

            std::thread worker([&]() {
                std::vector<int16_t> capturePcm;
                std::vector<int16_t> channelPcm;
                bool captureTimestampWarned = false;

                while (!g_stop.load() && !captureStop.load()) {
                    const DWORD audioWait =
                        WaitForSingleObject(audioCaptureEvent, 5);
                    if (audioWait == WAIT_FAILED) {
                        PrintWinError(
                            L"WaitForSingleObject(audio capture event)");
                        captureFault.store(true);
                        break;
                    }
                    if (audioWait != WAIT_OBJECT_0)
                        continue;

                    UINT32 packetFrames = 0;
                    const HRESULT packetHr =
                        captureClient->GetNextPacketSize(&packetFrames);
                    if (FAILED(packetHr)) {
                        PrintHr(L"IAudioCaptureClient::GetNextPacketSize",
                                packetHr);
                        captureFault.store(true);
                        break;
                    }

                    while (packetFrames > 0 &&
                           !g_stop.load() && !captureStop.load()) {
                        BYTE* data = nullptr;
                        UINT32 frames = 0;
                        DWORD flags = 0;
                        UINT64 qpcPosition = 0;

                        const HRESULT bufferHr = captureClient->GetBuffer(
                            &data, &frames, &flags, nullptr, &qpcPosition);
                        if (FAILED(bufferHr)) {
                            PrintHr(L"IAudioCaptureClient::GetBuffer",
                                    bufferHr);
                            captureFault.store(true);
                            break;
                        }

                        bool timestampAvailable =
                            (flags &
                             AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) == 0 &&
                            qpcPosition != 0;
                        if (!timestampAvailable && !captureTimestampWarned) {
                            std::wcerr
                                << L"WASAPI packet timestamp unavailable; using write-time fallback for this capture session.\n";
                            captureTimestampWarned = true;
                        }

                        if (frames > 0 && data != nullptr) {
                            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                                capturePcm.assign(
                                    static_cast<size_t>(frames) *
                                        format->nChannels,
                                    0);
                            } else if (!ConvertToS16(data, frames, format,
                                                     sampleType,
                                                     capturePcm)) {
                                std::wcerr << L"PCM conversion failed.\n";
                                captureClient->ReleaseBuffer(frames);
                                captureFault.store(true);
                                break;
                            }

                            if (!capturePcm.empty()) {
                                for (const auto& destinationPtr :
                                     destinations) {
                                    Destination& destination =
                                        *destinationPtr;

                                    if (destination.channelMode ==
                                        ChannelMode::Stereo) {
                                        enqueuePcm(
                                            destination,
                                            capturePcm,
                                            qpcPosition,
                                            frames,
                                            timestampAvailable);
                                        continue;
                                    }

                                    // L/R：只取第一/第二声道，输出单声道。
                                    // 源本身是单声道时，两路都复制该声道。
                                    const UINT32 channels =
                                        format->nChannels;
                                    const size_t wantedChannel =
                                        destination.channelMode ==
                                                ChannelMode::Left
                                            ? 0
                                            : 1;
                                    const size_t sourceChannel =
                                        std::min<size_t>(
                                            wantedChannel,
                                            static_cast<size_t>(channels) - 1);

                                    channelPcm.resize(frames);
                                    for (UINT32 frame = 0;
                                         frame < frames;
                                         ++frame) {
                                        channelPcm[frame] =
                                            capturePcm[
                                                static_cast<size_t>(frame) *
                                                    channels +
                                                sourceChannel];
                                    }

                                    enqueuePcm(
                                        destination,
                                        channelPcm,
                                        qpcPosition,
                                        frames,
                                        timestampAvailable);
                                }
                            }
                        }

                        captureClient->ReleaseBuffer(frames);

                        const HRESULT nextHr =
                            captureClient->GetNextPacketSize(&packetFrames);
                        if (FAILED(nextHr)) {
                            PrintHr(
                                L"IAudioCaptureClient::GetNextPacketSize",
                                nextHr);
                            captureFault.store(true);
                            break;
                        }
                    }
                }
            });

            if (captureThread.joinable())
                captureThread.join();
            captureThread = std::move(worker);
        };

        // 每路一个发送线程：独立的 HTTP 连接、重连计数、队列和写入统计。
        const auto runSender = [&](Destination& destination) {
            const DWORD sampleRate = format->nSamplesPerSec;
            const UINT32 outChannels = destination.outChannels;

            std::wstring finalUrl = BuildAudioUrl(
                destination.url,
                sampleRate,
                static_cast<WORD>(outChannels));

            if (finalUrl.empty()) {
                std::wcerr
                    << L"[dest " << (destination.index + 1)
                    << L"] Invalid URL.\n";
                return;
            }

            UrlParts activeParts;
            if (!CrackUrl(finalUrl, activeParts)) {
                std::wcerr
                    << L"[dest " << (destination.index + 1)
                    << L"] Invalid URL.\n";
                return;
            }

            {
                std::lock_guard<std::mutex> lock(destination.partsMutex);
                destination.activeParts = activeParts;
            }

            HINTERNET session = nullptr;
            HINTERNET connect = nullptr;
            HINTERNET request = nullptr;
            WriteStats stats;

            bool hasStreamedSuccessfully = false;
            bool connected = false;
            bool streamConfirmed = false;
            int successfulChunks = 0;
            int autoReconnectAttempts = 0;
            bool retryAfterDelay = false;
            bool serviceRetryDelay = false;
            bool shortReconnectDelay = false;
            bool waitingForEnter = false;

            auto lastDiagTime = std::chrono::steady_clock::now();
            size_t diagSentBytes = 0;

            const auto publishActiveParts = [&]() {
                std::lock_guard<std::mutex> lock(
                    destination.partsMutex);
                destination.activeParts = activeParts;
            };

            const auto closeHttp = [&]() {
                if (request) {
                    WinHttpCloseHandle(request);
                    request = nullptr;
                }
                if (connect) {
                    WinHttpCloseHandle(connect);
                    connect = nullptr;
                }
                if (session) {
                    WinHttpCloseHandle(session);
                    session = nullptr;
                }
            };

            const auto openHttp = [&]() -> bool {
                for (;;) {
                    closeHttp();
                    DWORD failureError = ERROR_SUCCESS;

                    session = WinHttpOpen(
                        L"workdayAlarmClockGo/1.0",
                        WINHTTP_ACCESS_TYPE_NO_PROXY,
                        WINHTTP_NO_PROXY_NAME,
                        WINHTTP_NO_PROXY_BYPASS,
                        0);

                    if (!session) {
                        failureError = GetLastError();
                        PrintWinError(L"WinHttpOpen", failureError);
                    } else {
                        WinHttpSetTimeouts(
                            session, 5000, 5000, 5000, 5000);

                        connect = WinHttpConnect(
                            session,
                            activeParts.host.c_str(),
                            activeParts.port,
                            0);

                        if (!connect) {
                            failureError = GetLastError();
                            PrintWinError(
                                L"WinHttpConnect", failureError);
                        } else {
                            request = WinHttpOpenRequest(
                                connect,
                                L"PUT",
                                activeParts.path.c_str(),
                                nullptr,
                                WINHTTP_NO_REFERER,
                                WINHTTP_DEFAULT_ACCEPT_TYPES,
                                activeParts.https
                                    ? WINHTTP_FLAG_SECURE
                                    : 0);

                            if (!request) {
                                failureError = GetLastError();
                                PrintWinError(
                                    L"WinHttpOpenRequest",
                                    failureError);
                            } else {
                                const wchar_t headers[] =
                                    L"Content-Type: application/octet-stream\r\n"
                                    L"Content-Length: 68719476735\r\n"
                                    L"Expect: 100-continue\r\n";

                                if (WinHttpSendRequest(
                                        request,
                                        headers,
                                        static_cast<DWORD>(-1),
                                        WINHTTP_NO_REQUEST_DATA,
                                        0,
                                        WINHTTP_IGNORE_REQUEST_TOTAL_LENGTH,
                                        0)) {
                                    return true;
                                }

                                failureError = GetLastError();
                                PrintWinError(
                                    L"WinHttpSendRequest",
                                    failureError);
                            }
                        }
                    }

                    closeHttp();

                    if (!hasStreamedSuccessfully &&
                        IsConnectionFailure(failureError) &&
                        AdvancePortFallback(activeParts)) {
                        publishActiveParts();
                        std::wcout
                            << L"[dest " << (destination.index + 1)
                            << L"] Cannot connect; trying port "
                            << activeParts.port << L".\n";
                        continue;
                    }

                    return false;
                }
            };

            const auto waitFor = [&](std::chrono::milliseconds duration) {
                const auto end =
                    std::chrono::steady_clock::now() + duration;

                while (!g_stop.load() && !destination.stop.load() &&
                       !destination.reconnectRequested.load()) {
                    if (std::chrono::steady_clock::now() >= end)
                        break;
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(20));
                }
            };

            const auto markDisconnected = [&]() {
                connected = false;
                streamConfirmed = false;
            };

            while (!g_stop.load() && !destination.stop.load()) {
                if (destination.reconnectRequested.exchange(false)) {
                    closeHttp();
                    markDisconnected();
                    successfulChunks = 0;
                    autoReconnectAttempts = 0;
                    retryAfterDelay = false;
                    serviceRetryDelay = false;
                    shortReconnectDelay = true;
                    waitingForEnter = false;
                    stats = WriteStats{};
                    clearSendQueue(destination);
                    std::wcout
                        << L"[dest " << (destination.index + 1)
                        << L"] Reconnect requested.\n";
                }

                if (!connected) {
                    if (waitingForEnter) {
                        while (!g_stop.load() &&
                               !destination.stop.load() &&
                               !destination.reconnectRequested.load()) {
                            std::this_thread::sleep_for(
                                std::chrono::milliseconds(50));
                        }
                        if (g_stop.load() || destination.stop.load())
                            break;
                        continue;
                    }

                    if (serviceRetryDelay) {
                        waitFor(
                            std::chrono::duration_cast<
                                std::chrono::milliseconds>(
                                kServiceRetryDelay));
                    } else if (retryAfterDelay) {
                        waitFor(std::chrono::seconds(1));
                    } else if (shortReconnectDelay) {
                        waitFor(
                            std::chrono::duration_cast<
                                std::chrono::milliseconds>(
                                kCongestionReconnectDelay));
                    }

                    if (g_stop.load() || destination.stop.load())
                        break;
                    if (destination.reconnectRequested.load())
                        continue;

                    std::wcout
                        << L"[dest " << (destination.index + 1)
                        << L"] Connecting...\n";

                    connected = openHttp();
                    retryAfterDelay = false;
                    shortReconnectDelay = false;

                    if (!connected) {
                        ++autoReconnectAttempts;

                        if (serviceMode && serviceRetryDelay) {
                            std::wcerr
                                << L"[dest "
                                << (destination.index + 1)
                                << L"] HTTP connect failed; retrying in 10 seconds.\n";
                            retryAfterDelay = false;
                        } else if (
                            autoReconnectAttempts <
                            kMaxAutoReconnectAttempts) {
                            std::wcerr
                                << L"[dest "
                                << (destination.index + 1)
                                << L"] HTTP connect failed; retrying in 1 second (attempt "
                                << autoReconnectAttempts
                                << L"/"
                                << kMaxAutoReconnectAttempts
                                << L")...\n";
                            retryAfterDelay = true;
                        } else if (serviceMode) {
                            std::wcerr
                                << L"[dest "
                                << (destination.index + 1)
                                << L"] Reconnect failed 3 times; retrying in 10 seconds.\n";
                            serviceRetryDelay = true;
                            retryAfterDelay = false;
                        } else {
                            std::wcerr
                                << L"[dest "
                                << (destination.index + 1)
                                << L"] Reconnect failed 3 times; press Enter to retry.\n";
                            waitingForEnter = true;
                            retryAfterDelay = false;
                        }

                        continue;
                    }

                    publishActiveParts();
                    stats = WriteStats{};
                    streamConfirmed = false;
                    successfulChunks = 0;
                    clearSendQueue(destination);
                    lastDiagTime = std::chrono::steady_clock::now();
                    diagSentBytes = 0;

                    std::wcout
                        << L"[dest " << (destination.index + 1)
                        << L"] HTTP connected; waiting for audio writes...\n";
                }

                size_t readyBytes = 0;
                {
                    std::lock_guard<std::mutex> lock(
                        destination.queue.mutex);
                    readyBytes = destination.queue.queuedBytes;
                }

                std::vector<BYTE> block;
                if (readyBytes >= destination.minSendBytes)
                    block = takeSendBlock(destination);

                if (!block.empty()) {
                    if (!WritePcm(
                            request,
                            block.data(),
                            static_cast<DWORD>(block.size()),
                            stats)) {
                        markDisconnected();
                    } else {
                        ++successfulChunks;

                        if (!streamConfirmed &&
                            successfulChunks >= 3) {
                            streamConfirmed = true;
                            hasStreamedSuccessfully = true;
                            autoReconnectAttempts = 0;
                            serviceRetryDelay = false;
                            retryAfterDelay = false;
                            std::wcout
                                << L"[dest "
                                << (destination.index + 1)
                                << L"] Connected. Audio data flowing.\n";
                        }

                        diagSentBytes += block.size();
                    }
                } else {
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(1));
                }

                if (connected) {
                    const auto nowDiag =
                        std::chrono::steady_clock::now();

                    if (nowDiag - lastDiagTime >=
                        std::chrono::seconds(5)) {
                        const double elapsedSec =
                            std::chrono::duration<double>(
                                nowDiag - lastDiagTime)
                                .count();

                        size_t queuedNow = 0;
                        {
                            std::lock_guard<std::mutex> lock(
                                destination.queue.mutex);
                            queuedNow =
                                destination.queue.queuedBytes;
                        }

                        const double sentMs =
                            diagSentBytes /
                            destination.sendBytesPerMs;
                        const double realtimeRatio =
                            elapsedSec > 0.0
                                ? sentMs /
                                      (elapsedSec * 1000.0)
                                : 0.0;

                        double headAgeMs = -1.0;
                        {
                            std::lock_guard<std::mutex> lock(
                                destination.queue.mutex);
                            if (!destination.queue.blocks.empty() &&
                                destination.queue.blocks.front()
                                    .hasTimestamp &&
                                qpcOffsetValid.load(
                                    std::memory_order_acquire)) {
                                LARGE_INTEGER nowQpc{};
                                if (QueryPerformanceCounter(&nowQpc)) {
                                    const double now100ns =
                                        static_cast<double>(
                                            nowQpc.QuadPart) *
                                            10000000.0 /
                                            static_cast<double>(
                                                qpcFrequency.QuadPart) +
                                        qpcOffset100ns.load(
                                            std::memory_order_relaxed);
                                    const PcmBlock& head =
                                        destination.queue.blocks.front();
                                    const double end100ns =
                                        static_cast<double>(
                                            head.qpcStart) +
                                        static_cast<double>(
                                            head.frames) *
                                            10000000.0 /
                                            static_cast<double>(
                                                format->nSamplesPerSec);
                                    headAgeMs =
                                        (now100ns - end100ns) /
                                        10000.0;
                                }
                            }
                        }

                        std::wcout
                            << L"[send "
                            << (destination.index + 1)
                            << L"] queued "
                            << static_cast<long long>(
                                   static_cast<double>(queuedNow) /
                                   destination.sendBytesPerMs)
                            << L" ms; age "
                            << static_cast<long long>(headAgeMs)
                            << L" ms; sent "
                            << static_cast<long long>(sentMs)
                            << L" ms in "
                            << static_cast<long long>(
                                   elapsedSec * 1000.0)
                            << L" ms ("
                            << static_cast<long long>(
                                   realtimeRatio * 100.0)
                            << L"% of realtime); write max "
                            << static_cast<long long>(
                                   stats.writeMaxMs)
                            << L" ms, >=1ms "
                            << stats.writeOver1MsCalls
                            << L"/"
                            << stats.writeCalls
                            << L"\n";

                        lastDiagTime = nowDiag;
                        diagSentBytes = 0;
                        stats.writeCalls = 0;
                        stats.writeOver1MsCalls = 0;
                        stats.writeMaxMs = 0.0;
                        stats.blockedMs = 0.0;
                    }
                }

                if (!connected && !g_stop.load() &&
                    !destination.stop.load()) {
                    DWORD httpStatus = 0;
                    std::string responseBody;
                    std::wstring location;
                    bool portFallbackApplied = false;

                    if (request) {
                        if (ReadHttpResponse(
                                request,
                                httpStatus,
                                responseBody,
                                location)) {
                            PrintHttpResponse(
                                httpStatus,
                                responseBody,
                                location);

                            if (httpStatus >= 300 &&
                                httpStatus < 400) {
                                UrlParts redirected;
                                bool redirectedOk =
                                    !location.empty() &&
                                    CrackUrl(
                                        location,
                                        redirected);

                                if (redirectedOk) {
                                    SetQueryParameter(
                                        redirected.path,
                                        L"rate",
                                        std::to_wstring(
                                            sampleRate));
                                    SetQueryParameter(
                                        redirected.path,
                                        L"channels",
                                        std::to_wstring(
                                            outChannels));
                                    activeParts = redirected;
                                    publishActiveParts();

                                    std::wcout
                                        << L"[dest "
                                        << (destination.index + 1)
                                        << L"] Following HTTP redirect to "
                                        << (activeParts.https
                                                ? L"https://"
                                                : L"http://")
                                        << activeParts.host
                                        << L":"
                                        << activeParts.port
                                        << activeParts.path
                                        << L"\n";
                                } else if (
                                    !hasStreamedSuccessfully &&
                                    AdvancePortFallback(
                                        activeParts)) {
                                    portFallbackApplied = true;
                                    publishActiveParts();
                                    std::wcout
                                        << L"[dest "
                                        << (destination.index + 1)
                                        << L"] HTTP redirect detected; trying port "
                                        << activeParts.port
                                        << L".\n";
                                }
                            }
                        }
                    }

                    if (!hasStreamedSuccessfully &&
                        !portFallbackApplied &&
                        IsConnectionFailure(
                            stats.lastWinHttpError) &&
                        AdvancePortFallback(activeParts)) {
                        publishActiveParts();
                        std::wcout
                            << L"[dest "
                            << (destination.index + 1)
                            << L"] WinHTTP connection error; trying port "
                            << activeParts.port
                            << L".\n";
                    }

                    closeHttp();
                    markDisconnected();
                    ++autoReconnectAttempts;

                    if (serviceMode && serviceRetryDelay) {
                        std::wcerr
                            << L"[dest "
                            << (destination.index + 1)
                            << L"] Connection interrupted; retrying in 10 seconds.\n";
                        retryAfterDelay = false;
                        shortReconnectDelay = false;
                    } else if (
                        autoReconnectAttempts <
                        kMaxAutoReconnectAttempts) {
                        std::wcerr
                            << L"[dest "
                            << (destination.index + 1)
                            << L"] Connection interrupted; retrying in 1 second (attempt "
                            << autoReconnectAttempts
                            << L"/"
                            << kMaxAutoReconnectAttempts
                            << L").\n";
                        retryAfterDelay = true;
                        shortReconnectDelay = false;
                    } else if (serviceMode) {
                        std::wcerr
                            << L"[dest "
                            << (destination.index + 1)
                            << L"] Reconnect failed 3 times; retrying automatically in 10 seconds.\n";
                        retryAfterDelay = false;
                        serviceRetryDelay = true;
                        shortReconnectDelay = false;
                    } else {
                        std::wcerr
                            << L"[dest "
                            << (destination.index + 1)
                            << L"] Reconnect failed 3 times; press Enter to retry.\n";
                        retryAfterDelay = false;
                        serviceRetryDelay = false;
                        shortReconnectDelay = false;
                        waitingForEnter = true;
                    }
                }
            }

            closeHttp();
        };

        // 先启动发送线程，再启动采集；避免刚启动时队列短暂无人消费。
        senderThreads.reserve(destinations.size());
        for (const auto& destination : destinations) {
            Destination* destinationPtr = destination.get();
            senderThreads.emplace_back(
                [&runSender, destinationPtr]() {
                    runSender(*destinationPtr);
                });
        }

        startCaptureThread();

        std::wcout
            << L"Press Enter to reconnect all, Ctrl+C to stop.\n";

        bool captureFailed = false;

        while (!g_stop.load()) {
            if (g_reconnectRequested.exchange(false)) {
                for (const auto& destination : destinations)
                    destination->reconnectRequested.store(true);
            }

            if (_kbhit()) {
                const int key = _getch();

                // Windows 控制台方向键：0/224 后跟具体键值，↑=72，↓=80。
                if (key == 0 || key == 224) {
                    const int arrow = _getch();

                    if (arrow == 72) {
                        ++g_volumeUpRequests;
                    } else if (arrow == 80) {
                        ++g_volumeDownRequests;
                    }
                } else if (key == '\r') {
                    std::wcout
                        << L"Reconnect requested for all destinations.\n";
                    for (const auto& destination : destinations)
                        destination->reconnectRequested.store(true);
                }
            }

            if (captureFault.load()) {
                std::wcerr
                    << L"Audio capture failed; exiting.\n";
                captureFailed = true;
                g_stop.store(true);
                break;
            }

            std::this_thread::sleep_for(
                std::chrono::milliseconds(20));
        }

        stopThreads();

        return !captureFailed;
    };

    ok = run();

    if (keyboardHookStarted)
        StopKeyboardHook();

    if (format)
        CoTaskMemFree(format);

    if (captureClient)
        captureClient->Release();

    if (audioClient) {
        audioClient->Stop();
        audioClient->Release();
    }

    if (device)
        device->Release();

    if (enumerator)
        enumerator->Release();

    if (audioCaptureEvent)
        CloseHandle(audioCaptureEvent);

    CoUninitialize();

    SetConsoleCtrlHandler(
        ConsoleHandler,
        FALSE);

    return ok ? 0 : 1;
}
