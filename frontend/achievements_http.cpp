// RetroAchievements' internet requests (achievements.cpp), each platform its own way:
//   Windows  WinHTTP, part of Windows (nothing more to ship)
//   Linux    the system's libcurl, opened when first needed (every desktop Linux and SteamOS has
//            it; without it achievements say so, and the game plays as ever)
//   others   to come (Android, the Mac, iOS, the Switch): achievements say they are not there yet
#include <string>

#if defined(_WIN32)
#include <windows.h>
#include <winhttp.h>
#elif defined(__linux__) && !defined(__ANDROID__)
#include <dlfcn.h>
#include <mutex>
#endif

namespace Achievements
{

#if defined(_WIN32)

const char* SystemName() { return "Windows"; }
bool Available() { return true; }

static std::wstring Wide(const std::string& s)
{
    if (s.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

bool HttpPost(const std::string& url, const std::string& post, const std::string& contentType,
              const std::string& userAgent, int& status, std::string& body, std::string& error)
{
    const std::wstring wurl = Wide(url);
    URL_COMPONENTS parts = {};
    parts.dwStructSize = sizeof parts;
    wchar_t host[256] = {}, path[4096] = {};
    parts.lpszHostName = host;
    parts.dwHostNameLength = 256;
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = 4096;
    wchar_t extra[4096] = {};
    parts.lpszExtraInfo = extra;
    parts.dwExtraInfoLength = 4096;
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &parts)) { error = "bad address"; return false; }
    const std::wstring target = std::wstring(path) + extra;
    const bool https = parts.nScheme == INTERNET_SCHEME_HTTPS;

    HINTERNET session = WinHttpOpen(Wide(userAgent).c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) // (Windows before 8.1: no automatic proxy)
        session = WinHttpOpen(Wide(userAgent).c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                              WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) { error = "no internet access (WinHttpOpen)"; return false; }
    WinHttpSetTimeouts(session, 10000, 10000, 30000, 30000);
    bool ok = false;
    HINTERNET connect = WinHttpConnect(session, host, parts.nPort, 0);
    HINTERNET request = connect ? WinHttpOpenRequest(connect, post.empty() ? L"GET" : L"POST", target.c_str(), nullptr,
                                                     WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                     https ? WINHTTP_FLAG_SECURE : 0)
                                : nullptr;
    if (request)
    {
        const std::wstring header = post.empty() ? std::wstring() : L"Content-Type: " + Wide(contentType) + L"\r\n";
        if (WinHttpSendRequest(request, header.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : header.c_str(), (DWORD)-1L,
                               post.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)post.data(), (DWORD)post.size(),
                               (DWORD)post.size(), 0) &&
            WinHttpReceiveResponse(request, nullptr))
        {
            DWORD code = 0, size = sizeof code;
            WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                                &code, &size, WINHTTP_NO_HEADER_INDEX);
            status = (int)code;
            body.clear();
            for (;;)
            {
                DWORD avail = 0;
                if (!WinHttpQueryDataAvailable(request, &avail) || avail == 0) break;
                const size_t at = body.size();
                body.resize(at + avail);
                DWORD got = 0;
                if (!WinHttpReadData(request, &body[at], avail, &got)) { body.resize(at); break; }
                body.resize(at + got);
            }
            ok = true;
        }
        else
            error = "no connection (WinHTTP error " + std::to_string(GetLastError()) + ")";
    }
    else
        error = "no connection (WinHTTP error " + std::to_string(GetLastError()) + ")";
    if (request) WinHttpCloseHandle(request);
    if (connect) WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return ok;
}

#elif defined(__linux__) && !defined(__ANDROID__)

const char* SystemName() { return "Linux"; }
bool Available() { return true; }

