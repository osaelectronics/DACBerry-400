#!/bin/bash
# Instala la estación de prueba DACBerry 400 para el usuario actual:
#   ~/dacberry-test/dacberry-test.sh  + icono en el escritorio y en el menú.
# Ejecutar como el usuario normal (no root):  ./install.sh
set -e

[ "$(id -u)" -ne 0 ] || { echo "Ejecútalo como tu usuario, no con sudo." >&2; exit 1; }

SRC="$(cd "$(dirname "$0")" && pwd)"
DEST="$HOME/dacberry-test"

echo "==> Dependencias"
sudo apt-get install -y i2c-tools alsa-utils psmisc ffmpeg

echo "==> Copiando el script a $DEST"
mkdir -p "$DEST"
install -m 755 "$SRC/dacberry-test.sh" "$DEST/dacberry-test.sh"

DESKTOP_FILE="[Desktop Entry]
Type=Application
Name=DACBerry 400 Test
Comment=Prueba en serie de placas DACBerry 400 sin reiniciar
Exec=$DEST/dacberry-test.sh --pause
Icon=audio-card
Terminal=true
Categories=AudioVideo;Utility;"

mkdir -p "$HOME/.local/share/applications"
printf '%s\n' "$DESKTOP_FILE" > "$HOME/.local/share/applications/dacberry-test.desktop"

DESK=$(xdg-user-dir DESKTOP 2>/dev/null || echo "$HOME/Desktop")
if [ -d "$DESK" ]; then
	printf '%s\n' "$DESKTOP_FILE" > "$DESK/dacberry-test.desktop"
	chmod +x "$DESK/dacberry-test.desktop"
	gio set "$DESK/dacberry-test.desktop" metadata::trusted true 2>/dev/null || true
	echo "==> Icono creado en $DESK"
fi

if [ ! -f /sys/module/snd_soc_dacberry400/parameters/route_ramp ]; then
	echo
	echo "AVISO: el driver dacberry400 v3.1.x no está cargado."
	echo "       Instálalo antes con install-dkms.sh (en la raíz del repo) y reinicia una vez."
fi

echo
echo "Listo. Lánzalo desde el icono 'DACBerry 400 Test' o con:"
echo "    $DEST/dacberry-test.sh"
echo "Desde Windows:  ssh -t $(id -un)@$(hostname -I | awk '{print $1}') ~/dacberry-test/dacberry-test.sh"
