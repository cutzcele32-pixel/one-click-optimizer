@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Insiders\VC\Auxiliary\Build\vcvars64.bat"
cl /nologo /std:c++17 /W4 /permissive- /EHsc /O2 ^
   /DUNICODE /D_UNICODE /DOPT_ONLY_BUILD ^
   pcopt.cpp ^
   /link /OUT:"optimizer test.exe" /SUBSYSTEM:WINDOWS /MACHINE:X64 ^
        /MANIFESTUAC:level='requireAdministrator' ^
   winhttp.lib iphlpapi.lib psapi.lib shell32.lib advapi32.lib user32.lib gdi32.lib winmm.lib
