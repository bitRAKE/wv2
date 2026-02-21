
REM	call this from other scripts to meet environment expectations
REM	update with installed WebView2 SDK version

set "WebView2SDK=%USERPROFILE%\.nuget\packages\microsoft.web.webview2\1.0.864.35\build\native"
set "INCLUDE=%INCLUDE%;%WebView2SDK%\include"
set "LIB=%LIB%;%WebView2SDK%\x64"
