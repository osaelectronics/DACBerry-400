#!/bin/bash
# DACBerry 400 - estación de prueba en serie, sin reiniciar la Pi.
#
# Por cada placa: desvincula el driver (salida apagada de forma suave y codec
# en reset), esperas a que cambies la placa, vuelve a vincular el driver
# (el codec nuevo se inicializa desde cero), comprueba por I2C que la placa
# responde y que los registros son los que el driver ha programado, hace la
# prueba de canales L/R, reproduce tu canción de referencia comprobando el PLL
# y el apagado del DAC, y guarda el resultado en un CSV.
#
# Uso:   ~/dacberry-test/dacberry-test.sh          (escritorio o SSH: ssh -t ...)
# Config: ~/dacberry-test/dacberry-test.conf        (se crea en la primera ejecución)

set -u
# i2cget/i2cset están en /usr/sbin, que no está en el PATH de un usuario normal por SSH
export PATH="$PATH:/usr/local/sbin:/usr/sbin:/sbin"

DIR="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"
CONF="$DIR/dacberry-test.conf"

# ---------------------------------------------------------------- config ----
CARD=tlvaudioCODEC          # nombre de la tarjeta ALSA (overlay dacberry400)
I2C_BUS=1
I2C_ADDR=0x18
VOLUME=90                   # % del control PCM durante la prueba
SONG=""                     # vacío = 1.flac en la carpeta del script (~/dacberry-test)
SONG_START=0                # segundo de la canción donde empieza el fragmento
SONG_SECONDS=20             # duración del fragmento
LOG="$HOME/dacberry-test-log.csv"
# shellcheck source=/dev/null
[ -f "$CONF" ] && . "$CONF"

PAUSE_AT_END=0
[ "${1:-}" = "--pause" ] && PAUSE_AT_END=1

CODEC_DEV="${I2C_BUS}-$(printf '%04x' $((I2C_ADDR)))"
CODEC_DRV=/sys/bus/i2c/drivers/tlv320aic3x
CARD_DRV=/sys/bus/platform/drivers/snd-rpi-dacberry400
REGMAP=/sys/kernel/debug/regmap/$CODEC_DEV/registers
MODPARAM=/sys/module/snd_soc_dacberry400/parameters
REF="$DIR/ref.wav"

# Sesión de usuario también por SSH (para parar/arrancar PipeWire)
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
export DBUS_SESSION_BUS_ADDRESS="${DBUS_SESSION_BUS_ADDRESS:-unix:path=$XDG_RUNTIME_DIR/bus}"

# ---------------------------------------------------------------- salida ----
B=$'\e[1m'; G=$'\e[32m'; R=$'\e[31m'; Y=$'\e[33m'; C=$'\e[36m'; N=$'\e[0m'
ok()   { printf '  %s✔%s %s\n' "$G" "$N" "$*"; }
bad()  { printf '  %s✘%s %s\n' "$R" "$N" "$*"; }
warn() { printf '  %s!%s %s\n' "$Y" "$N" "$*"; }
info() { printf '  %s•%s %s\n' "$C" "$N" "$*"; }
die()  { bad "$*"; exit 1; }

ask() {   # ask "pregunta" -> 0 si sí
	local a
	read -r -p "  $1 [s/n] " a
	case "$a" in s|S|si|SI|sí|Sí|y|Y) return 0 ;; *) return 1 ;; esac
}

save_conf() {
	cat > "$CONF" <<EOF
# DACBerry 400 test station
CARD=$CARD
I2C_BUS=$I2C_BUS
I2C_ADDR=$I2C_ADDR
VOLUME=$VOLUME
SONG=$(printf '%q' "$SONG")
SONG_START=$SONG_START
SONG_SECONDS=$SONG_SECONDS
LOG=$(printf '%q' "$LOG")
EOF
}

