@echo off

REM	Extra safe, only relative to this script:

IF EXIST "%~dp0advanced_userdata" (
    rmdir /S /Q "%~dp0advanced_userdata"
)

IF EXIST "%~dp0minimal.exe.WebView2" (
    rmdir /S /Q "%~dp0minimal.exe.WebView2"
)