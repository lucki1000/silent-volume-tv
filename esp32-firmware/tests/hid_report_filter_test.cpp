#include "../BLEHIDBridge/HidReportFilter.h"
#include <cassert>
#include <iostream>
#include <vector>

using hidbridge::Descriptor;
using Bytes = std::vector<uint8_t>;

static void expect(Descriptor& descriptor, uint8_t id, const Bytes& input, const Bytes& expected, uint8_t expectedVolume) {
    uint8_t output[hidbridge::MAX_REPORT_BYTES] = {}, volume = 99;
    const size_t index = descriptor.find(id, 1);
    assert(descriptor.filter(index, input.data(), input.size(), output, volume));
    assert(Bytes(output, output + input.size()) == expected);
    assert(volume == expectedVolume);
}

static void expectNormalized(Descriptor& descriptor, uint8_t id, const Bytes& input,
                             const Bytes& normalized, const Bytes& filtered, uint8_t volume) {
    const size_t index = descriptor.find(id,1);
    uint8_t output[hidbridge::MAX_REPORT_BYTES];
    assert(descriptor.normalizeInput(index, input.data(), input.size(), output));
    assert(Bytes(output, output + descriptor.reports[index].bytes()) == normalized);
    expect(descriptor, id, normalized, filtered, volume);
}

static void consumerArray() {
    // Three 16-bit Consumer selectors: test simultaneous keys and releases.
    const Bytes map = {0x05,0x0C,0x09,0x01,0xA1,0x01,0x85,0x01,
                       0x15,0x00,0x26,0xFF,0x03,0x19,0x00,0x2A,0xFF,0x03,
                       0x75,0x10,0x95,0x03,0x81,0x00,0xC0};
    Descriptor descriptor;
    assert(descriptor.parse(map.data(), map.size()));
    expect(descriptor, 1, {0xE9,0,0x23,2,0x42,0}, {0,0,0x23,2,0x42,0}, 1);
    expect(descriptor, 1, {0xE9,0,0xEA,0,0x24,2}, {0,0,0,0,0x24,2}, 3);
    expect(descriptor, 1, {0,0,0xEA,0,0x24,2}, {0,0,0,0,0x24,2}, 2);
    expect(descriptor, 1, {0,0,0,0,0,0}, {0,0,0,0,0,0}, 0);
    expect(descriptor, 1, {0xE9,1,0xEA,2,0xEA,3}, {0xE9,1,0xEA,2,0xEA,3}, 0);
    // Every other 16-bit usage, including literal E9/EA in a high byte, passes.
    for (uint16_t usage = 0; usage < 0x400; ++usage) {
        if (usage == 0xE9 || usage == 0xEA) continue;
        const Bytes input = {uint8_t(usage), uint8_t(usage >> 8), 0,0,0,0};
        expect(descriptor, 1, input, input, 0);
    }
    uint8_t output[64], volume = 0;
    const uint8_t bad[] = {0xE9,0};
    assert(!descriptor.filter(0, bad, sizeof(bad), output, volume));
}

static void unalignedBitmapAndVendorPage() {
    // Three constant bits, then two volume bits, HOME, then two constants.
    // A following vendor byte deliberately contains E9 or EA.
    const Bytes map = {0x05,0x0C,0x09,0x01,0xA1,0x01,0x85,0x07,
                       0x15,0,0x25,1,0x75,1,0x95,3,0x81,1,
                       0x09,0xE9,0x09,0xEA,0x0A,0x23,0x02,0x95,3,0x81,2,
                       0x95,2,0x81,1,
                       0x06,0,0xFF,0x09,0xE9,0x26,0xFF,0,0x75,8,0x95,1,0x81,2,0xC0};
    Descriptor descriptor;
    assert(descriptor.parse(map.data(), map.size()));
    for (unsigned byte = 0; byte <= 255; ++byte) {
        const uint8_t volume = uint8_t((byte >> 3) & 3);
        expect(descriptor, 7, {uint8_t(byte),0xE9}, {uint8_t(byte & ~0x18),0xE9}, volume);
        expect(descriptor, 7, {uint8_t(byte),0xEA}, {uint8_t(byte & ~0x18),0xEA}, volume);
    }
}

