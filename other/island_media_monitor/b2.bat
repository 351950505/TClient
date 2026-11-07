@echo off
call "C:\VS2026\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b %errorlevel%
cd /d "D:\kehuduan\tools\island_media_monitor"
cl /nologo /O2 /EHsc /W0 /utf-8 /DUNICODE /D_UNICODE island_media_monitor.cpp /Fe:island_media_monitor.exe /link /SUBSYSTEM:WINDOWS /INCREMENTAL:NO shfolder.lib > build2.log 2>&1
echo EXIT=%errorlevel%
