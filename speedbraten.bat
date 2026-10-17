@ECHO OFF
REM Starts speedbraten from a Sauerbraten 2020 folder. Arguments are passed on to the client;
REM -q<dir> replaces the default home directory (My Games\Sauerbraten, shared with Sauerbraten).

cd /d "%~dp0"
setlocal EnableDelayedExpansion
set "home=-q$HOME\My Games\Sauerbraten"
for %%a in (%*) do (
    set "arg=%%~a"
    if /i "!arg:~0,2!"=="-q" set "home="
)

if defined home (
    start bin64_speedbraten\speedbraten_x86_64_client.exe "%home%" -glog.txt %*
) else (
    start bin64_speedbraten\speedbraten_x86_64_client.exe -glog.txt %*
)
