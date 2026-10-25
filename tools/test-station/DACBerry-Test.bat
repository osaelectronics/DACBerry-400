@echo off
setlocal
rem Estacion de prueba DACBerry 400 desde Windows (OpenSSH viene con Windows 10/11).
rem Cambia la IP/usuario si tu Pi tiene otros.
set PI=osa@192.168.50.127

rem Copia a la Pi lo que haya junto a este .bat:
rem   dacberry-test.sh  -> actualiza el script de prueba
rem   1.flac            -> cancion de referencia
set FILES=
if exist "%~dp0dacberry-test.sh" set FILES=%FILES% "%~dp0dacberry-test.sh"
if exist "%~dp01.flac" set FILES=%FILES% "%~dp01.flac"

if not "%FILES%"=="" (
  echo Copiando a la Pi:%FILES%
  ssh %PI% "mkdir -p ~/dacberry-test"
  scp -p %FILES% %PI%:dacberry-test/
  ssh %PI% "chmod +x ~/dacberry-test/dacberry-test.sh; sed -i 's/\r$//' ~/dacberry-test/dacberry-test.sh"
)

ssh -t %PI% "~/dacberry-test/dacberry-test.sh"
pause
