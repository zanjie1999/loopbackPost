#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <winhttp.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <thread>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(lib, "winhttp.lib")

static std::atomic_bool g_stop{false};

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

    // 给 WinHttpCrackUrl 提供实际的输出缓冲区。
    wchar_t host[512] = {};
    wchar_t path[4096] = {};
    wchar_t extra[4096] = {};

    uc.lpszHostName = host;
    uc.dwHostNameLength = _countof(host);

    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = _countof(path);

    uc.lpszExtraInfo = extra;
    uc.dwExtraInfoLength = _countof(extra);

    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) {
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

    // 没有路径时使用 /
    if (uc.dwUrlPathLength > 0)
        out.path.assign(path, uc.dwUrlPathLength);
    else
        out.path = L"/";

    // ?query / #fragment
    if (uc.dwExtraInfoLength > 0)
        out.path.append(extra, uc.dwExtraInfoLength);

    return true;
}

static void SetQueryParameter(std::wstring& path, const std::wstring& name, const std::wstring& value)
{
    const size_t q = path.find(L'?');
    if (q == std::wstring::npos) {
        path += L"?" + name + L"=" + value;
        return;
    }

    const size_t queryBegin = q + 1;
    const size_t fragment = path.find(L'#', queryBegin);
    const size_t queryEnd = (fragment == std::wstring::npos) ? path.size() : fragment;

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
        const std::wstring key = (eq == std::wstring::npos) ? item : item.substr(0, eq);

        if (key == name) {
            if (found) {
                // Drop duplicate occurrences; keep one canonical value.
            } else {
                if (!out.empty()) out += L'&';
                out += name + L"=" + value;
                found = true;
            }
        } else if (!item.empty()) {
            if (!out.empty()) out += L'&';
            out += item;
        }

        if (amp == query.size())
            break;
        pos = amp + 1;
    }

    if (!found) {
        if (!out.empty()) out += L'&';
        out += name + L"=" + value;
    }

    path = path.substr(0, queryBegin) + out +
           ((fragment == std::wstring::npos) ? L"" : path.substr(fragment));
}

static std::wstring BuildAudioUrl(const std::wstring& originalUrl,
                                  DWORD sampleRate,
                                  WORD channels)
{
    UrlParts parts;
    if (!CrackUrl(originalUrl, parts))
        return L"";

    SetQueryParameter(parts.path, L"rate", std::to_wstring(sampleRate));
    SetQueryParameter(parts.path, L"channels", std::to_wstring(channels));

    std::wstring scheme = parts.https ? L"https://" : L"http://";
    std::wstring result = scheme + parts.host;

    const bool defaultPort = (parts.https && parts.port == INTERNET_DEFAULT_HTTPS_PORT) ||
                             (!parts.https && parts.port == INTERNET_DEFAULT_HTTP_PORT);
    if (!defaultPort)
        result += L":" + std::to_wstring(parts.port);

    result += parts.path;
    return result;
}

static bool WriteAll(HINTERNET request, const BYTE* data, DWORD bytes)
{
    while (bytes > 0) {
        DWORD written = 0;
        if (!WinHttpWriteData(request, data, bytes, &written)) {
            PrintWinError(L"WinHttpWriteData");
            return false;
        }
        if (written == 0) {
            std::wcerr << L"WinHttpWriteData wrote 0 bytes.\n";
            return false;
        }
        data += written;
        bytes -= written;
    }
    return true;
}

