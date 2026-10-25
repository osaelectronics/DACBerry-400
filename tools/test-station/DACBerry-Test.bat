@echo off
setlocal
rem Estacion de prueba DACBerry 400 desde Windows (OpenSSH viene con Windows 10/11).
rem Cambia la IP/usuario si tu Pi tiene otros.
set PI=osa@192.168.50.127

rem Si junto a este .bat hay un 1.flac, se copia a la Pi como cancion de referencia.
if exist "%~dp01.flac" (
  echo Copiando 1.flac a la Pi...
  ssh %PI% "mkdir -p ~/dacberry-test"
  scp -p "%~dp01.flac" %PI%:dacberry-test/1.flac
)

ssh -t %PI% "~/dacberry-test/dacberry-test.sh"
pause
