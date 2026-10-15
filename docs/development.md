# Development

## Firmware helper checks

On Linux/POSIX with Python 3, GCC and G++ installed, from the repository root:

```bash
./scripts/check-firmware.sh
```

This runs the C++ report filter and all eight Python-driven checks, which compile
actual extracted helpers or exercise the serial capture through a pseudo-terminal.
They do not access a physical serial port, upload firmware, or change TV volume.
If a NimBLE library exists at the default sketchbook path, the persistence test
also checks the patch transformation against it without writing to the library.

For AddressSanitizer/UndefinedBehaviorSanitizer on the descriptor filter:

```bash
g++ -std=c++11 -Wall -Wextra -Werror -pedantic \
  -fsanitize=address,undefined -fno-omit-frame-pointer -g \
  esp32-firmware/tests/hid_report_filter_test.cpp -o /tmp/silent-volume-filter-test
ASAN_OPTIONS=detect_leaks=0 /tmp/silent-volume-filter-test
```

These checks do not compile the full Arduino sketch. Follow
[BUILD.md](../esp32-firmware/BUILD.md) for that separate check.

## Android checks

Install the SDK/JDK described in the [app guide](../android-app/README.md#build):

```bash
cd android-app
./scripts/build.sh
```

The default tasks build the debug APK, run Android lint and run the JVM tests.
Tests cover HTTP parsing/authentication/socket lifecycle, silent volume flags,
settings/boot behavior, and overlay behavior/rendering. Runtime Android code has
no third-party library dependencies; tests use JUnit and Robolectric. Socket tests
need permission to bind loopback sockets. Rendering writes a preview under ignored
`app/build/outputs/preview/`.

## CI

The GitHub Actions workflow runs firmware helper checks and Android build/lint/JVM
tests on Ubuntu. The Android job installs API 30 and Build Tools 34.0.0. CI does
not flash hardware or publish artifacts. Full Arduino compilation still requires
the pinned toolchain and a locally created configuration header.

## Device acceptance

After any firmware or app behavior change, check navigation and audible volume
while another app plays subtitles. Test both volume limits and held keys. Check
overlay position/size/timeout if enabled, then ESP32 reset, full power removal and
TV reboot without reopening the Android app. Confirm **Stop receiver** survives
reboot. Record exact TV model, Android version, audio output, remote and board
when reporting compatibility.

## Validation status

All nine native firmware test programs have passed, including sanitizer checks
for the HID filter. Five Android controller/API-feedback tests have also passed
under JDK 17/JUnit. Full Android build/lint/Robolectric checks and full Arduino
compilation of the reorganized source haven't yet been verified. The hardware
checks in the [reconnect notes](reconnect-notes.md) used the working firmware
before remote-selection settings moved into the private configuration header.

## Making releases

Before tagging a release, run the Android build/lint/tests, compile the firmware
with the pinned and patched toolchain, and complete the TV checks above.

Use a stable, privately backed-up signing key for downloadable APKs. Updates to
an installed app require the same signing key; uninstalling to change signers
loses the app's saved settings and token.

Configured firmware and flash/NVS backups can contain Wi-Fi credentials, API
tokens and BLE pairing keys. Distribute the source and blank configuration
template for users to build locally. Review logs and screenshots for private
information before attaching them to an issue or release.

Preserve `LICENSE` and [third-party notices](../THIRD_PARTY_NOTICES.md). Binary
releases also need the applicable dependency sources, notices and relinking
materials described there. The project's MIT license doesn't replace the
licenses of the libraries linked into firmware.
