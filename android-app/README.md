# Silent Volume TV app

This is the Android half of my volume-bar workaround. It runs on the TV and
receives volume up/down commands from the ESP32 over the local network. I built
it for my Android 11 TV; it doesn't need root or an Accessibility service.

For the complete setup, start with the [main README](../README.md). The app can
also receive commands from other devices using its [HTTP API](../docs/architecture.md#local-api).

## Build

You'll need **JDK 17**, Android SDK Platform **30**, Build Tools **34.0.0** and
Platform Tools. The included wrapper uses Gradle **8.9**, with Android Gradle
Plugin **8.7.3**. Install the Android SDK command-line tools through Android
Studio or your usual SDK setup.

From `android-app/`, with `sdkmanager` on PATH:

```bash
export JAVA_HOME=/path/to/your/jdk-17
export ANDROID_HOME=/path/to/your/android-sdk
export PATH="$ANDROID_HOME/platform-tools:$PATH"

sdkmanager --sdk_root="$ANDROID_HOME" \
  'platforms;android-30' 'build-tools;34.0.0' 'platform-tools'
sdkmanager --sdk_root="$ANDROID_HOME" --licenses

# Build the debug APK, run lint and run the JVM tests.
./scripts/build.sh
```

Alternatively, put `sdk.dir=/absolute/path/to/android-sdk` in ignored
`local.properties`. The build helper recognizes common Linux JDK 17 paths if
`JAVA_HOME` isn't set. On openSUSE, `sudo zypper install java-17-openjdk-devel`
installs JDK 17.

The helper keeps its default caches and Android user data in `.build-support/`.
Keep signing keys private and backed up: updating an installed APK requires the
same signing key. A fresh checkout normally creates a different debug key.
Public APK releases should use a stable release key; see the
[release guidance](../docs/development.md#making-releases).

This is a sideloaded Android 11 app with target SDK 30. It isn't configured for
Google Play submission.

## Install and start the receiver

1. Connect and authorize ADB on the TV, then check `adb devices -l`.
2. Install the APK:

   ```bash
   adb install -r app/build/outputs/apk/debug/app-debug.apk
   ```

3. Open **Silent Volume TV** from the TV launcher and press **Start receiver**.
4. Note the TV's LAN address, receiver port and access token. The default port is
   **8090**. Put these values in the ESP32's private `BridgeConfig.h`.
5. If you want the smaller volume bar, follow the overlay steps below.

The receiver keeps running when you close the app screen or open another TV app.
It doesn't need to take audio focus or interrupt playback to change the volume.

## Try a command

From a computer on the same network, replace the address and token below with
those shown on your TV:

```bash
TV_ADDRESS='192.168.1.50'
TV_TOKEN='paste-the-32-character-token-shown-on-the-TV'

# One step up.
curl --fail-with-body --max-time 5 -X POST \
  -H "Authorization: Bearer $TV_TOKEN" \
  "http://$TV_ADDRESS:8090/volume/up"

# One step down.
curl --fail-with-body --max-time 5 -X POST \
  -H "Authorization: Bearer $TV_TOKEN" \
  "http://$TV_ADDRESS:8090/volume/down"

# Full volume state, without showing the overlay.
curl --fail-with-body --max-time 5 \
  -H "Authorization: Bearer $TV_TOKEN" \
  "http://$TV_ADDRESS:8090/status"

# Just the current volume as an integer.
curl --silent --show-error --fail-with-body --max-time 5 \
  -H "Authorization: Bearer $TV_TOKEN" \
  "http://$TV_ADDRESS:8090/volume/integer"
```

Change `8090` too if you chose another port. Each POST requests one volume step;
a command at the minimum or maximum can succeed without changing the value.
The [API reference](../docs/architecture.md#local-api) lists responses and errors.

Use a trusted LAN: the API requires a token but uses unencrypted HTTP. Don't
forward its port to the Internet. A DHCP reservation helps keep the TV address
stable. If you generate a new token in the app, update the ESP32's private header
and rebuild/upload its firmware too.

## Use the smaller volume overlay

![My TV settings: overlay on the right at 75%, with the access token covered](../docs/images/app-settings-redacted.png)

I use **Right side** at **75%** size. A fresh install starts with the overlay off
and size **100%**. I've covered my token in the screenshot.

1. Enable **Show custom volume overlay for network commands**.
2. Press **Allow display over other apps**, select **Silent Volume TV** in
   Android's settings, allow it, then return to the app.
3. Check that the app says the overlay is ready, then send a volume command.

Choose either side and **75%**, **100%**, **125%** or **150%** size. Position and
size are saved across reboots. The panel appears for two seconds after the last
successful network volume command. It doesn't take D-pad focus, and status
requests don't show it. Turning the overlay off or stopping the receiver hides it.

Some TVs don't offer the overlay permission screen. With ADB authorized, try:

```bash
adb shell appops set dev.silentvolume.tv SYSTEM_ALERT_WINDOW allow
```

Return to the app and check the permission status again. Volume commands still
work if the overlay is off or its permission is missing.

## Background operation and reboot

**Start automatically after reboot** is enabled by default. Open the app once
and start the receiver after installing it. It can then start at boot, before
unlock, and after an app update. The app screen doesn't have to open for commands
to work.

**Stop receiver** saves a disabled state, so reboot won't restart it. Turning off
only the reboot checkbox leaves a currently running receiver active. Force-stop
blocks automatic operation until you launch the app again. TV power-saving and
background-service settings can also affect it.

After an update, open the app, check the reboot option and press **Start
receiver** before trying a TV reboot. Settings and the token survive an in-place
update signed with the same key.

## Troubleshooting

| Problem | What to check |
| --- | --- |
| Can't reach the receiver | Receiver started, TV address/port correct, both devices on a network that allows them to communicate |
| HTTP `401` | Token matches the TV; the ESP32 header contains the token without the `Bearer ` prefix |
| HTTP `409` | Android reports fixed volume; this app can't change that output's volume |
| Overlay missing | Overlay enabled, permission ready, command sent to an up/down endpoint |
| Original volume bar still appears | Remote connected through the bridge; IR or HDMI-CEC isn't sending volume commands separately |
| Commands stop after reboot | Last boot result in the app, reboot option, TV background-service settings |

The app changes Android's media stream without requesting the system volume UI.
It can't suppress a bar drawn independently by TV firmware or another HDMI device.
Try it while playing content through your usual audio output.

For reboot problems, collect these **before opening the app**, with ADB connected:

```bash
adb shell getprop ro.product.model
adb shell dumpsys activity services dev.silentvolume.tv
adb shell dumpsys package dev.silentvolume.tv
adb logcat -d -s SilentVolumeTV:I AndroidRuntime:E
```

The app records the last boot event and startup result; opening it doesn't
replace that record. Its logs omit the token, but review diagnostic output for
other private information before sharing it.

See [development](../docs/development.md) for tests and TV checks, and
[architecture](../docs/architecture.md) for HTTP framing, lifecycle and overlay
implementation details.
