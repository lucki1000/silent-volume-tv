# ESP32 BLE bridge

The ESP32 acts as a BLE central for the physical remote and a BLE HID peripheral
for the TV. It learns the remote's report map, exposes that profile to the TV,
and forwards supported reports. Consumer Control volume up (`0xE9`) and down
(`0xEA`) are removed from their declared HID fields and sent as HTTP commands to
the Android app. Unrelated bytes with those values are left intact.

Use [BUILD.md](BUILD.md) for the exact tested toolchain and library patch.

## Configuration

From this directory:

```bash
cp BLEHIDBridge/BridgeConfig.h.example BLEHIDBridge/BridgeConfig.h
```

Create the file once, then edit it locally. Do not replace an existing configured
header with the example during a rebuild.

| Setting | Meaning |
| --- | --- |
| `SOURCE_NAME` | Remote's advertised name; template uses `RC860G` |
| `SOURCE_ADDRESS` | Remote's Bluetooth address for direct connection; blank scans by name |
| `SOURCE_ADDRESS_TYPE` | `0` public, `1` random, `0xFF` tries public then random |
| `WIFI_SSID`, `WIFI_PASSWORD` | Local 2.4 GHz Wi-Fi network |
| `TV_IP` | TV's IPv4 address without `http://` or a port |
| `TV_HTTP_PORT` | Android app's receiver port, default `8090` |
| `API_TOKEN` | Token shown by the Android app, without `Bearer ` |
| `VOLUME_REPEAT_MS` | Held-report repeat throttle, default `200`; `0` disables repeats |
| `VOLUME_MAX_AGE_MS` | Drop requests older than this, default `2000` |

A configured address is tried directly even if the remote omits its name or HID
UUID from advertisements. With an empty address, scanning matches `SOURCE_NAME`.
With both blank, the first advertising HID peripheral is selected; keep only your
intended remote in pairing mode. The learned identity is saved and used on later
boots. Changing the configuration does not replace an already learned profile.

Blank network settings disable HTTP volume actions. Volume usages remain removed
from the BLE path, so fill the settings and start the Android receiver before
expecting volume to work. A DHCP reservation for the TV helps keep its address stable.

## Pairing

1. Build and upload the bridge with flash erasure disabled. Open Serial Monitor at
   **115200 baud** with a CR or LF line ending.
2. For a fresh profile, put the remote in Bluetooth pairing mode and make it
   available to the ESP32. A remote still connected directly to the TV may need
   that connection released first. The bridge learns and saves its HID map.
   Keep another way to navigate the TV, such as its controls or a USB keyboard,
   available during setup.
3. Wait for `Advertising "ESP32 TV Remote"` before scanning on the TV. With no
   learned profile, the ESP32 cannot advertise the cloned HID device yet.
4. On the TV, open **Remotes & Accessories → Add accessory** and pair
   **ESP32 TV Remote**. If replacing the standalone navigation example, forget
   that earlier ESP accessory once because its HID descriptor differs.
5. The bridge waits for an encrypted TV connection with key notifications enabled,
   then reconnects the remote. Send `s` and confirm both links show `ready=yes`.
6. Check navigation, then volume. Serial output should show intercepted volume
   events and HTTP `200` responses.

On later boots, the saved profile lets the ESP advertise immediately. The TV
connects first; the remote is claimed only after the TV can receive keys. If TV
readiness stays lost for 1.5 seconds, the bridge releases the remote connection
while retaining its bond. Normal resets do not require clearing or re-pairing.

The RC860G declares a four-byte key report but sends two-byte notifications. The
bridge fills only safely omitted array selectors with their declared no-key value.
Other malformed/unsafe shortened reports are rejected. Its two 255-byte vendor
inputs cannot pass through a TV connection negotiated at MTU 23; key navigation
can still be ready. Voice and vendor features have not been validated.

## HTTP behavior

Each initial press queues one empty-body POST to `/volume/up` or `/volume/down`
with `Authorization: Bearer <token>`. Repeated held reports are throttled; releases
do not send requests. The eight-entry queue and separate FreeRTOS worker keep
network delays out of BLE forwarding. Release/new presses cancel pending repeats;
short taps still retain their initial action. An in-flight request cannot be canceled.

Old/offline actions are dropped. Failed requests and redirects are not retried,
because the TV may already have applied the volume step. Connect/read timeouts
default to 1/1.5 seconds; Wi-Fi reconnect attempts use a ten-second interval.
`s` shows queue and HTTP success/failure/drop counts. Passwords, API tokens and
HTTP response bodies are not logged; device/network addresses are logged.

## Serial commands

Send **one character followed by Enter**. Uppercase works. Longer lines are
discarded; a character without a line ending waits for completion.

| Command | Action |
| --- | --- |
| `s` | Link, subscription, Wi-Fi, HTTP and queue status |
| `?` | Help |
| `d` | Toggle raw RX/TX debugging |
| `a` | Restart TV advertising when disconnected |
| `c` | Resume remote connections after TV readiness |
| `r` | Pause/release the remote connection |
| `t` | Disconnect TV, retaining its bond |
| `k` | Bond identities and key-presence metadata; no key contents |
| `e` | Request security once on a connected, unencrypted TV link |
| `x` | Release defined HID key states and local volume state |
| `m` | Print learned HID report map |
| `p` | Clear learned remote/profile and reboot; BLE bonds remain |
| `f` | Remove the sole non-remote bond and reboot on success |

`p` and `f` are explicit recovery operations. They are unnecessary for normal
uploads or resets. `f` refuses an active TV, a missing learned remote bond, or an
ambiguous bond inventory. Use it only after deliberately forgetting the ESP
accessory on the TV to repair a stale TV bond. See [reconnect notes](../docs/reconnect-notes.md).

## Serial capture

The POSIX helper needs Python 3 and access to your serial port. Close other serial
monitors first. From this directory:

```bash
python3 tools/capture_serial.py --interactive --append --port /dev/ttyUSB0
```

It logs to ignored `serial-diagnostic.log`, requests `s` every ten seconds, and
accepts serial commands, `/note TEXT`, `/help` and `/quit`. From another terminal:

```bash
python3 tools/capture_serial.py --send s
python3 tools/capture_serial.py --send '/note Navigation works'
python3 tools/capture_serial.py --send /quit
```

The shared control FIFO is removed on normal exit. Remove a stale FIFO only after
checking no helper is running. USB power removal ends capture; restart it after
the device returns. The helper leaves 115200/raw/no-echo settings in place on
close to prevent diagnostic output echoing back as commands. Logs can reveal
Bluetooth identities, local addresses and key activity; review before sharing.

## Other files

- `BLEHIDBridge/`: production bridge sketch and its headers.
- `tools/patch_nimble_bonds.py`: version-guarded, idempotent dependency patch.
- `tests/`: native C/C++ helper checks and serial pseudo-terminal checks.
- `examples/AndroidTVRemote/`: standalone serial navigation diagnostic. It does
  not intercept a physical remote or use the Android app; see its own README.
