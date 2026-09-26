#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN

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
#include <iostream>
#include <limits>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(lib, "winhttp.lib")

static std::atomic_bool g_stop{false};

// 单次写入超过这个毫秒数，就认为它被写满的发送缓冲挡住了
static constexpr double kBlockedWriteMs = 5.0;

// 本次连接内累计“被挡住”的毫秒数
static double g_blockedMs = 0.0;

// 累计值超过这个阈值就触发重连
static constexpr double kReconnectBlockedMs = 100.0;

// 两次拥塞重连之间的冷却，避免网络持续抖动时疯狂重连
static constexpr auto kCongestionCooldown = std::chrono::seconds(3);

// 拥塞/手动重连：先停采集并清空捕获缓冲，等待服务端释放旧的 aplay 后再恢复采集。
static constexpr auto kCongestionReconnectDelay =
    std::chrono::milliseconds(150);

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

struct UrlParts {
    std::wstring host;
    std::wstring path;
    INTERNET_PORT port = 0;
    bool https = false;
};

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

    // url 是 const 引用，不能直接改；拷一份到局部变量，在副本上做补全
    std::wstring fullUrl = url;

    // 只输入ip和端口时补充前缀和后缀
    if (fullUrl.rfind(L"http", 0) != 0) {
        fullUrl = L"http://" + fullUrl + L"/aplay";
    }

    if (!WinHttpCrackUrl(fullUrl.c_str(), 0, 0, &uc)) {
        PrintWinError(L"WinHttpCrackUrl");
        return false;
    }

    out.https = (uc.nScheme == INTERNET_SCHEME_HTTPS);
    out.port = uc.nPort;

    if (uc.dwHostNameLength == 0) {
        std::wcerr << L"Invalid URL: missing host.\n";
        return false;
    }

    out.host.assign(host, uc.dwHostNameLength);

    if (uc.dwUrlPathLength > 0)
        out.path.assign(path, uc.dwUrlPathLength);
    else
        out.path = L"/";

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

    if (!defaultPort)
        result += L":" + std::to_wstring(parts.port);

    result += parts.path;

    return result;
}

