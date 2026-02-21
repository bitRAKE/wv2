@echo off
setlocal
call _env.cmd

clang @minimal.response

clang @advanced.response

rc.exe /nologo game.rc
clang @game.response

endlocal
