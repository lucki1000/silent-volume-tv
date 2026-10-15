#!/usr/bin/env python3
"""Exercise the pinned ESP32 identity-cache repair using synthetic saved keys."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
code = r'''
#include <cassert>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#define CONFIG_IDF_TARGET_ESP32 1
#define NIMBLE_CPP_VERSION_MAJOR 2
#define NIMBLE_CPP_VERSION_MINOR 5
#define NIMBLE_CPP_VERSION_PATCH 1
#define MYNEWT_VAL(name) MYNEWT_VAL_##name
#define MYNEWT_VAL_BLE_HOST_BASED_PRIVACY 1
#define MYNEWT_VAL_BLE_STORE_MAX_BONDS 3
struct ble_addr_t { uint8_t type; uint8_t val[6]; };
struct ble_store_key_sec { ble_addr_t peer_addr; };
struct ble_store_value_sec { ble_addr_t peer_addr; uint8_t irk[16]; bool irk_present; };
struct ble_hs_peer_sec { ble_addr_t peer_addr; uint8_t irk[16]; bool irk_present; };
struct ble_hs_dev_records {
    bool rec_used; uint8_t rand_addr_type;
    uint8_t pseudo_addr[6], rand_addr[6], identity_addr[6];
    ble_hs_peer_sec peer_sec;
};
struct Address {
    ble_addr_t address;
    const ble_addr_t* getBase() const { return &address; }
    std::string toString() const { return "synthetic-peer"; }
    uint8_t getType() const { return address.type; }
};
using NimBLEAddress = Address;
struct SerialStub {
    template<class... Args> void printf(const char*, Args...) {}
    void println(const char*) {}
} Serial;
std::vector<ble_store_value_sec> saved;
struct NimBLEDevice {
    static int getNumBonds() { return saved.size(); }
    static Address getBondedAddress(int i) { return {saved[i].peer_addr}; }
};
bool readFails = false;
int ble_store_read_peer_sec(const ble_store_key_sec* key, ble_store_value_sec* out) {
    if (readFails) return 1;
    for (const auto& peer : saved)
        if (peer.peer_addr.type == key->peer_addr.type && !memcmp(peer.peer_addr.val, key->peer_addr.val, 6)) {
            *out = peer; return 0;
        }
    return 1;
}
ble_hs_dev_records cache[4]{};
int used = 0;
int ble_rpa_get_num_peer_dev_records() { return used; }
ble_hs_dev_records* ble_rpa_find_peer_dev_rec(uint8_t* addr) {
    for (int i = 0; i < used; ++i)
        if (cache[i].rec_used && !memcmp(cache[i].identity_addr, addr, 6)) return &cache[i];
    return nullptr;
}
int ble_rpa_resolv_add_peer_rec(uint8_t* addr) {
    assert(used < 4); auto& record = cache[used++]; record.rec_used = true;
    memcpy(record.identity_addr, addr, 6); memcpy(record.pseudo_addr, addr, 6);
    memcpy(record.rand_addr, addr, 6); return 0;
}
unsigned resolveQueries = 0;
int ble_hs_resolv_rpa(uint8_t*, uint8_t*) { ++resolveQueries; return 1; }
ble_hs_dev_records* ble_hs_resolv_list_find(uint8_t* addr) { return ble_rpa_find_peer_dev_rec(addr); }
#include "BondIdentityCache.h"
int main() {
    ble_store_value_sec peer{};
    peer.peer_addr.val[0] = 7; peer.peer_addr.type = 0; peer.irk_present = true;
    for (int i = 0; i < 16; ++i) peer.irk[i] = uint8_t(i + 1);
    saved = {peer};
    hidbridge::printBondIdentityResolution({peer.peer_addr});
    assert(resolveQueries == 0); // Public peers do not need an RPA check.
    ble_addr_t privateAddress{}; privateAddress.type = 1; privateAddress.val[5] = 0x40;
    hidbridge::printBondIdentityResolution({privateAddress});
    assert(resolveQueries == 2); // Inspect both byte orders without changing saved keys.

    // Missing RAM records are reconstructed from the persistent peer identity.
    hidbridge::restoreBondIdentityCache();
    assert(used == 1 && cache[0].peer_sec.irk_present);
    assert(!memcmp(cache[0].identity_addr, peer.peer_addr.val, 6));
    for (int i = 0; i < 16; ++i) assert(cache[0].peer_sec.irk[i] == peer.irk[15 - i]);
    const auto first = cache[0];
    hidbridge::restoreBondIdentityCache();
    assert(used == 1 && !memcmp(&first, &cache[0], sizeof(first)));

    // A re-pair changes the saved IRK without changing the record count.
    saved[0].irk[0] = 99; cache[0].rand_addr[0] = 42;
    hidbridge::restoreBondIdentityCache();
    assert(used == 1 && cache[0].peer_sec.irk[15] == 99);
    assert(cache[0].rand_addr[0] == 42); // Keep the host's over-air history.
    assert(saved[0].irk[0] == 99); // Persistent keys are never modified.

    // A failed store read or an absent IRK cannot manufacture an identity key.
    cache[0].peer_sec.irk[15] = 1; readFails = true;
    hidbridge::restoreBondIdentityCache(); assert(cache[0].peer_sec.irk[15] == 1);
    readFails = false; saved[0].irk_present = false;
    hidbridge::restoreBondIdentityCache(); assert(cache[0].peer_sec.irk[15] == 1);

    // Missing entries cannot overrun the host's bonds+1 RAM capacity.
    saved[0].irk_present = true; used = 4;
    for (int i = 0; i < used; ++i) { cache[i].rec_used = true; cache[i].identity_addr[0] = uint8_t(i + 20); }
    hidbridge::restoreBondIdentityCache(); assert(used == 4);
}
'''

with tempfile.TemporaryDirectory(prefix="esp32-bond-cache-") as directory:
    path = Path(directory)
    # Stand in for the library's internal declarations, supplied above.
    include = path / "nimble/nimble/host/src/ble_hs_resolv_priv.h"
    include.parent.mkdir(parents=True)
    include.write_text("")
    cpp, binary = path / "test.cpp", path / "test"
    cpp.write_text(code)
    subprocess.run(["g++", "-std=c++11", "-Wall", "-Wextra", "-Werror", "-pedantic",
                    "-I", str(path), "-I", str(root / "BLEHIDBridge"),
                    str(cpp), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print("Bond identity cache tests passed.")
