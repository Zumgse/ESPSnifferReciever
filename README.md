# Heltec ESP LoRa Receiver (ESP-IDF)

## Why your `idf.py flash` failed

If your project folder path includes parentheses (for example `ESPSnifferReciever-main (2)`),
some generated build commands can fail with shell parsing errors such as:

- `/bin/sh: syntax error near unexpected token '('`

This is a shell/path issue, not a LoRa application logic error.

## Fix options

### Option 1 (recommended): use a path without parentheses/spaces

```bash
mv "ESPSnifferReciever-main (2)" ESPSnifferReciever-main-2
cd ESPSnifferReciever-main-2
idf.py fullclean
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

### Option 2: use the included wrapper

This repository includes `safe_idf.py`, which runs `idf.py` from a safe symlink path.

```bash
./safe_idf.py fullclean
./safe_idf.py build
./safe_idf.py -p /dev/ttyUSB0 flash monitor
```

## Notes

- The message `Adding "flash"'s dependency "all"...` is informational.
- Your failure happened earlier in build command execution because the shell parsed `(` / `)` in path segments.

## Compile error fixes applied

If you saw these exact errors:

- `implicit declaration of function 'ets_delay_us'`
- `comparison is always false due to limited range of data type`

they are fixed in `main/heltec_lora_receiver.c` by:

- using `esp_rom_delay_us(100)` with `#include "esp_rom_sys.h"`
- widening `payload_len` to `size_t` before bounds checks

So if your local tree still shows those errors, the usual root cause is that your checkout is not at the fixed commit (or stale build artifacts are being reused). Run:

```bash
git log --oneline -n 3
idf.py fullclean
idf.py build
```

Additionally, the receiver now uses an ESP-IDF-version-safe delay wrapper:

- IDF >= 5.x: `esp_rom_delay_us(...)`
- older IDF: `ets_delay_us(...)` via `rom/ets_sys.h`

## ESP32-S3 target (Heltec V3)

Heltec WiFi LoRa 32 V3 is an **ESP32-S3** board. If you see:

- `This chip is ESP32-S3, not ESP32. Wrong chip argument?`

then your build target is set to `esp32` instead of `esp32s3`.

This repo pins the target in `sdkconfig.defaults`:

- `CONFIG_IDF_TARGET="esp32s3"`

Use this recovery sequence from the project directory:

```bash
idf.py fullclean
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

If your project path has problematic characters, run the same via the wrapper:

```bash
./safe_idf.py fullclean
./safe_idf.py set-target esp32s3
./safe_idf.py build
./safe_idf.py -p /dev/ttyUSB0 flash monitor
```