static bool WriteRaw(HINTERNET request,
                     const BYTE* data,
                     DWORD bytes)
{
    while (bytes > 0) {
        DWORD written = 0;

        const auto writeStart = std::chrono::steady_clock::now();

        if (!WinHttpWriteData(
                request,
                data,
                bytes,
                &written)) {
            PrintWinError(L"WinHttpWriteData");
            return false;
        }

        const double writeMs =
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - writeStart).count();

        if (writeMs > kBlockedWriteMs)
            g_blockedMs += writeMs;

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

static bool WriteChunk(HINTERNET request,
                       const BYTE* data,
                       DWORD bytes)
{
    char header[32];

    const int headerLen =
        sprintf_s(
            header,
            sizeof(header),
            "%lX\r\n",
            static_cast<unsigned long>(bytes));

    if (headerLen <= 0)
        return false;

    // chunk size
    if (!WriteRaw(
            request,
            reinterpret_cast<const BYTE*>(header),
            static_cast<DWORD>(headerLen))) {
        return false;
    }

    // chunk data
    if (bytes > 0) {
        if (!WriteRaw(request, data, bytes))
            return false;
    }

    // trailing CRLF
    static const BYTE crlf[] = {'\r', '\n'};

    if (!WriteRaw(request, crlf, 2))
        return false;

    return true;
}

static bool EndChunkedRequest(HINTERNET request)
{
    static const BYTE end[] = {
        '0', '\r', '\n',
        '\r', '\n'
    };

    return WriteRaw(
        request,
        end,
        sizeof(end));
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
    std::wstring url;

    if (argc >= 2) {
        url = argv[1];
    } else {
        std::wcout
            << L"workdayAlarmClockGo URL end with /aplay: ";

        std::getline(std::wcin, url);
    }

    if (url.empty()) {
        std::wcerr << L"No URL.\n";
        return 1;
    }

    UrlParts parts;

    if (!CrackUrl(url, parts))
        return 1;

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

    HINTERNET session = nullptr;
    HINTERNET connect = nullptr;
    HINTERNET request = nullptr;

    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* device = nullptr;
    IAudioClient* audioClient = nullptr;
    IAudioCaptureClient* captureClient = nullptr;
    WAVEFORMATEX* format = nullptr;

    bool ok = false;

    do {
        hr = CoCreateInstance(
            __uuidof(MMDeviceEnumerator),
            nullptr,
            CLSCTX_ALL,
            __uuidof(IMMDeviceEnumerator),
            reinterpret_cast<void**>(&enumerator));

        if (FAILED(hr)) {
            PrintHr(
                L"CoCreateInstance(MMDeviceEnumerator)",
                hr);
            break;
        }

        hr = enumerator->GetDefaultAudioEndpoint(
            eRender,
            eMultimedia,
            &device);

        if (FAILED(hr)) {
            PrintHr(
                L"GetDefaultAudioEndpoint",
                hr);
            break;
        }

        hr = device->Activate(
            __uuidof(IAudioClient),
            CLSCTX_ALL,
            nullptr,
            reinterpret_cast<void**>(&audioClient));

        if (FAILED(hr)) {
            PrintHr(
                L"IMMDevice::Activate(IAudioClient)",
                hr);
            break;
        }

        hr = audioClient->GetMixFormat(&format);

        if (FAILED(hr)) {
            PrintHr(
                L"IAudioClient::GetMixFormat",
                hr);
            break;
        }

        // 保存当前默认输出设备的 ID。每次重连都会重新查询默认输出，
        // 如果设备发生变化，就重新建立 loopback capture。
        std::wstring deviceId;
        {
            LPWSTR rawId = nullptr;
            hr = device->GetId(&rawId);
            if (FAILED(hr)) {
                PrintHr(L"IMMDevice::GetId", hr);
                break;
            }
            if (rawId) {
                deviceId = rawId;
                CoTaskMemFree(rawId);
            }
        }

        SampleType sampleType = GetSampleType(format);

        if (sampleType == SampleType::UNSUPPORTED) {
            std::wcerr
                << L"Unsupported WASAPI sample format.\n";
            break;
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

        std::wstring finalUrl =
            BuildAudioUrl(
                url,
                format->nSamplesPerSec,
                format->nChannels);

        if (finalUrl.empty())
            break;

        UrlParts finalParts;

        if (!CrackUrl(finalUrl, finalParts))
            break;

        std::wcout
            << L"Output format: "
            << format->nSamplesPerSec
            << L" Hz, "
            << format->nChannels
            << L" ch, s16le\n";

        std::wcout
            << L"Streaming PCM to: "
            << finalUrl
            << L"\n";

        const auto closeHttp = [&]() {
            if (request) { WinHttpCloseHandle(request); request = nullptr; }
            if (connect) { WinHttpCloseHandle(connect); connect = nullptr; }
            if (session) { WinHttpCloseHandle(session); session = nullptr; }
        };

        const auto openHttp = [&]() -> bool {
            closeHttp();
            session = WinHttpOpen(
                L"workdayAlarmClockGo/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY,
                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
            if (!session) { PrintWinError(L"WinHttpOpen"); return false; }
            // 设置超时：Resolve、Connect、Send、Receive
            WinHttpSetTimeouts(session, 5000, 5000, 1000, 5000);
            connect = WinHttpConnect(session, finalParts.host.c_str(), finalParts.port, 0);
            if (!connect) { PrintWinError(L"WinHttpConnect"); closeHttp(); return false; }
            request = WinHttpOpenRequest(
                connect, L"PUT", finalParts.path.c_str(), nullptr,
                WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                finalParts.https ? WINHTTP_FLAG_SECURE : 0);
            if (!request) { PrintWinError(L"WinHttpOpenRequest"); closeHttp(); return false; }
            const wchar_t headers[] =
                L"Content-Type: application/octet-stream\r\n"
                L"Transfer-Encoding: chunked\r\n"
                L"Expect: 100-continue\r\n";
            if (!WinHttpSendRequest(
                    request, headers, static_cast<DWORD>(-1),
                    WINHTTP_NO_REQUEST_DATA, 0,
                    WINHTTP_IGNORE_REQUEST_TOTAL_LENGTH, 0)) {
                PrintWinError(L"WinHttpSendRequest");
                closeHttp();
                return false;
            }
            return true;
        };

        // 重连时只 Stop + Reset，不要在这里 Start。
        // 这样在等待网络/服务端恢复的这段时间里，WASAPI 不会继续产生
        // 捕获数据，避免 150 ms（或普通重连等待期间）产生新的积压。
        const auto stopAndFlushAudioCapture = [&]() -> bool {
            if (!audioClient)
                return false;

            hr = audioClient->Stop();
            if (FAILED(hr)) {
                PrintHr(L"IAudioClient::Stop(reconnect)", hr);
                return false;
            }

            hr = audioClient->Reset();
            if (FAILED(hr)) {
                PrintHr(L"IAudioClient::Reset(reconnect)", hr);
                return false;
            }

            return true;
        };

        const auto startAudioCapture = [&]() -> bool {
            if (!audioClient)
                return false;

            hr = audioClient->Start();
            if (FAILED(hr)) {
                PrintHr(L"IAudioClient::Start(reconnect)", hr);
                return false;
            }

            return true;
        };

        // 每次真正重连前重新读取默认输出设备。
        // 当前设备没变：保持已经 Stop+Reset 的状态，不再次启动。
        // 设备变了：完整释放旧 endpoint，并建立新的 loopback，但仍不 Start，
        // 等 HTTP 连接成功后再统一启动，这样重连等待期间绝不会积累音频。
        const auto refreshAudioDevice = [&]() -> bool {
            IMMDevice* currentDevice = nullptr;

            hr = enumerator->GetDefaultAudioEndpoint(
                eRender,
                eMultimedia,
                &currentDevice);

            if (FAILED(hr)) {
                PrintHr(L"GetDefaultAudioEndpoint(reconnect)", hr);
                return false;
            }

            LPWSTR rawId = nullptr;
            hr = currentDevice->GetId(&rawId);
            if (FAILED(hr)) {
                PrintHr(L"IMMDevice::GetId(reconnect)", hr);
                currentDevice->Release();
                return false;
            }

            std::wstring currentId = rawId ? rawId : L"";
            if (rawId)
                CoTaskMemFree(rawId);

            if (currentId == deviceId) {
                currentDevice->Release();
                return true;
            }

            std::wcout
                << L"Default audio output changed; switching capture device.\n";

            if (audioClient) {
                // 正常情况下这里应该已经 Stop；失败也不影响后面释放重建。
                audioClient->Stop();
            }

            if (captureClient) {
                captureClient->Release();
                captureClient = nullptr;
            }

            if (audioClient) {
                audioClient->Release();
                audioClient = nullptr;
            }

            if (device) {
                device->Release();
                device = nullptr;
            }

            if (format) {
                CoTaskMemFree(format);
                format = nullptr;
            }

            device = currentDevice;
            currentDevice = nullptr;
            deviceId = currentId;

            hr = device->Activate(
                __uuidof(IAudioClient),
                CLSCTX_ALL,
                nullptr,
                reinterpret_cast<void**>(&audioClient));

            if (FAILED(hr)) {
                PrintHr(L"IMMDevice::Activate(IAudioClient,reconnect)", hr);
                return false;
            }

            hr = audioClient->GetMixFormat(&format);
            if (FAILED(hr)) {
                PrintHr(L"IAudioClient::GetMixFormat(reconnect)", hr);
                return false;
            }

            sampleType = GetSampleType(format);
            if (sampleType == SampleType::UNSUPPORTED) {
                std::wcerr
                    << L"Unsupported WASAPI sample format after device switch.\n";
                return false;
            }

            finalUrl = BuildAudioUrl(
                url,
                format->nSamplesPerSec,
                format->nChannels);

            if (finalUrl.empty() || !CrackUrl(finalUrl, finalParts))
                return false;

            REFERENCE_TIME defaultPeriod = 0;
            REFERENCE_TIME minPeriod = 0;

            hr = audioClient->GetDevicePeriod(
                &defaultPeriod,
                &minPeriod);

            if (FAILED(hr) || defaultPeriod <= 0)
                defaultPeriod = 100000;

            constexpr REFERENCE_TIME targetDuration = 300000;

            const REFERENCE_TIME bufferDuration =
                ((targetDuration + defaultPeriod - 1) / defaultPeriod) *
                defaultPeriod;

            hr = audioClient->Initialize(
                AUDCLNT_SHAREMODE_SHARED,
                AUDCLNT_STREAMFLAGS_LOOPBACK,
                bufferDuration,
                0,
                format,
                nullptr);

            if (FAILED(hr)) {
                PrintHr(L"IAudioClient::Initialize(loopback,reconnect)", hr);
                return false;
            }

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
                PrintHr(
                    L"IAudioClient::GetService(IAudioCaptureClient,reconnect)",
                    hr);
                return false;
            }

            // 注意：这里故意不 Start。等 HTTP 真正连接成功后，由主循环统一 Start。
            return true;
        };

        // 共享模式下缓冲最终由音频引擎决定，我们只能“申请目标值”。
        // 先问一下引擎周期，按它的整数倍去申请，免得因为没对齐被拒绝。
        REFERENCE_TIME defaultPeriod = 0;
        REFERENCE_TIME minPeriod = 0;

        hr = audioClient->GetDevicePeriod(&defaultPeriod, &minPeriod);

        if (FAILED(hr) || defaultPeriod <= 0)
            defaultPeriod = 100000; // 问不到就按 10 ms 兜底

        constexpr REFERENCE_TIME targetDuration = 300000; // 想要 30 ms

        const REFERENCE_TIME bufferDuration =
            ((targetDuration + defaultPeriod - 1) / defaultPeriod) *
            defaultPeriod;

        std::wcout
            << L"Engine period: "
            << defaultPeriod / 10000
            << L" ms, requesting capture buffer: "
            << bufferDuration / 10000
            << L" ms\n";

        hr = audioClient->Initialize(
            AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_LOOPBACK,
            bufferDuration,
            0,
            format,
            nullptr);

        if (FAILED(hr)) {
            PrintHr(
                L"IAudioClient::Initialize(loopback)",
                hr);
            break;
        }

        // 共享模式下申请值不一定被采纳，把真实缓冲大小打出来，
        // 这样才能知道这一项到底省下了多少延迟
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
            PrintHr(
                L"IAudioClient::GetService(IAudioCaptureClient)",
                hr);
            break;
        }

        // 不在这里启动采集。
        // 首次 HTTP 连接成功后，由主循环统一 Start；这样启动阶段也不会
        // 在网络还没建立时产生无法发送的 WASAPI 积压。
        std::wcout
            << L"Press Enter to reconnect, Ctrl+C to stop.\n";

        ok = true;

        std::vector<int16_t> pcm16;

        bool connected = false;
        bool retryAfterDelay = false; // 普通断线：等 1 秒再重连
        bool shortReconnectDelay = false; // 手动/拥塞：等待 150 ms
        bool congested = false;       // 用于输出拥塞重连日志

        auto lastCongestion =
            std::chrono::steady_clock::now() - kCongestionCooldown;

        while (!g_stop.load()) {
            if (!connected) {
                if (retryAfterDelay) {
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                } else if (shortReconnectDelay) {
                    // 150 ms 等待期间采集已经 Stop+Reset，因此不会产生新的音频积压。
                    std::this_thread::sleep_for(kCongestionReconnectDelay);
                }

                if (g_stop.load())
                    break;

                if (!refreshAudioDevice()) {
                    std::wcerr
                        << L"Audio capture reinitialization failed. Retrying in 1 second.\n";
                    retryAfterDelay = true;
                    shortReconnectDelay = false;
                    congested = false;
                    continue;
                }

                if (congested)
                    std::wcout << L"Reconnecting to resync...\n";
                std::wcout << L"Connecting...\n";
                connected = openHttp();

                retryAfterDelay = false;
                shortReconnectDelay = false;

                if (!connected) {
                    std::wcerr << L"Reconnect failed; audio capture remains stopped. Press Enter to retry.\n";
                    retryAfterDelay = true;
                    congested = false;
                    g_blockedMs = 0.0;
                    continue;
                }

                // HTTP 连接成功后才恢复采集。此前整个等待/重连过程都不会积累 WASAPI 数据。
                if (!startAudioCapture()) {
                    closeHttp();
                    connected = false;
                    retryAfterDelay = true;
                    congested = false;
                    g_blockedMs = 0.0;
                    continue;
                }

                g_blockedMs = 0.0;
                congested = false;
                std::wcout << L"Connected.\n";
            }

            if (_kbhit() && _getch() == '\r') {
                std::wcout << L"Reconnect requested; flushing capture and refreshing audio output device.\n";
                closeHttp();
                connected = false;
                retryAfterDelay = false;
                shortReconnectDelay = true;
                congested = false;

                // 立刻 Stop+Reset 清掉当前捕获积压，然后等 150 ms。
                // 这 150 ms 内采集保持停止，绝不会继续往 WASAPI buffer 里堆数据。
                if (!stopAndFlushAudioCapture()) {
                    std::wcerr
                        << L"Failed to flush audio capture; will rebuild it during reconnect.\n";
                }
                continue;
            }

            UINT32 packetFrames = 0;

            hr = captureClient->GetNextPacketSize(
                &packetFrames);

            if (FAILED(hr)) {
                PrintHr(
                    L"IAudioCaptureClient::GetNextPacketSize",
                    hr);
                connected = false;
                break;
            }

            if (packetFrames == 0) {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(5));
                continue;
            }

            while (
                packetFrames > 0 &&
                !g_stop.load()) {

                BYTE* data = nullptr;
                UINT32 frames = 0;
                DWORD flags = 0;

                hr = captureClient->GetBuffer(
                    &data,
                    &frames,
                    &flags,
                    nullptr,
                    nullptr);

                if (FAILED(hr)) {
                    PrintHr(
                        L"IAudioCaptureClient::GetBuffer",
                        hr);
                    connected = false;
                    break;
                }

                const size_t sampleCount =
                    static_cast<size_t>(frames) *
                    format->nChannels;

                const DWORD outputBytes =
                    static_cast<DWORD>(
                        sampleCount *
                        sizeof(int16_t));

                if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                    static BYTE zeroBuffer[64 * 1024]{};

                    DWORD left = outputBytes;

                    while (
                        left > 0 &&
                        !g_stop.load()) {

                        DWORD chunk =
                            std::min<DWORD>(
                                left,
                                static_cast<DWORD>(
                                    sizeof(zeroBuffer)));

                        if (!WriteChunk(
                                request,
                                zeroBuffer,
                                chunk)) {
                            connected = false;
                            break;
                        }

                        left -= chunk;
                    }
                } else if (
                    data != nullptr &&
                    frames > 0) {

                    if (!ConvertToS16(
                            data,
                            frames,
                            format,
                            sampleType,
                            pcm16)) {
                        std::wcerr
                            << L"PCM conversion failed.\n";
                        connected = false;
                    } else {
                        if (!WriteChunk(
                                request,
                                reinterpret_cast<
                                    const BYTE*>(
                                    pcm16.data()),
                                outputBytes)) {
                            connected = false;
                        }
                    }
                }

                captureClient->ReleaseBuffer(frames);

                if (!connected || g_stop.load())
                    break;

                // 拥塞判定：本次连接累计“被挡住的写入时间”超过阈值，
                // 说明下游已经积压了至少一个发送缓冲的数据。
                // 断开重连会触发服务端 kill aplay，把整条链路的积压清空。
                if (g_blockedMs >= kReconnectBlockedMs) {
                    const auto now =
                        std::chrono::steady_clock::now();

                    if (now - lastCongestion >= kCongestionCooldown) {
                        lastCongestion = now;

                        std::wcout
                            << L"Network congested (writes blocked "
                            << static_cast<long long>(g_blockedMs)
                            << L" ms); dropping the connection to flush the backlog.\n";

                        closeHttp();
                        connected = false;
                        congested = true;
                        shortReconnectDelay = true;

                        // 立刻停止并清空 WASAPI 捕获缓冲；150 ms 等待期间保持停止。
                        if (!stopAndFlushAudioCapture()) {
                            std::wcerr
                                << L"Failed to flush audio capture after congestion; will rebuild it during reconnect.\n";
                        }
                        break;
                    }

                    // 还在冷却期，先不重连；清零重新观察，
                    // 免得冷却一结束就立刻又触发
                    g_blockedMs = 0.0;
                }

                hr = captureClient->GetNextPacketSize(
                    &packetFrames);

                if (FAILED(hr)) {
                    PrintHr(
                        L"IAudioCaptureClient::GetNextPacketSize",
                        hr);
                    connected = false;
                    break;
                }
            }

            if (!connected && !g_stop.load()) {
                closeHttp();

                if (congested) {
                    std::wcerr
                        << L"Congestion reconnect; capture stopped, flushing buffers, waiting 150 ms.\n";
                    retryAfterDelay = false;
                    shortReconnectDelay = true;
                } else {
                    std::wcerr
                        << L"Connection interrupted; capture stopped, retrying in 1 second.\n";
                    retryAfterDelay = true;
                    shortReconnectDelay = false;

                    // 普通网络错误也不能让 WASAPI 在 1 秒重连等待期间继续积压。
                    stopAndFlushAudioCapture();
                }
            }
        }

        audioClient->Stop();

        // 结束 chunked request
        if (connected && request) {
            EndChunkedRequest(request);
            if (WinHttpReceiveResponse(
                    request,
                    nullptr)) {

                DWORD status = 0;
                DWORD statusSize =
                    sizeof(status);

                if (WinHttpQueryHeaders(
                        request,
                        WINHTTP_QUERY_STATUS_CODE |
                            WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX,
                        &status,
                        &statusSize,
                        WINHTTP_NO_HEADER_INDEX)) {

                    std::wcout
                        << L"HTTP status: "
                        << status
                        << L"\n";
                }
            }
        }

        closeHttp();

    } while (false);

    if (format)
        CoTaskMemFree(format);

    if (captureClient)
        captureClient->Release();

    if (audioClient)
        audioClient->Release();

    if (device)
        device->Release();

    if (enumerator)
        enumerator->Release();

    if (request)
        WinHttpCloseHandle(request);

    if (connect)
        WinHttpCloseHandle(connect);

    if (session)
        WinHttpCloseHandle(session);

    CoUninitialize();

    SetConsoleCtrlHandler(
        ConsoleHandler,
        FALSE);

    return ok ? 0 : 1;
}
