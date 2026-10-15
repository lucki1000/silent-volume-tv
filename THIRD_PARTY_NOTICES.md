# Third-party notices

This project's own code and documentation use the [MIT License](LICENSE).
The components below retain their existing licenses.

## Bundled Gradle wrapper

`android-app/gradlew`, `android-app/gradlew.bat` and
`android-app/gradle/wrapper/gradle-wrapper.jar` are Gradle wrapper files. Preserve
their copyright headers. The Gradle 8.9 distribution's
[Apache License 2.0](third_party/gradle/LICENSE) and [NOTICE](third_party/gradle/NOTICE)
are included for attribution. The wrapper configuration verifies the distribution
with its supplied SHA-256 checksum.

## NimBLE-Arduino

The bridge uses NimBLE-Arduino **2.5.1**, installed separately. Its upstream
[license](https://github.com/h2zero/NimBLE-Arduino/blob/2.5.1/LICENSE) is Apache 2.0.
Preserve its notices and those of its bundled components when redistributing
library source or compiled firmware.

`tools/patch_nimble_bonds.py` under `esp32-firmware/` modifies an installed
NimBLE-Arduino source file while preserving its existing file headers. Retain
upstream notices when distributing patched dependency source or firmware. Review
dependency distribution obligations when preparing public binary releases.

## ESP32 Arduino core, Wi-Fi and HTTP

The tested ESP32 Arduino core is **3.3.12**. Its
[package metadata](https://raw.githubusercontent.com/espressif/arduino-esp32/3.3.12/package.json)
declares **LGPL-2.1-or-later**. The installed Wi-Fi and HTTPClient headers also
carry LGPL 2.1-or-later notices. These components are installed separately; they
aren't copied into this repository.

Distributing a firmware binary that links these libraries requires meeting their
LGPL terms, including the applicable source and relinking requirements in
[section 6](https://opensource.org/license/lgpl-2-1). A license for this project's
own code doesn't replace those requirements. Other ESP32 SDK components can have
their own licenses too.

## Other tools and dependencies

Android SDK components, Android Gradle Plugin, JUnit and Robolectric are installed
separately. Their licenses and any transitive dependency notices still apply
when distributing those components. This file isn't a complete notice bundle
for future APK or firmware releases.

## Screenshots

The TV screenshots include screensaver photography with on-screen credits.
Rights to that photography remain with its creators; a software license for this
project doesn't cover it. The app settings screenshot has its access token covered.