# --------------------------------------------------------------- I2C/regs ---
rd() {    # lee un registro directamente del chip (no de la caché), en decimal
	local v
	v=$(sudo i2cget -f -y "$I2C_BUS" "$I2C_ADDR" "$1" 2>/dev/null) || return 1
	echo $((v))
}
cache() { # valor que el driver cree que tiene el registro
	local r v
	r=$(printf '%02x:' "$1")
	v=$(sudo awk -v r="$r" '$1==r {print $2}' "$REGMAP" 2>/dev/null)
	[ -n "$v" ] && echo $((16#$v))
}
hx() { printf '0x%02x' "$1"; }

# ------------------------------------------------------------- PipeWire -----
PW_UNITS="pipewire.socket pipewire-pulse.socket wireplumber.service pipewire.service pipewire-pulse.service filter-chain.service"
PW_WAS=""
pw_stop() {
	PW_WAS=$(systemctl --user is-active pipewire.service 2>/dev/null)
	local u
	for u in $PW_UNITS; do systemctl --user stop "$u" 2>/dev/null; done
}
pw_start() {
	[ "$PW_WAS" = active ] || return 0
	local u
	for u in pipewire.socket pipewire-pulse.socket pipewire.service \
		 pipewire-pulse.service wireplumber.service filter-chain.service; do
		systemctl --user start "$u" 2>/dev/null
	done
}

# --------------------------------------------------------------- drivers ----
find_card_dev() {
	local d c
	for d in /sys/bus/platform/devices/*; do
		c=$(tr '\0' ' ' < "$d/of_node/compatible" 2>/dev/null) || continue
		case "$c" in *dacberry400*) basename "$d"; return 0 ;; esac
	done
	return 1
}
card_bound()  { [ -e "/sys/bus/platform/devices/$CARD_DEV/driver" ]; }
codec_bound() { [ -e "/sys/bus/i2c/devices/$CODEC_DEV/driver" ]; }

card_pids() {
	local n
	n=$(readlink "/proc/asound/$CARD" 2>/dev/null) || return 0
	n=${n#card}
	sudo fuser /dev/snd/controlC"$n" /dev/snd/pcmC"$n"D* 2>/dev/null | tr -s ' '
}

unbind_all() {
	if card_bound; then
		local pids
		pids=$(card_pids)
		if [ -n "${pids// /}" ]; then
			warn "La tarjeta está abierta por: $(ps -o comm= -p "$(echo "$pids" | xargs | tr ' ' ',')" | tr '\n' ' ')"
			ask "¿Desvincular igualmente?" || return 1
		fi
		echo "$CARD_DEV" | sudo tee "$CARD_DRV/unbind" >/dev/null || return 1
	fi
	if codec_bound; then
		echo "$CODEC_DEV" | sudo tee "$CODEC_DRV/unbind" >/dev/null || return 1
	fi
	return 0
}

bind_all() {
	codec_bound || echo "$CODEC_DEV" | sudo tee "$CODEC_DRV/bind" >/dev/null 2>&1
	sleep 0.3
	card_bound || echo "$CARD_DEV" | sudo tee "$CARD_DRV/bind" >/dev/null 2>&1
	for _ in $(seq 50); do
		[ -e "/proc/asound/$CARD" ] && return 0
		sleep 0.1
	done
	return 1
}

mixer() { amixer -q -c "$CARD" sset "$@" 2>/dev/null; }
set_mixer() {
	mixer 'PCM' "${VOLUME}%"
	mixer 'HP DAC' 100%
	mixer 'HP' 0dB unmute
	mixer 'Left HPCOM Mux' 'constant VCM'
	mixer 'Right HPCOM Mux' 'external feedback'
}

# ----------------------------------------------------------- preparación ---
preflight() {
	local c missing="" pkgs=""
	for c in i2cget:i2c-tools aplay:alsa-utils speaker-test:alsa-utils amixer:alsa-utils \
		 fuser:psmisc ffmpeg:ffmpeg; do
		command -v "${c%%:*}" >/dev/null && continue
		missing="$missing ${c%%:*}"
		case " $pkgs " in *" ${c#*:} "*) ;; *) pkgs="$pkgs ${c#*:}" ;; esac
	done
	if [ -n "$missing" ]; then
		warn "Faltan herramientas:$missing"
		ask "¿Instalarlas ahora (apt install$pkgs)?" || die "No se puede continuar sin ellas."
		# shellcheck disable=SC2086
		sudo apt-get install -y $pkgs || die "Falló la instalación"
	fi

	[ -f "$MODPARAM/route_ramp" ] ||
		die "No está cargado el driver dacberry400 v3.1.x (fix/incremental). Instálalo con install-dkms.sh y reinicia una vez."

	local v
	v=$(dkms status dacberry400 2>/dev/null | sed -n 's#^dacberry400/\([0-9.]*\).*#\1#p' | head -n1)
	if [ -n "$v" ] && [ "$(printf '%s\n3.1.4\n' "$v" | sort -V | head -n1)" != "3.1.4" ]; then
		warn "Driver $v: con versiones < 3.1.4 la primera reproducción tras cambiar de placa puede dar pop."
	fi

	CARD_DEV=$(find_card_dev) || die "No encuentro el nodo del overlay dacberry400 (¿dtoverlay=dacberry400 en config.txt?)"
}

prepare_song() {
	if [ -z "$SONG" ] || [ ! -f "$SONG" ]; then
		[ -n "$SONG" ] && warn "No existe $SONG, uso $DIR/1.flac"
		SONG="$DIR/1.flac"
	fi
	if [ ! -f "$SONG" ]; then
		bad "No encuentro la canción de referencia: $SONG"
		info "Copia tu canción como ${B}1.flac${N} en $DIR"
		info "(desde Windows basta con dejar 1.flac junto a DACBerry-Test.bat)"
		exit 1
	fi
	[ -f "$CONF" ] || save_conf

	local stamp
	stamp="$SONG|$(stat -c %Y "$SONG")|$SONG_START|$SONG_SECONDS"
	if [ -f "$REF" ] && [ "$(cat "$DIR/ref.stamp" 2>/dev/null)" = "$stamp" ]; then
		return 0
	fi
	if command -v ffmpeg >/dev/null; then
		info "Preparando el fragmento de referencia (${SONG_SECONDS} s desde ${SONG_START} s)..."
		ffmpeg -v error -y -ss "$SONG_START" -t "$SONG_SECONDS" -i "$SONG" \
			-ac 2 -ar 48000 -c:a pcm_s16le "$REF" || die "ffmpeg no pudo convertir $SONG"
	else
		case "${SONG,,}" in
			*.wav) cp "$SONG" "$REF" ;;
			*) die "Para canciones que no son WAV hace falta ffmpeg (sudo apt install ffmpeg)" ;;
		esac
	fi
	echo "$stamp" > "$DIR/ref.stamp"
}

# ---------------------------------------------------------------- prueba ----
RES_I2C=""; RES_REGS=""; RES_PLL=""; RES_DAC=""; RES_KERNEL=""; RES_LR=""; RES_AUDIO=""

check_static() {   # placa recién vinculada, sin reproducir
	local r25 r26 reg chip cch bad_regs=""
	if ! r25=$(rd 0x25); then
		bad "La placa no responde por I2C en $I2C_ADDR (bus $I2C_BUS)"
		RES_I2C=FAIL; return 1
	fi
	ok "Codec responde por I2C"
	RES_I2C=OK

	r26=$(rd 0x26)
	for reg in 0x25 0x26 0x2a 0x2b 0x2c 0x2f 0x40; do
		chip=$(rd "$reg") || chip=-1
		cch=$(cache "$reg") || cch=-2
		[ "$chip" = "$cch" ] || bad_regs="$bad_regs $reg(chip $(hx "$chip")≠driver $(hx "$cch"))"
	done
	if [ -z "$bad_regs" ] && [ $((r25 & 0x30)) -eq $((0x10)) ] && [ $((r26 & 0x38)) -eq $((0x20)) ]; then
		ok "Registros programados (salida capless, pop-reduction, volúmenes)"
		RES_REGS=OK
	else
		[ -n "$bad_regs" ] && bad "El chip no tiene lo que programó el driver:$bad_regs"
		[ $((r25 & 0x30)) -eq $((0x10)) ] || bad "HPLCOM no está en 'constant VCM' (reg 37 = $(hx "$r25"))"
		[ $((r26 & 0x38)) -eq $((0x20)) ] || bad "HPRCOM no está en 'external feedback' (reg 38 = $(hx "$r26"))"
		RES_REGS=FAIL
	fi
}

check_kernel() {
	local errs
	errs=$(sudo dmesg | tail -n +"$((KMARK + 1))" |
		grep -iE 'aic3x|dacberry|asoc|i2c|1-0018' | grep -iE 'error|fail|timed? ?out|stuck')
	if [ -z "$errs" ]; then
		ok "Sin errores del kernel"
		RES_KERNEL=OK
	else
		bad "Errores del kernel:"
		printf '      %s\n' "$errs"
		RES_KERNEL=FAIL
	fi
}

check_pll() {     # durante la reproducción
	local hp fmt rate ch w r3 r4 r5 r6 r11 r102 r94 p j d r bclk fs res
	hp=$(cat "/proc/asound/$CARD/pcm0p/sub0/hw_params" 2>/dev/null)
	fmt=$(awk '/^format:/ {print $2}' <<<"$hp")
	rate=$(awk '/^rate:/ {print $2}' <<<"$hp")
	ch=$(awk '/^channels:/ {print $2}' <<<"$hp")
	case "$fmt" in S16_LE) w=16 ;; S24_LE|S24_3LE) w=24 ;; S32_LE) w=32 ;; *) w=0 ;; esac
	if [ -z "$rate" ] || [ "$w" = 0 ]; then
		bad "No pude leer el formato del stream ($fmt)"; RES_PLL=FAIL; return
	fi
	r3=$(rd 0x03); r4=$(rd 0x04); r5=$(rd 0x05); r6=$(rd 0x06)
	r11=$(rd 0x0b); r102=$(rd 0x66); r94=$(rd 0x5e)
	p=$((r3 & 7)); [ "$p" -eq 0 ] && p=8
	j=$((r4 >> 2)); d=$(((r5 << 6) | (r6 >> 2)))
	r=$((r11 & 15)); [ "$r" -eq 0 ] && r=16
	bclk=$((rate * ch * w))
	if [ $((rate % 11025)) -eq 0 ]; then fs=44100; else fs=48000; fi
	res=$(awk -v b="$bclk" -v j="$j" -v d="$d" -v r="$r" -v p="$p" -v fs="$fs" \
		'BEGIN { f = b * (j + d / 10000.0) * r / p; t = 2048 * fs;
			 e = (f - t) / t * 1e6; if (e < 0) e = -e;
			 printf "%.3f %.0f", f / 1e6, e }')
	local fout=${res% *} ppm=${res#* }
	if [ $((r3 & 0x80)) -ne 0 ] && [ $((r102 & 0xf0)) -eq $((0xa0)) ] && [ "$ppm" -lt 1000 ]; then
		ok "PLL: ${rate} Hz ${fmt}, BCLK $(awk -v b="$bclk" 'BEGIN{printf "%.3f", b/1e6}') MHz, P=$p J=$j D=$d R=$r → ${fout} MHz"
		RES_PLL=OK
	else
		bad "PLL mal: reg3=$(hx "$r3") P=$p J=$j D=$d R=$r reg102=$(hx "$r102") → ${fout} MHz (${ppm} ppm)"
		RES_PLL=FAIL
	fi
	if [ $((r94 & 0xc0)) -eq $((0xc0)) ]; then
		ok "DACs encendidos durante la reproducción"
	else
		bad "DACs no encendidos durante la reproducción (reg94=$(hx "$r94"))"
		RES_PLL=FAIL
	fi
}

check_idle() {    # tras la reproducción
	local r94 r3
	sleep 1
	r94=$(rd 0x5e); r3=$(rd 0x03)
	if [ $((r94 & 0xc0)) -eq 0 ]; then
		ok "DACs apagados en reposo (reg94=$(hx "$r94"))"
		RES_DAC=OK
	else
		bad "DAC sin apagar en reposo (reg94=$(hx "$r94")): habrá ruido de fondo"
		RES_DAC=FAIL
	fi
	if [ "$(cat "$MODPARAM/gate_pll" 2>/dev/null)" = Y ] && [ $((r3 & 0x80)) -ne 0 ]; then
		bad "PLL sigue encendido en reposo (reg3=$(hx "$r3"))"
		RES_DAC=FAIL
	fi
}

play_song() {
	local pid
	aplay -q -D "plughw:CARD=$CARD" -d "$SONG_SECONDS" "$REF" &
	pid=$!
	sleep 1.5
	check_pll
	printf '  %s(Enter para cortar la canción)%s\n' "$C" "$N"
	while kill -0 "$pid" 2>/dev/null; do
		if read -r -t 0.5 _; then kill "$pid" 2>/dev/null; break; fi
	done
	wait "$pid" 2>/dev/null
}

log_result() {
	local label="$1" result="$2" notes="$3"
	[ -f "$LOG" ] || echo "fecha,placa,i2c,registros,kernel,pll,dac_reposo,canales,audio,resultado,notas" > "$LOG"
	notes=${notes//\"/\'}
	printf '%s,"%s",%s,%s,%s,%s,%s,%s,%s,%s,"%s"\n' "$(date '+%F %T')" "$label" \
		"${RES_I2C:--}" "${RES_REGS:--}" "${RES_KERNEL:--}" "${RES_PLL:--}" \
		"${RES_DAC:--}" "${RES_LR:--}" "${RES_AUDIO:--}" "$result" "$notes" >> "$LOG"
}

test_board() {
	local label="$1" t0 result notes=""
	RES_I2C=""; RES_REGS=""; RES_PLL=""; RES_DAC=""; RES_KERNEL=""; RES_LR=""; RES_AUDIO=""
	t0=$(date +%s)
	KMARK=$(sudo dmesg | wc -l)

	echo
	info "Cargando el driver en la placa ${B}$label${N}..."
	if ! bind_all; then
		bad "La tarjeta de sonido no aparece (¿placa mal colocada?)"
		RES_I2C=FAIL
	else
		udevadm settle -t 5 2>/dev/null
		sleep 1.5            # alsa-restore y encendido de los drivers HP
		set_mixer
		check_static
	fi
	check_kernel

	if [ "$RES_I2C" = OK ]; then
		echo
		printf '  %s▶ Canales:%s debe sonar «Front Left» por la IZQUIERDA y «Front Right» por la DERECHA\n' "$B" "$N"
		speaker-test -D "plughw:CARD=$CARD" -c 2 -t wav -l 1 >/dev/null 2>&1
		if ask "¿Canales correctos?"; then RES_LR=OK; else RES_LR=FAIL; fi

		echo
		printf '  %s▶ Canción de referencia%s (%s s)\n' "$B" "$N" "$SONG_SECONDS"
		play_song
		check_idle
		if ask "¿Suena bien (sin distorsión, ruido ni pops anormales)?"; then RES_AUDIO=OK; else RES_AUDIO=FAIL; fi
	fi

	result=PASS
	for r in "$RES_I2C" "$RES_REGS" "$RES_KERNEL" "$RES_PLL" "$RES_DAC" "$RES_LR" "$RES_AUDIO"; do
		[ "$r" = OK ] || result=FAIL
	done
	if [ "$result" = FAIL ]; then
		read -r -p "  Notas (opcional): " notes
	fi
	log_result "$label" "$result" "$notes"

	echo
	if [ "$result" = PASS ]; then
		printf '  %s%s███  %s: OK  ███%s   (%ss)\n' "$B" "$G" "$label" "$N" "$(( $(date +%s) - t0 ))"
	else
		printf '  %s%s███  %s: FALLO  ███%s   (%ss)\n' "$B" "$R" "$label" "$N" "$(( $(date +%s) - t0 ))"
	fi
}

# ----------------------------------------------------------------- main -----
KEEPALIVE=""
cleanup() {
	trap - EXIT INT TERM
	echo
	info "Dejando el sistema como estaba (driver cargado, PipeWire en marcha)..."
	bind_all >/dev/null 2>&1
	pw_start
	[ -n "$KEEPALIVE" ] && kill "$KEEPALIVE" 2>/dev/null
	info "Resultados en: $LOG"
	if [ "$PAUSE_AT_END" = 1 ]; then read -r -p "Pulsa Enter para cerrar..." _; fi
}

clear 2>/dev/null
printf '%s DACBerry 400 · estación de prueba %s\n\n' "$B" "$N"
printf '  %sAntes de cambiar cada placa, corta su alimentación (interruptor del utillaje).%s\n' "$Y" "$N"
printf '  %sConectar o quitar un HAT con 5V/3V3 presentes puede resetear la Pi o dañar la placa.%s\n\n' "$Y" "$N"

sudo -v || die "Hace falta sudo"
preflight
( while true; do sudo -n true 2>/dev/null; sleep 50; done ) &
KEEPALIVE=$!
trap cleanup EXIT
trap 'exit 130' INT TERM

prepare_song
info "Canción: $SONG  ·  volumen PCM ${VOLUME}%  ·  registro: $LOG"
info "Parando PipeWire mientras dure la prueba..."
pw_stop

count=$( [ -f "$LOG" ] && tail -n +2 "$LOG" | wc -l || echo 0 )
while true; do
	if ! unbind_all; then
		read -r -p "  Cierra el programa que usa la tarjeta y pulsa Enter " _
		continue
	fi
	echo
	printf '%s─── Coloca la siguiente placa ───%s\n' "$B" "$N"
	read -r -p "  Etiqueta / nº de serie (Enter = #$((count + 1)), q = salir): " label
	[ "$label" = q ] || [ "$label" = Q ] && break
	[ -z "$label" ] && label="#$((count + 1))"

	while true; do
		test_board "$label"
		read -r -p "  Enter = siguiente placa · r = repetir esta · q = salir: " next
		case "$next" in
			r|R) unbind_all || break; echo; read -r -p "  Recoloca la placa y pulsa Enter " _ ;;
			*) break ;;
		esac
	done
	count=$((count + 1))
	[ "$next" = q ] || [ "$next" = Q ] && break
done