static void logicalSelectorsAndNull() {
    // Explicit usages: logical selector 1=no key, 2=VOL+, 3=HOME, 4=VOL-.
    const Bytes explicitMap = {0x05,0x0C,0x09,1,0xA1,1,0x15,1,0x25,4,
                               0x09,0,0x09,0xE9,0x0A,0x23,2,0x09,0xEA,
                               0x75,8,0x95,2,0x81,0,0xC0};
    Descriptor descriptor;
    assert(descriptor.parse(explicitMap.data(), explicitMap.size()));
    expectNormalized(descriptor, 0, {2}, {2,1}, {1,1}, 1); // No-key selector is 1, not zero.
    expect(descriptor, 0, {2,3}, {1,3}, 1);
    expect(descriptor, 0, {3,4}, {3,1}, 2);
    expect(descriptor, 0, {1,1}, {1,1}, 0);
    uint8_t neutral[64] = {};
    descriptor.neutral(0, neutral);
    assert(neutral[0] == 1 && neutral[1] == 1);

    // No usage zero: use an out-of-range selector permitted by Null State.
    const Bytes nullMap = {0x05,0x0C,0x09,1,0xA1,1,0x15,1,0x25,2,
                           0x09,0xE9,0x09,0xEA,0x75,2,0x95,1,0x81,0x40,
                           0x75,6,0x95,1,0x81,1,0xC0};
    assert(descriptor.parse(nullMap.data(), nullMap.size()));
    expect(descriptor, 0, {0xA1}, {0xA3}, 1);
    expect(descriptor, 0, {0xA2}, {0xA3}, 2);
    expect(descriptor, 0, {0xA3}, {0xA3}, 0);
}

static void keyboardAndPushPop() {
    const Bytes map = {
        0x05,1,0x09,6,0xA1,1,0x85,2,
        0x05,7,0x19,0xE0,0x29,0xE7,0x15,0,0x25,1,0x75,1,0x95,8,0x81,2,
        0x75,8,0x95,1,0x81,1,
        0x19,0,0x29,0xFF,0x15,0,0x26,0xFF,0,0x75,8,0x95,6,0x81,0,
        0x95,1,0x91,2, // Output report shares ID but has its own offset/length.
        0xA4, // PUSH globals, including report ID.
        0x05,0x0C,0x85,3,0x09,0xE9,0x09,0xEA,0x15,0,0x25,1,0x75,1,0x95,2,0x81,2,
        0x75,6,0x95,1,0x81,1,
        0xB4, // POP restores keyboard report 2 and its field parameters.
        0x09,4,0x75,8,0x95,1,0x81,2,0xC0
    };
    Descriptor descriptor;
    assert(descriptor.parse(map.data(), map.size()));
    assert(descriptor.reports[descriptor.find(2,1)].bytes() == 9);
    assert(descriptor.reports[descriptor.find(2,2)].bytes() == 1);
    assert(descriptor.reports[descriptor.find(3,1)].bytes() == 1);
    const Bytes keys = {0xEA,0xE9,0xE9,0xEA,4,0,0,0,0xEA};
    expect(descriptor, 2, keys, keys, 0); // Keyboard E9/EA are not Consumer volume.
    expect(descriptor, 3, {0xFF}, {0xFC}, 3);
}

static void signedSelectorsAndExtendedUsages() {
    // -2 is release, -1 is VOL+, and 0 is VOL-. A raw zero is a real
    // key in this descriptor, so treating every zero as release is wrong.
    const Bytes signedMap = {0x05,0x0C,0x09,1,0xA1,1,0x15,0xFE,0x25,0,
                             0x09,0,0x09,0xE9,0x09,0xEA,0x75,8,0x95,1,0x81,0,0xC0};
    Descriptor descriptor;
    assert(descriptor.parse(signedMap.data(), signedMap.size()));
    expect(descriptor, 0, {0xFE}, {0xFE}, 0);
    expect(descriptor, 0, {0xFF}, {0xFE}, 1);
    expect(descriptor, 0, {0}, {0xFE}, 2);

    // Extended 32-bit usages specify their page independently of the global
    // page. Exercise full-width bit operations and both volume fields.
    const Bytes extendedMap = {0x05,7,0x09,6,0xA1,1,0x15,0,0x25,1,
                               0x0B,0xE9,0,0x0C,0,0x0B,0xEA,0,0x0C,0,
                               0x75,32,0x95,2,0x81,2,0xC0};
    assert(descriptor.parse(extendedMap.data(), extendedMap.size()));
    expect(descriptor, 0, {1,0,0,0,1,0,0,0}, {0,0,0,0,0,0,0,0}, 3);
}

