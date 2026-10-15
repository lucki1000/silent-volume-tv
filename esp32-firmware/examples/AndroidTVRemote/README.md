# Standalone navigation diagnostic

This earlier sketch turns serial characters into BLE Consumer Control navigation
keys. It does not bridge a physical remote or perform Wi-Fi volume control.
Use `../../BLEHIDBridge/` for the complete project.

Open `AndroidTVRemote.ino`, install the ESP32 core and NimBLE-Arduino, select an
appropriate original ESP32 board/USB port, and upload. Open Serial Monitor at
115200 baud. Pair **ESP32 TV Remote** in the TV's accessory settings and wait
for `ready=yes`. Commands are single characters; uppercase also works:

| Character | Action |
| --- | --- |
| `u`, `d`, `l`, `r` | Up, down, left, right |
| `o` | OK/select |
| `h`, `b` | Home, back |
| `s` | Status |
| `a` | Restart advertising while disconnected |
| `x` | Retry releasing keys |
| `?` | Help |

Each tap holds the usage for 80 ms and sends a release. A queued notification
does not confirm Android handled it. Switching between this sketch and the bridge
changes the HID descriptor; forget the earlier ESP accessory on the TV once
before pairing the other sketch. Keep flash erasure disabled for routine uploads.