int wmain(int argc, wchar_t* argv[])
{
    std::wstring url;
    if (argc >= 2) {
        url = argv[1];
    } else {
        std::wcout << L"workdayAlarmClockGo URL: ";
        std::getline(std::wcin, url);
    }

    if (url.empty()) {
        std::wcerr << L"No URL.\n";
        return 1;
    }

    UrlParts parts;
    if (!CrackUrl(url, parts))
        return 1;

    if (!SetConsoleCtrlHandler(ConsoleHandler, TRUE)) {
        PrintWinError(L"SetConsoleCtrlHandler");
        return 1;
    }

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
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
        // Open the default render endpoint first so we can determine the exact
        // WASAPI format and update rate/channels in the destination URL.
        hr = CoCreateInstance(
            __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
            __uuidof(IMMDeviceEnumerator),
            reinterpret_cast<void**>(&enumerator));
        if (FAILED(hr)) {
            PrintHr(L"CoCreateInstance(MMDeviceEnumerator)", hr);
            break;
        }

        // Capture the system mix going to the default multimedia render device.
        hr = enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &device);
        if (FAILED(hr)) {
            PrintHr(L"GetDefaultAudioEndpoint", hr);
            break;
        }

        hr = device->Activate(
            __uuidof(IAudioClient), CLSCTX_ALL, nullptr,
            reinterpret_cast<void**>(&audioClient));
        if (FAILED(hr)) {
            PrintHr(L"IMMDevice::Activate(IAudioClient)", hr);
            break;
        }

        // IMPORTANT: use the device's mix format exactly as returned by WASAPI.
        // No sample-rate conversion, channel conversion, or sample-format conversion.
        hr = audioClient->GetMixFormat(&format);
        if (FAILED(hr)) {
            PrintHr(L"IAudioClient::GetMixFormat", hr);
            break;
        }

        std::wcout << L"Capture format: "
                   << format->nSamplesPerSec << L" Hz, "
                   << format->nChannels << L" ch, "
                   << format->wBitsPerSample << L" bit, block="
                   << format->nBlockAlign << L" bytes";
        if (format->wFormatTag == WAVE_FORMAT_PCM)
            std::wcout << L", PCM";
        else if (format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT)
            std::wcout << L", IEEE float";
        else if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
            std::wcout << L", WAVE_FORMAT_EXTENSIBLE";
        std::wcout << L"\n";

        // Synchronize the server URL with the actual WASAPI format.
        const std::wstring finalUrl = BuildAudioUrl(
            url, format->nSamplesPerSec, format->nChannels);
        if (finalUrl.empty())
            break;

        UrlParts finalParts;
        if (!CrackUrl(finalUrl, finalParts))
            break;

        std::wcout << L"Streaming PCM to: " << finalUrl << L"\n";

        // Direct connection; no curl/FFmpeg and no external proxy dependency.
        session = WinHttpOpen(L"WASAPI-Loopback-PCM/1.0",
                              WINHTTP_ACCESS_TYPE_NO_PROXY,
                              WINHTTP_NO_PROXY_NAME,
                              WINHTTP_NO_PROXY_BYPASS,
                              0);
        if (!session) {
            PrintWinError(L"WinHttpOpen");
            break;
        }

        WinHttpSetTimeouts(session, 5000, 5000, 10000, 10000);

        connect = WinHttpConnect(session, finalParts.host.c_str(), finalParts.port, 0);
        if (!connect) {
            PrintWinError(L"WinHttpConnect");
            break;
        }

        request = WinHttpOpenRequest(
            connect,
            L"PUT",
            finalParts.path.c_str(),
            nullptr,
            WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES,
            finalParts.https ? WINHTTP_FLAG_SECURE : 0);

        if (!request) {
            PrintWinError(L"WinHttpOpenRequest");
            break;
        }

        // Unknown total length because the stream runs until Ctrl+C.
        const wchar_t headers[] =
            L"Content-Type: application/octet-stream\r\n"
            L"Transfer-Encoding: chunked\r\n"
            L"Expect:\r\n";

        if (!WinHttpSendRequest(
                request,
                headers,
                static_cast<DWORD>(-1),
                WINHTTP_NO_REQUEST_DATA,
                0,
                WINHTTP_IGNORE_REQUEST_TOTAL_LENGTH,
                0)) {
            PrintWinError(L"WinHttpSendRequest");
            break;
        }

        // 100 ms shared-mode buffer. Loopback is valid only in shared mode.
        constexpr REFERENCE_TIME bufferDuration = 1000000; // 100 ms
        hr = audioClient->Initialize(
            AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_LOOPBACK,
            bufferDuration,
            0,
            format,
            nullptr);
        if (FAILED(hr)) {
            PrintHr(L"IAudioClient::Initialize(loopback)", hr);
            break;
        }

        hr = audioClient->GetService(
            __uuidof(IAudioCaptureClient),
            reinterpret_cast<void**>(&captureClient));
        if (FAILED(hr)) {
            PrintHr(L"IAudioClient::GetService(IAudioCaptureClient)", hr);
            break;
        }

        hr = audioClient->Start();
        if (FAILED(hr)) {
            PrintHr(L"IAudioClient::Start", hr);
            break;
        }

        std::wcout << L"Press Ctrl+C to stop.\n";

        ok = true;

        while (!g_stop.load()) {
            UINT32 packetFrames = 0;
            hr = captureClient->GetNextPacketSize(&packetFrames);
            if (FAILED(hr)) {
                PrintHr(L"IAudioCaptureClient::GetNextPacketSize", hr);
                ok = false;
                break;
            }

            if (packetFrames == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }

            while (packetFrames > 0 && !g_stop.load()) {
                BYTE* data = nullptr;
                UINT32 frames = 0;
                DWORD flags = 0;

                hr = captureClient->GetBuffer(
                    &data, &frames, &flags, nullptr, nullptr);
                if (FAILED(hr)) {
                    PrintHr(L"IAudioCaptureClient::GetBuffer", hr);
                    ok = false;
                    break;
                }

                const DWORD bytes = frames * format->nBlockAlign;

                // Silent packets may return a null/unspecified data buffer.
                // Send zeroed PCM with the same size so timing remains continuous.
                if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                    static BYTE zeroBuffer[64 * 1024]{};
                    DWORD left = bytes;
                    while (left > 0 && !g_stop.load()) {
                        DWORD chunk = (left < sizeof(zeroBuffer)) ? left : sizeof(zeroBuffer);
                        if (!WriteAll(request, zeroBuffer, chunk)) {
                            ok = false;
                            break;
                        }
                        left -= chunk;
                    }
                } else if (bytes > 0) {
                    if (!WriteAll(request, data, bytes)) {
                        ok = false;
                    }
                }

                captureClient->ReleaseBuffer(frames);

                if (!ok || g_stop.load())
                    break;

                hr = captureClient->GetNextPacketSize(&packetFrames);
                if (FAILED(hr)) {
                    PrintHr(L"IAudioCaptureClient::GetNextPacketSize", hr);
                    ok = false;
                    break;
                }
            }
        }

        audioClient->Stop();

        // End the HTTP chunked request with a zero-length chunk.
        // WinHTTP documents this as the way to terminate a chunked upload.
        DWORD ignored = 0;
        if (!WinHttpWriteData(request, nullptr, 0, &ignored)) {
            // If the peer already closed the connection, this can fail; the stream is still done.
            if (ok)
                PrintWinError(L"WinHttpWriteData(end chunk)");
        }

        if (request) {
            if (WinHttpReceiveResponse(request, nullptr)) {
                DWORD status = 0;
                DWORD statusSize = sizeof(status);
                if (WinHttpQueryHeaders(
                        request,
                        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX,
                        &status,
                        &statusSize,
                        WINHTTP_NO_HEADER_INDEX)) {
                    std::wcout << L"HTTP status: " << status << L"\n";
                }
            }
        }

    } while (false);

    if (format) CoTaskMemFree(format);
    if (captureClient) captureClient->Release();
    if (audioClient) audioClient->Release();
    if (device) device->Release();
    if (enumerator) enumerator->Release();

    if (request) WinHttpCloseHandle(request);
    if (connect) WinHttpCloseHandle(connect);
    if (session) WinHttpCloseHandle(session);

    CoUninitialize();
    SetConsoleCtrlHandler(ConsoleHandler, FALSE);

    return ok ? 0 : 1;
}
