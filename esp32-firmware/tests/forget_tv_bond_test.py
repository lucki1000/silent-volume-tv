#!/usr/bin/env python3
"""Exercise the actual TV-bond recovery helper; the RC bond must survive."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
sketch = (root / "BLEHIDBridge/BLEHIDBridge.ino").read_text()
start = sketch.index("static bool forgetTvBond() {")
end = sketch.index("\nstatic void printHelp()", start)
code = r'''
#include <algorithm>
#include <atomic>
#include <cassert>
#include <string>
#include <vector>
constexpr int BLE_HS_CONN_HANDLE_NONE = 65535;
std::atomic<int> tvHandle{BLE_HS_CONN_HANDLE_NONE};
std::string selectedAddress = "remote";
unsigned char selectedAddressType = 0;
struct NimBLEAddress {
    std::string value; unsigned char type;
    NimBLEAddress(std::string value, unsigned char type) : value(value), type(type) {}
    bool operator==(const NimBLEAddress& other) const { return value == other.value && type == other.type; }
    bool operator!=(const NimBLEAddress& other) const { return !(*this == other); }
    std::string toString() const { return value; }
};
std::vector<NimBLEAddress> bonds;
int deletes = 0;
bool deleteFails = false;
bool advertising = true, scanning = true;
struct Advertising { void stop() { advertising = false; } } advertiser;
struct Scan { void stop() { scanning = false; } } scanner;
struct NimBLEDevice {
    static Advertising* getAdvertising() { return &advertiser; }
    static Scan* getScan() { return &scanner; }
    static int getNumBonds() { return bonds.size(); }
    static NimBLEAddress getBondedAddress(int i) { return bonds.at(i); }
    static bool isBonded(const NimBLEAddress& address) { return std::find(bonds.begin(), bonds.end(), address) != bonds.end(); }
    static bool deleteBond(const NimBLEAddress& address) {
        assert(!advertising && !scanning); // Native unpair otherwise returns busy.
        ++deletes;
        if (deleteFails) return false;
        auto item = std::find(bonds.begin(), bonds.end(), address);
        assert(item != bonds.end()); bonds.erase(item); return true;
    }
};
struct SerialStub {
    void println(const char*) {}
    template<class... Args> void printf(const char*, Args...) {}
} Serial;
''' + sketch[start:end] + r'''
int main() {
    const NimBLEAddress remote("remote", 0), tv("tv", 0), other("other", 1);
    for (auto initial : {std::vector<NimBLEAddress>{remote, tv}, std::vector<NimBLEAddress>{tv, remote}}) {
        advertising = scanning = true;
        bonds = initial; assert(forgetTvBond());
        assert(bonds == std::vector<NimBLEAddress>{remote});
    }
    for (auto initial : {std::vector<NimBLEAddress>{}, {remote}, {tv}, {remote, tv, other}}) {
        bonds = initial; int before = deletes;
        assert(!forgetTvBond() && bonds == initial && deletes == before);
    }
    bonds = {remote, tv}; tvHandle = 2;
    assert(!forgetTvBond() && bonds.size() == 2); tvHandle = BLE_HS_CONN_HANDLE_NONE;
    selectedAddress = ""; assert(!forgetTvBond() && bonds.size() == 2);
    selectedAddress = "remote"; selectedAddressType = 1;
    assert(!forgetTvBond() && bonds.size() == 2); selectedAddressType = 0;
    deleteFails = true; assert(!forgetTvBond() && bonds.size() == 2);
}
'''
with tempfile.TemporaryDirectory(prefix="forget-tv-bond-") as directory:
    path = Path(directory)
    source, binary = path / "test.cpp", path / "test"
    source.write_text(code)
    subprocess.run(["g++", "-std=c++11", "-Wall", "-Wextra", "-Werror", "-pedantic",
                    str(source), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print("TV bond recovery: RC preservation, ambiguity, active link and failure checks passed")