namespace
{
// The few parts of libcurl used, by their numbers in curl.h (stable since libcurl 7).
enum
{
    CURLOPT_WRITEDATA = 10001, CURLOPT_URL = 10002, CURLOPT_POSTFIELDS = 10015, CURLOPT_USERAGENT = 10018,
    CURLOPT_HTTPHEADER = 10023, CURLOPT_WRITEFUNCTION = 20011, CURLOPT_TIMEOUT = 13, CURLOPT_FOLLOWLOCATION = 52,
    CURLOPT_POSTFIELDSIZE = 60, CURLOPT_NOSIGNAL = 99, CURLOPT_CONNECTTIMEOUT = 78,
    CURLINFO_RESPONSE_CODE = 0x200002, CURL_GLOBAL_DEFAULT = 3,
};
struct Curl
{
    void* lib = nullptr;
    int (*global_init)(long) = nullptr;
    void* (*easy_init)() = nullptr;
    int (*easy_setopt)(void*, int, ...) = nullptr;
    int (*easy_perform)(void*) = nullptr;
    int (*easy_getinfo)(void*, int, ...) = nullptr;
    void (*easy_cleanup)(void*) = nullptr;
    const char* (*easy_strerror)(int) = nullptr;
    void* (*slist_append)(void*, const char*) = nullptr;
    void (*slist_free_all)(void*) = nullptr;
};
Curl g_curl;
bool g_curlTried = false;
std::mutex g_curlMutex;

bool LoadCurl()
{
    std::lock_guard<std::mutex> lock(g_curlMutex);
    if (g_curlTried) return g_curl.lib != nullptr;
    g_curlTried = true;
    for (const char* name : { "libcurl.so.4", "libcurl-gnutls.so.4", "libcurl.so" })
        if ((g_curl.lib = dlopen(name, RTLD_NOW | RTLD_LOCAL)) != nullptr) break;
    if (!g_curl.lib) return false;
    auto get = [](const char* n) { return dlsym(g_curl.lib, n); };
    g_curl.global_init = (int (*)(long))get("curl_global_init");
    g_curl.easy_init = (void* (*)())get("curl_easy_init");
    g_curl.easy_setopt = (int (*)(void*, int, ...))get("curl_easy_setopt");
    g_curl.easy_perform = (int (*)(void*))get("curl_easy_perform");
    g_curl.easy_getinfo = (int (*)(void*, int, ...))get("curl_easy_getinfo");
    g_curl.easy_cleanup = (void (*)(void*))get("curl_easy_cleanup");
    g_curl.easy_strerror = (const char* (*)(int))get("curl_easy_strerror");
    g_curl.slist_append = (void* (*)(void*, const char*))get("curl_slist_append");
    g_curl.slist_free_all = (void (*)(void*))get("curl_slist_free_all");
    if (!g_curl.global_init || !g_curl.easy_init || !g_curl.easy_setopt || !g_curl.easy_perform || !g_curl.easy_getinfo ||
        !g_curl.easy_cleanup || !g_curl.slist_append || !g_curl.slist_free_all)
    {
        dlclose(g_curl.lib);
        g_curl.lib = nullptr;
        return false;
    }
    g_curl.global_init(CURL_GLOBAL_DEFAULT);
    return true;
}

size_t Write(char* data, size_t size, size_t n, void* user)
{
    static_cast<std::string*>(user)->append(data, size * n);
    return size * n;
}
}

bool HttpPost(const std::string& url, const std::string& post, const std::string& contentType,
              const std::string& userAgent, int& status, std::string& body, std::string& error)
{
    if (!LoadCurl()) { error = "libcurl not found (install curl to use RetroAchievements)"; return false; }
    void* c = g_curl.easy_init();
    if (!c) { error = "curl_easy_init failed"; return false; }
    body.clear();
    void* headers = nullptr;
    g_curl.easy_setopt(c, CURLOPT_URL, url.c_str());
    g_curl.easy_setopt(c, CURLOPT_USERAGENT, userAgent.c_str());
    g_curl.easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    g_curl.easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    g_curl.easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
    g_curl.easy_setopt(c, CURLOPT_TIMEOUT, 30L);
    g_curl.easy_setopt(c, CURLOPT_WRITEFUNCTION, (size_t (*)(char*, size_t, size_t, void*))Write);
    g_curl.easy_setopt(c, CURLOPT_WRITEDATA, (void*)&body);
    if (!post.empty())
    {
        headers = g_curl.slist_append(nullptr, ("Content-Type: " + contentType).c_str());
        g_curl.easy_setopt(c, CURLOPT_HTTPHEADER, headers);
        g_curl.easy_setopt(c, CURLOPT_POSTFIELDS, post.c_str());
        g_curl.easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)post.size());
    }
    const int res = g_curl.easy_perform(c);
    long code = 0;
    if (res == 0) g_curl.easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    else error = g_curl.easy_strerror ? g_curl.easy_strerror(res) : "curl error " + std::to_string(res);
    if (headers) g_curl.slist_free_all(headers);
    g_curl.easy_cleanup(c);
    status = (int)code;
    return res == 0;
}

#else

const char* SystemName()
{
#if defined(__ANDROID__)
    return "Android";
#elif defined(__SWITCH__)
    return "Nintendo Switch";
#elif defined(__APPLE__)
    return "Apple";
#else
    return "Unknown";
#endif
}

bool Available() { return false; }

bool HttpPost(const std::string&, const std::string&, const std::string&, const std::string&, int&, std::string&,
              std::string& error)
{
    error = "not in this version yet";
    return false;
}

#endif

}
