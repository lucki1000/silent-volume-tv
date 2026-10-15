# Build the ESP32 firmware

These are the versions and board settings I used for the working bridge and
reconnect tests on October 13, 2026.

## Tested configuration

| Component/setting | Value |
| --- | --- |
| Arduino CLI | `1.5.1` |
| ESP32 Arduino core | `3.3.12` |
| NimBLE-Arduino | `2.5.1` **with the persistence patch below** |
| Board target | `esp32:esp32:esp32da` / ESP32-WROOM-DA Module |
| CPU / flash | 240 MHz / 80 MHz, QIO, 4 MB |
| Partition | Default 4 MB with SPIFFS (`default`) |
| Loop / events core | 1 / 1 |
| Debug / flash erase | None / disabled |

I use a WROOM development board; the chip identifies itself as ESP32-D0WD-V3.
I built with the **ESP32-WROOM-DA Module** board entry listed above. That's the
compiler setting I tested; the board itself wasn't identified as a DA model.

The working build used about 94% of the default app partition, so keep an eye on
the size when making changes. Other board targets may need different settings.

## Install and patch dependencies

Install Arduino CLI (or Arduino IDE), ESP32 core **3.3.12**, and NimBLE-Arduino
**2.5.1**. With CLI, from this directory:

```bash
arduino-cli core update-index --additional-urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
arduino-cli core install esp32:esp32@3.3.12 --additional-urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
arduino-cli lib install 'NimBLE-Arduino@2.5.1'
```

Create and fill `BLEHIDBridge/BridgeConfig.h` as described in [README.md](README.md).
Apply the local library patch **before compiling**:

```bash
# Default library path: ~/Arduino/libraries/NimBLE-Arduino
python3 tools/patch_nimble_bonds.py
python3 tools/patch_nimble_bonds.py --apply
```

For a different Arduino sketchbook, pass `--library /path/to/NimBLE-Arduino`.
Without `--apply`, the tool prints a proposed diff or reports an installed patch.
It requires exactly 2.5.1, saves a `.blehidbridge-original` source backup, and
does not touch a device. Library upgrades/reinstalls can overwrite this patch.
Review other versions separately; stock 2.5.1 does not reproduce the reconnect fix.

## Compile

Run from `esp32-firmware/`:

```bash
BRIDGE_FQBN='esp32:esp32:esp32da:UploadSpeed=921600,CPUFreq=240,FlashFreq=80,FlashMode=qio,FlashSize=4M,PartitionScheme=default,DebugLevel=none,LoopCore=1,EventsCore=1,EraseFlash=none'
BRIDGE_BUILD_DIR="$PWD/build/bridge"

arduino-cli compile --fqbn "$BRIDGE_FQBN" \
  --build-path "$BRIDGE_BUILD_DIR" BLEHIDBridge
```

Check that the selected NimBLE library is the patched copy. The output binary
contains the configured credentials and must remain private. In Arduino IDE,
open `BLEHIDBridge/BLEHIDBridge.ino` and select the same settings.

## Upload

Close Serial Monitor/capture before uploading. For a blank board, use the normal
IDE or CLI upload with the tested target and flash erasure disabled:

```bash
arduino-cli upload --fqbn "$BRIDGE_FQBN" --port /dev/ttyUSB0 \
  --input-dir "$BRIDGE_BUILD_DIR" BLEHIDBridge
```

For the reconnect tests, I used an **application-only** esptool upload at
`0x10000` to keep my existing bootloader, partition table and NVS intact. Use this
method only when the compatible default layout is already installed. A blank
board needs the normal upload above. With esptool **5.3.1** installed/on PATH:

```bash
esptool --chip esp32 --port /dev/ttyUSB0 --baud 460800 \
  write-flash --flash-mode keep --flash-freq keep --flash-size keep \
  0x10000 "$BRIDGE_BUILD_DIR/BLEHIDBridge.ino.bin"
```

Keep the same partition layout and **Erase All Flash disabled** to preserve
bonds/profile. The normal IDE/CLI command is included for convenience; the
reconnect checks used the application-only method. There's no need to erase
flash, clear the profile or forget accessories for a routine rebuild.

After uploading, check both links with `s`, navigation, and HTTP volume. Then
check ESP reset, complete ESP power removal, and TV reboot without re-pairing.

## Restore the library

To undo the source patch, restore `ble_store_nvs.c.blehidbridge-original` over
`ble_store_nvs.c` in the library and rebuild. The path below the library root is
`src/nimble/nimble/host/store/config/src/`. Restoring source alone does not alter
the running ESP. Uploading the unpatched build can restore the reconnect defect.