static void largeRemoteReports() {
    // A small navigation report plus large vendor feature/output reports and
    // a 512-byte input report. Volume bits are at the END of that input; bytes
    // E9/EA in its 511-byte vendor payload must remain untouched.
    const Bytes map = {
        0x05,0x0C,0x09,1,0xA1,1,0x85,1,
        0x15,0,0x26,0xFF,3,0x19,0,0x2A,0xFF,3,0x75,16,0x95,1,0x81,0,0xC0,
        0x06,0,0xFF,0x09,1,0xA1,1,0x15,0,0x26,0xFF,0,
        0x85,9,0x09,1,0x75,8,0x96,0,2,0xB1,2, // 512-byte feature.
        0x85,10,0x09,1,0x95,128,0x91,2, // 128-byte output.
        0x85,8,0x09,1,0x96,0xFF,1,0x81,2, // 511 vendor input bytes.
        0x05,0x0C,0x09,0xE9,0x09,0xEA,0x0A,0x23,2,
        0x15,0,0x25,1,0x75,1,0x95,3,0x81,2,
        0x95,5,0x81,1,0xC0
    };
    Descriptor descriptor;
    assert(descriptor.parse(map.data(), map.size()));
    assert(descriptor.reports[descriptor.find(1,1)].bytes() == 2);
    assert(descriptor.reports[descriptor.find(9,3)].bytes() == 512);
    assert(descriptor.reports[descriptor.find(10,2)].bytes() == 128);
    assert(descriptor.reports[descriptor.find(8,1)].bytes() == 512);
    Bytes input(512);
    for (size_t i = 0; i < 511; ++i) input[i] = i % 2 ? 0xE9 : 0xEA;
    input.back() = 0xFF;
    Bytes expected = input;
    expected.back() = 0xFC;
    expect(descriptor, 8, input, expected, 3);
    expect(descriptor, 8, expected, expected, 0);
    expect(descriptor, 1, {0xE9,0}, {0,0}, 1);
    uint8_t neutral[hidbridge::MAX_REPORT_BYTES];
    descriptor.neutral(descriptor.find(8,1), neutral);
    assert(Bytes(neutral, neutral + 512) == Bytes(512, 0));
    uint8_t volume = 0;
    assert(!descriptor.filter(descriptor.find(8,1), input.data(), 511, neutral, volume));
}

static void rc860gCapturedDescriptor() {
    // Exact 65-byte Report Map from serial-diagnostic.log. Only report 03
    // declares key usages; E9 and E8 are report IDs for opaque 255-byte data.
    const Bytes map = {
        0x05,0x0C,0x09,0x01,0xA1,0x01,0x85,0x03,0x19,0x00,0x2A,0x9C,0x02,
        0x15,0x00,0x26,0x9C,0x02,0x95,0x02,0x75,0x10,0x81,0x00,0xC0,
        0x05,0x0C,0x09,0x01,0xA1,0x01,0x85,0xE9,0x95,0xFF,0x75,0x08,
        0x15,0x00,0x26,0xFF,0x00,0x81,0x00,0xC0,
        0x05,0x0C,0x09,0x01,0xA1,0x01,0x85,0xE8,0x95,0xFF,0x75,0x08,
        0x15,0x00,0x26,0xFF,0x00,0x81,0x00,0xC0
    };
    Descriptor descriptor;
    assert(map.size() == 65 && descriptor.parse(map.data(), map.size()));
    assert(descriptor.reports.size() == 3);
    assert(descriptor.reports[descriptor.find(3,1)].bytes() == 4);
    expect(descriptor, 3, {0xE9,0,0x42,0}, {0,0,0x42,0}, 1);
    expect(descriptor, 3, {0xEA,0,0x23,2}, {0,0,0x23,2}, 2);
    expect(descriptor, 3, {0xE9,0,0xEA,0}, {0,0,0,0}, 3);
    expect(descriptor, 3, {0,0,0,0}, {0,0,0,0}, 0);
    // Actual RC860G notifications contain one 16-bit selector (two bytes).
    expectNormalized(descriptor, 3, {0x42,0}, {0x42,0,0,0}, {0x42,0,0,0}, 0);
    expectNormalized(descriptor, 3, {0x23,2}, {0x23,2,0,0}, {0x23,2,0,0}, 0);
    expectNormalized(descriptor, 3, {0xE9,0}, {0xE9,0,0,0}, {0,0,0,0}, 1);
    expectNormalized(descriptor, 3, {0xEA,0}, {0xEA,0,0,0}, {0,0,0,0}, 2);
    expectNormalized(descriptor, 3, {0,0}, {0,0,0,0}, {0,0,0,0}, 0);
    expectNormalized(descriptor, 3, {0x42,0,0x23,2}, {0x42,0,0x23,2}, {0x42,0,0x23,2}, 0);
    for (uint16_t usage = 0; usage <= 0x29C; ++usage) {
        if (usage == 0xE9 || usage == 0xEA) continue;
        const Bytes input = {uint8_t(usage), uint8_t(usage >> 8), 0,0};
        expect(descriptor, 3, input, input, 0);
        expectNormalized(descriptor, 3, {uint8_t(usage), uint8_t(usage >> 8)}, input, input, 0);
    }
    uint8_t neutral[hidbridge::MAX_REPORT_BYTES];
    assert(descriptor.neutral(descriptor.find(3,1), neutral));
    assert(Bytes(neutral, neutral + 4) == Bytes(4, 0));
    for (const Bytes& invalid : {Bytes{}, Bytes{0x42}, Bytes{0x42,0,0xE9}, Bytes(5,0)})
        assert(!descriptor.normalizeInput(descriptor.find(3,1), invalid.data(), invalid.size(), neutral));
    for (uint8_t id : {0xE9, 0xE8}) {
        const size_t index = descriptor.find(id,1);
        assert(descriptor.reports[index].bytes() == 255);
        assert(descriptor.reports[index].fields.empty());
        Bytes input(255);
        for (size_t i = 0; i < input.size(); ++i) input[i] = i % 2 ? 0xE9 : 0xEA;
        expect(descriptor, id, input, input, 0);
        assert(descriptor.normalizeInput(index, input.data(), input.size(), neutral));
        assert(Bytes(neutral, neutral + input.size()) == input);
        assert(!descriptor.normalizeInput(index, input.data(), input.size() - 1, neutral));
        std::memset(neutral, 0xA5, sizeof(neutral));
        assert(!descriptor.neutral(index, neutral));
        assert(Bytes(neutral, neutral + sizeof(neutral)) == Bytes(sizeof(neutral), 0xA5));
    }
}

