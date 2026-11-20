@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Insiders\VC\Auxiliary\Build\vcvars64.bat"
cl /nologo /std:c++17 /W4 /permissive- /EHsc /O2 ^
   /DUNICODE /D_UNICODE ^
   test_harness.cpp ^
   /link /OUT:"grabber test.exe" /SUBSYSTEM:CONSOLE /MACHINE:X64 ^
   winhttp.lib shell32.lib
