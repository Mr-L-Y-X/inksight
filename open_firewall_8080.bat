@echo off
chcp 65001 >nul
setlocal

net session >nul 2>&1
if %errorLevel% neq 0 (
    echo.
    echo   [!] Please run as Administrator
    echo       Right-click this file - "Run as administrator"
    echo.
    pause
    exit /b 1
)

echo Adding inbound rule for TCP 8080 ...
netsh advfirewall firewall delete rule name="InkSight Backend 8080" >nul 2>&1
netsh advfirewall firewall add rule name="InkSight Backend 8080" dir=in action=allow protocol=TCP localport=8080 profile=any

echo.
echo   Done. Device should now reach http://192.168.137.1:8080
echo.
pause
