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

So if your local tree still shows those errors, pull the latest commit and run a clean build:

```bash
idf.py fullclean
idf.py build
```
