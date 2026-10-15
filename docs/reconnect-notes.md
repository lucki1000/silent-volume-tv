# Reconnect implementation notes

Getting the bridge to reconnect reliably took a bit more work than forwarding
keys. These notes cover the pairing problem I ran into on October 12–13, 2026,
the fixes, and what I tested on my TV.

## Problem and saved state

At first, restarting the ESP32 or TV brought back a pairing prompt and stopped
navigation. There turned out to be three separate kinds of saved state:

- NimBLE's flash-backed BLE bonds and security keys.
- The bridge's Preferences profile: remote identity, HID map and metadata.
- The TV's saved input-notification subscriptions (CCCDs).

A saved bond identity alone does not prove its current keys match the other
device or that input notifications are enabled.

## NimBLE-Arduino 2.5.1 persistence patch

In the installed `ble_store_nvs.c`, existing records were
not rewritten when RAM and NVS record counts matched. Replacing a peer's keys
or changing a CCCD can leave the count unchanged, so RAM could work immediately
after pairing while reboot restored stale contents.

[`patch_nimble_bonds.py`](../esp32-firmware/tools/patch_nimble_bonds.py) replaces
four persistence bodies with a snapshot synchronizer for local security, peer
security, CCCDs and host identity records. It preserves the native NVS namespace,
key format and record structures, writes missing/changed blobs, removes trailing
records, and avoids redundant writes. Storage failures are returned to the host.
Each collection is synchronized separately; this is not a multi-collection transaction.

The installer accepts exactly 2.5.1, checks expected function structure, saves the
original source and replaces the file atomically. Reapplying is idempotent.
It changes library source only; rebuilding/uploading puts the fix on a device.
Library reinstalls can remove it. See the [build guide](../esp32-firmware/BUILD.md).

## What changed in the firmware

- Stop advertising after accepting the single TV link; restart while disconnected.
  Stop advertising/scanning before explicit bond deletion, since the tested host
  refused IRK removal while those operations were active.
- Give the TV ten seconds to restore encryption, then allow at most one application
  fallback request per connection. `e` manually rearms one attempt. NimBLE can
  independently request security during encrypted characteristic access.
- Restore/refresh the original ESP32 host's RAM identity cache from saved IRKs
  before starting GAP operations. This private-header helper is guarded for
  original ESP32/NimBLE 2.5.1/host privacy and is skipped elsewhere. It does not
  rewrite NVS or print keys. I haven't tested whether it's still needed with the
  persistence fix in place.
- Track actual subscription callbacks. Where supported, reconcile a live CCCD;
  otherwise recover unknown subscriptions from valid saved records only after
  encryption. An explicit unsubscribe always takes precedence.
- Permit targeted stale TV-bond recovery with `f`, preserving the remote bond and
  learned profile. Refuse active TV links, missing learned remote bonds, and
  ambiguous inventories. This never runs automatically.
- Require complete one-character serial command lines. The capture helper leaves
  terminal echo disabled on close. These changes prevent diagnostic text echoed
  into serial input from accidentally invoking profile-clear command `p`.

Changing security timing and repairing the RAM identity cache didn't solve the
problem on their own. I got it working with the combined fixes and a fresh TV
pairing. Since then, the reset tests below have worked without re-pairing.

## What I tested

| Check | Result on my setup |
| --- | --- |
| ESP32 EN/reset | Navigation returned without pairing prompt |
| Complete ESP32 power removal/reconnection | Navigation returned without pairing prompt |
| TV reboot with ESP powered | Both encrypted links and navigation returned |
| Navigation and volume after recovery | Working; serial showed key reports and HTTP 200 |

I ran these checks with my PEAQ Android 11 TV, RC860G remote, ESP32 core 3.3.12
and patched NimBLE-Arduino 2.5.1. I couldn't find the exact model in Android's
settings, so the TV is identified by its brand and Android version here.
I haven't done a long-duration stress test or tested every fix separately.

Native tests cover persistence updates/deletion/errors, identity-cache handling,
subscription ordering, connection policy, serial noise, capture cleanup and
targeted bond recovery. They help catch regressions, but you still need to check
reconnection on your own hardware.