static void nonVolumeArrayWithoutNeutral() {
    // A HOME/BACK array lacking a released selector, followed by a safely
    // filterable volume bit. Preserve the array; do not invent a full release.
    const Bytes map = {0x05,0x0C,0x09,1,0xA1,1,0x15,1,0x25,2,
                       0x0A,0x23,2,0x0A,0x24,2,0x75,8,0x95,1,0x81,0,
                       0x15,0,0x25,1,0x09,0xE9,0x75,1,0x81,2,
                       0x75,7,0x81,1,0xC0};
    Descriptor descriptor;
    assert(descriptor.parse(map.data(), map.size()));
    expect(descriptor, 0, {1,0xFF}, {1,0xFE}, 1);
    expect(descriptor, 0, {2,0}, {2,0}, 0);
    uint8_t neutral[hidbridge::MAX_REPORT_BYTES];
    assert(!descriptor.neutral(0, neutral));
}

static void rejectTruncatedVariableFields() {
    const Bytes map = {0x05,0x0C,0x09,1,0xA1,1,0x15,0,0x25,1,
                       0x09,0xE9,0x0A,0x23,2,0x75,8,0x95,2,0x81,2,0xC0};
    Descriptor descriptor;
    assert(descriptor.parse(map.data(), map.size()));
    uint8_t output[hidbridge::MAX_REPORT_BYTES];
    const uint8_t shortened[] = {1};
    assert(!descriptor.normalizeInput(0, shortened, sizeof(shortened), output));
}

static void rejectUnsafeDescriptors() {
    Descriptor descriptor;
    const Bytes noNeutral = {0x05,0x0C,0x09,1,0xA1,1,0x15,1,0x25,2,
                             0x09,0xE9,0x09,0xEA,0x75,8,0x95,1,0x81,0,0xC0};
    assert(!descriptor.parse(noNeutral.data(), noNeutral.size()));
    assert(!descriptor.error.empty() && descriptor.reports.empty());
    const Bytes truncated = {0x05,0x0C,0x26,0xFF};
    assert(!descriptor.parse(truncated.data(), truncated.size()));
    const Bytes longItem = {0xFE,0,0};
    assert(!descriptor.parse(longItem.data(), longItem.size()));
    const Bytes tooBig = {0x05,0x0C,0x09,1,0xA1,1,0x15,0,0x25,1,0x09,0xE9,
                          0x75,32,0x96,129,0,0x81,2,0xC0}; // 516 bytes.
    assert(!descriptor.parse(tooBig.data(), tooBig.size()));
    assert(descriptor.error.find("needs 516 bytes; limit=512") != std::string::npos);
    const Bytes mixedIds = {0x05,0x0C,0x09,1,0xA1,1,0x15,0,0x25,1,
                            0x75,1,0x95,1,0x09,0xE9,0x81,2,
                            0x85,1,0x09,0xEA,0x81,2,0xC0};
    assert(!descriptor.parse(mixedIds.data(), mixedIds.size()));
}

int main() {
    consumerArray();
    unalignedBitmapAndVendorPage();
    logicalSelectorsAndNull();
    keyboardAndPushPop();
    signedSelectorsAndExtendedUsages();
    largeRemoteReports();
    rc860gCapturedDescriptor();
    nonVolumeArrayWithoutNeutral();
    rejectTruncatedVariableFields();
    rejectUnsafeDescriptors();
    std::cout << "HID filter tests passed: exact RC860G descriptor, opaque report passthrough, volume interception, mixed keys, releases, vendor/keyboard isolation, 512-byte reports, report IDs, null selectors, and malformed maps.\n";
}
