# DACBerry 400 – estación de prueba en serie

Prueba placas una tras otra **sin reiniciar la Pi**. Para cada placa:

1. Desvincula el driver: apaga la salida de forma suave y deja el codec en reset (GPIO26).
2. Te pide que cambies la placa y su etiqueta o número de serie.
3. Vuelve a vincular el driver: el codec nuevo se inicializa desde cero.
4. Hace estas comprobaciones automáticas:
   - el codec responde por I2C;
   - los registros del chip coinciden con los que programó el driver (salida capless, pop-reduction, volúmenes);
   - no hay errores del kernel;
   - durante la reproducción, el PLL da 2048·Fs exactos a partir del BCLK real y los DACs están encendidos;
   - después, en reposo, los DACs y el PLL están apagados.
5. Pruebas de escucha:
   - canales: «Front Left» por la izquierda y «Front Right» por la derecha;
   - un fragmento de tu canción de referencia.
6. Guarda el resultado en `~/dacberry-test-log.csv`.

Cada placa tarda unos 30 s con un fragmento de 20 s.

## Requisitos

- El driver `dacberry400` 3.1.4 o posterior (rama `fix/incremental`), instalado con `install-dkms.sh`.
- `dtoverlay=dacberry400` en `config.txt`.

## Instalación (en la Pi, como tu usuario)

```bash
cd ~/DACBerry-400/tools/test-station
./install.sh
```

Crea `~/dacberry-test/dacberry-test.sh` y el icono **DACBerry 400 Test** en el escritorio y en el menú.

## Uso

- **Escritorio:** doble clic en el icono. Se abre una terminal y te pide la contraseña de sudo una vez.
- **SSH desde Windows:** `DACBerry-Test.bat` (edita la IP), o bien `ssh -t osa@IP ~/dacberry-test/dacberry-test.sh`.

La primera vez te pide la ruta de la canción de referencia (puedes arrastrar el archivo a la terminal). Con `ffmpeg` se convierte una sola vez a WAV 48 kHz y se recorta.

La configuración está en `~/dacberry-test/dacberry-test.conf`:

| Variable | Qué ajusta |
|---|---|
| `VOLUME` | % del control PCM durante la prueba |
| `SONG` | Canción de referencia |
| `SONG_START`, `SONG_SECONDS` | Inicio y duración del fragmento |
| `LOG` | Archivo de resultados |

Mientras dura la prueba, PipeWire se para para que nada abra la tarjeta. Al salir con `q` o Ctrl+C se deja todo como estaba: el driver cargado y PipeWire en marcha.

## Seguridad eléctrica

El script solo resuelve la parte de software. **No cambies la placa con 5 V y 3V3 presentes en el conector**:
- la corriente de arranque de los condensadores de la placa puede tirar el 3V3 de la Pi y resetearla;
- al insertarla torcida, un pin de 5 V puede tocar un GPIO;
- las líneas I2S e I2C pueden alimentar el codec a través de sus diodos de protección antes que su alimentación.

Lo recomendable es un utillaje con interruptor (o *load switch*) en 5 V y 3V3 hacia la placa bajo prueba, que se corta antes de cada cambio. Mejor aún si lleva pogo pins.

Cada vinculación del driver reproduce el pop de arranque en frío del codec. Es normal y no indica fallo.
