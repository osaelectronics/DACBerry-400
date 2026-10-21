#!/bin/bash
# DACBerry 400 driver (incremental fixes), DKMS install of the module only.
# Uses the stock overlay: dtoverlay=dacberry400
#
#   sudo ./install-dkms.sh            install / update
#   sudo ./install-dkms.sh --remove   back to the in-tree driver
set -euo pipefail

PKG=dacberry400
VER=$(sed -n 's/^PACKAGE_VERSION="\(.*\)"/\1/p' "$(dirname "$0")/dkms.conf")
SRC_DIR="$(cd "$(dirname "$0")" && pwd)"

[ "$(id -u)" -eq 0 ] || { echo "Run as root (sudo $0)" >&2; exit 1; }

if [ -d /boot/firmware/overlays ]; then BOOT=/boot/firmware; else BOOT=/boot; fi
CONFIG=${BOOT}/config.txt

remove_all_versions() {
	local d v
	for d in /var/lib/dkms/${PKG}/*/; do
		[ -d "$d" ] || continue
		v=$(basename "$d"); [ "$v" = "source" ] && continue
		dkms remove -m ${PKG} -v "$v" --all 2>/dev/null || true
		rm -rf "/usr/src/${PKG}-${v}"
	done
}

stock_config() {
	cp "${CONFIG}" "${CONFIG}.bak-dacberry400"
	sed -i 's/^dtoverlay=dacberry400-ext.*/dtoverlay=dacberry400/' "${CONFIG}"
	grep -q '^dtoverlay=dacberry400$' "${CONFIG}" || echo 'dtoverlay=dacberry400' >> "${CONFIG}"
	sed -i 's/^#dtparam=i2s=on.*/dtparam=i2s=on/' "${CONFIG}"
	grep -q '^dtparam=i2s=on' "${CONFIG}" || echo 'dtparam=i2s=on' >> "${CONFIG}"
	rm -f ${BOOT}/overlays/dacberry400-ext.dtbo /etc/modprobe.d/dacberry400.conf
}

if [ "${1:-}" = "--remove" ]; then
	remove_all_versions; stock_config
	echo "Removed. Reboot to go back to the in-tree driver."; exit 0
fi

apt-get install -y dkms "linux-headers-$(uname -r)" 2>/dev/null ||
	apt-get install -y dkms raspberrypi-kernel-headers

remove_all_versions
mkdir -p /usr/src/${PKG}-${VER}
cp -r "${SRC_DIR}/src" "${SRC_DIR}/dkms.conf" /usr/src/${PKG}-${VER}/
rm -f /usr/src/${PKG}-${VER}/src/*.ko /usr/src/${PKG}-${VER}/src/*.o
dkms add -m ${PKG} -v ${VER}
dkms build -m ${PKG} -v ${VER}
dkms install -m ${PKG} -v ${VER} --force
stock_config
echo "Installed ${PKG} ${VER}. Reboot to load it."
