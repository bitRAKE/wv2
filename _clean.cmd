@echo off

REM	Extra safe, only relative to this script:

IF EXIST "%~dp0minimal.exe.WebView2" (
    rmdir /S /Q "%~dp0minimal.exe.WebView2"
)
IF EXIST "%~dp0game.exe.WebView2" (
    rmdir /S /Q "%~dp0game.exe.WebView2"
)
IF EXIST "%~dp0advanced_userdata" (
    rmdir /S /Q "%~dp0advanced_userdata"
)
IF EXIST "%~dp0deep_stack\deep_stack.exe.WebView2" (
    rmdir /S /Q "%~dp0deep_stack\deep_stack.exe.WebView2"
)
