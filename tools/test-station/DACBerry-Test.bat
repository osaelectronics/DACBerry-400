@echo off
rem Estacion de prueba DACBerry 400 desde Windows (OpenSSH viene con Windows 10/11).
rem Cambia la IP/usuario si tu Pi tiene otros.
set PI=osa@192.168.50.127

ssh -t %PI% "~/dacberry-test/dacberry-test.sh"
pause
