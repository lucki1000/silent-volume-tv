#pragma once

// Descriptor-driven HID input filtering. This file has no Arduino dependency,
// so the same parser and bit manipulation can be tested with a host compiler.
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace hidbridge {
// Match NimBLE's maximum GATT attribute value length, including large vendor
// input/feature reports advertised by otherwise ordinary TV remotes.
constexpr size_t MAX_REPORT_BYTES = 512;
constexpr size_t MAX_REPORTS = 16;
constexpr size_t MAX_FIELDS = 1024;
constexpr uint32_t VOLUME_UP = 0x000C00E9;
constexpr uint32_t VOLUME_DOWN = 0x000C00EA;

struct UsageRange { uint32_t first, last; };
struct Field {
    uint16_t offset = 0;
    uint8_t width = 0;
    bool variable = false;
    bool relative = false;
    int64_t logicalMin = 0, logicalMax = 0;
    uint32_t usage = 0, neutral = 0;
    std::vector<UsageRange> usages;
};
struct Report {
    uint8_t id = 0, type = 1; // Report Reference types: input=1, output=2, feature=3.
    uint16_t bits = 0;
    bool canNeutralize = true; // False if any input data has no defined release value.
    std::vector<Field> fields;
    size_t bytes() const { return (bits + 7u) / 8u; }
};

inline uint32_t readBits(const uint8_t* data, size_t offset, uint8_t width) {
    uint32_t result = 0;
    for (uint8_t bit = 0; bit < width; ++bit) {
        result |= uint32_t((data[(offset + bit) / 8] >> ((offset + bit) % 8)) & 1u) << bit;
    }
    return result;
}
inline void writeBits(uint8_t* data, size_t offset, uint8_t width, uint32_t value) {
    for (uint8_t bit = 0; bit < width; ++bit) {
        const size_t byte = (offset + bit) / 8;
        const uint8_t mask = uint8_t(1u << ((offset + bit) % 8));
        data[byte] = uint8_t((data[byte] & ~mask) | (((value >> bit) & 1u) ? mask : 0));
    }
}
inline int64_t decodeValue(uint32_t value, uint8_t width, bool isSigned) {
    if (isSigned && (value & (uint32_t(1) << (width - 1)))) {
        return int64_t(value) - (int64_t(1) << width);
    }
    return value;
}
inline bool fitsValue(int64_t value, uint8_t width, bool isSigned) {
    const int64_t minimum = isSigned ? -(int64_t(1) << (width - 1)) : 0;
    const int64_t maximum = isSigned ? (int64_t(1) << (width - 1)) - 1 : (int64_t(1) << width) - 1;
    return value >= minimum && value <= maximum;
}
inline uint32_t usageAt(const std::vector<UsageRange>& ranges, uint64_t index) {
    for (const auto& range : ranges) {
        const uint64_t count = uint64_t(range.last) - range.first + 1;
        if (index < count) return range.first + uint32_t(index);
        index -= count;
    }
    return UINT32_MAX;
}
inline uint64_t usageCount(const std::vector<UsageRange>& ranges) {
    uint64_t count = 0;
    for (const auto& range : ranges) count += uint64_t(range.last) - range.first + 1;
    return count;
}
inline bool findUsage(const Field& field, uint32_t usage, int64_t& selector) {
    uint64_t index = 0;
    for (const auto& range : field.usages) {
        if (usage >= range.first && usage <= range.last) {
            selector = field.logicalMin + int64_t(index + usage - range.first);
            return selector <= field.logicalMax && fitsValue(selector, field.width, field.logicalMin < 0);
        }
        index += uint64_t(range.last) - range.first + 1;
    }
    return false;
}
inline uint8_t volumeBit(uint32_t usage) {
    return usage == VOLUME_UP ? 1 : usage == VOLUME_DOWN ? 2 : 0;
}

class Descriptor {
public:
    std::vector<Report> reports;
    std::string error;

    bool parse(const uint8_t* map, size_t size) {
        reports.clear();
        error.clear();
        struct Globals {
            uint32_t page = 0, width = 0, count = 0;
            int64_t minimum = 0, maximum = 0;
            uint8_t id = 0;
        } global;
        std::vector<Globals> stack;
        std::vector<UsageRange> usages;
        uint32_t rangeMinimum = 0;
        bool pendingRange = false, hasIds = false;
        size_t fields = 0;
        int collections = 0;
        for (size_t cursor = 0; cursor < size;) {
            const uint8_t prefix = map[cursor++];
            if (prefix == 0xFE) return fail("HID long items are unsupported");
            const size_t itemSize = (prefix & 3u) == 3 ? 4 : (prefix & 3u);
            if (cursor + itemSize > size) return fail("Truncated HID item");
            uint32_t value = 0;
            for (size_t i = 0; i < itemSize; ++i) value |= uint32_t(map[cursor++]) << (8 * i);
            const uint8_t kind = (prefix >> 2) & 3, tag = prefix >> 4;
            const int64_t signedValue = itemSize ? decodeValue(value, uint8_t(itemSize * 8), true) : 0;
            if (kind == 1) {
                switch (tag) {
                    case 0:
                        if (value > 0xFFFF) return fail("Usage page exceeds 16 bits");
                        global.page = value; break;
                    case 1: global.minimum = signedValue; break;
                    case 2: global.maximum = global.minimum < 0 ? signedValue : int64_t(value); break;
                    case 7: global.width = value; break;
                    case 8:
                        if (value == 0 || value > 255) return fail("Invalid report ID");
                        global.id = uint8_t(value); hasIds = true; break;
                    case 9: global.count = value; break;
                    case 10:
                        if (stack.size() == 8) return fail("HID global stack too deep");
                        stack.push_back(global); break;
                    case 11:
                        if (stack.empty()) return fail("HID global stack underflow");
                        global = stack.back(); stack.pop_back(); break;
                    default: break; // Physical range, units, etc. do not affect bit layout.
                }
            } else if (kind == 2) {
                const uint32_t usage = itemSize == 4 ? value : (global.page << 16) | value;
                switch (tag) {
                    case 0:
                        if (pendingRange) return fail("Incomplete usage range");
                        usages.push_back({usage, usage}); break;
                    case 1:
                        if (pendingRange) return fail("Nested usage ranges");
                        rangeMinimum = usage; pendingRange = true; break;
                    case 2:
                        if (!pendingRange || usage < rangeMinimum || (usage >> 16) != (rangeMinimum >> 16))
                            return fail("Invalid usage range");
                        usages.push_back({rangeMinimum, usage}); pendingRange = false; break;
                    case 10: return fail("HID usage delimiters are unsupported");
                    default: break;
                }
                if (usages.size() > 64) return fail("Too many local usages");
            } else if (kind == 0) {
                if (pendingRange) return fail("Incomplete usage range");
                if (tag == 10) ++collections;
                else if (tag == 12) {
                    if (--collections < 0) return fail("Unbalanced HID collection");
                } else if (tag == 8 || tag == 9 || tag == 11) {
                    const uint8_t type = tag == 8 ? 1 : tag == 9 ? 2 : 3;
                    if (!global.width || global.width > 32 || !global.count)
                        return fail("HID field width/count unsupported");
                    if (global.minimum > global.maximum) return fail("Invalid logical range");
                    if ((value & 0x100u) != 0) return fail("Buffered-byte HID fields are unsupported");
                    size_t index = find(global.id, type);
                    if (index == reports.size()) {
                        if (reports.size() == MAX_REPORTS) return fail("Too many HID reports");
                        Report report;
                        report.id = global.id; report.type = type;
                        reports.push_back(report);
                    }
                    Report& report = reports[index];
                    const uint64_t addedBits = uint64_t(global.width) * global.count;
                    if (addedBits + report.bits > MAX_REPORT_BYTES * 8)
                        return fail("HID report id=" + std::to_string(global.id) +
                                    " type=" + std::to_string(type) + " needs " +
                                    std::to_string((addedBits + report.bits + 7) / 8) +
                                    " bytes; limit=" + std::to_string(MAX_REPORT_BYTES));
                    if (type == 1 && (value & 1u) == 0) { // Input data, excluding constant padding.
                        // Some remotes declare bulk input reports with no local
                        // field usages at all (RC860G IDs E9/E8). Their report
                        // IDs are not usages. Copy the payload, and never invent
                        // a released/neutral value for these unknown fields.
                        if (usages.empty()) report.canNeutralize = false;
                        for (uint32_t i = 0; i < global.count && !usages.empty(); ++i) {
                            Field field;
                            field.offset = uint16_t(report.bits + i * global.width);
                            field.width = uint8_t(global.width);
                            field.variable = (value & 2u) != 0;
                            field.relative = (value & 4u) != 0;
                            field.logicalMin = global.minimum; field.logicalMax = global.maximum;
                            if (field.variable) {
                                const uint64_t count = usageCount(usages);
                                field.usage = count ? usageAt(usages, i < count ? i : count - 1) : 0;
                                if (volumeBit(field.usage) && (global.minimum > 0 || global.maximum < 0))
                                    return fail("Volume variable has no zero/released value");
                            } else {
                                field.usages = usages;
                                if (!arrayNeutral(field, (value & 0x40u) != 0)) {
                                    int64_t selector;
                                    if (findUsage(field, VOLUME_UP, selector) || findUsage(field, VOLUME_DOWN, selector))
                                        return fail("Volume array has no safe no-key selector (usage 0 or Null State required)");
                                    // Non-volume arrays can be forwarded without
                                    // rewriting them. A synthetic release cannot
                                    // safely be generated for this report.
                                    report.canNeutralize = false;
                                    continue;
                                }
                            }
                            if (++fields > MAX_FIELDS) return fail("Too many input fields");
                            report.fields.push_back(field);
                        }
                    }
                    report.bits += uint16_t(addedBits);
                }
                usages.clear();
            } else {
                return fail("Reserved HID item type");
            }
        }
        if (collections || !stack.empty() || pendingRange) return fail("Incomplete HID descriptor");
        bool hasInput = false;
        for (const auto& report : reports) {
            hasInput |= report.type == 1;
            if (hasIds && report.id == 0) return fail("Mixed numbered and unnumbered reports");
        }
        return hasInput || fail("No HID input reports");
    }

    size_t find(uint8_t id, uint8_t type) const {
        for (size_t i = 0; i < reports.size(); ++i)
            if (reports[i].id == id && reports[i].type == type) return i;
        return reports.size();
    }

    // RC860G declares two 16-bit selectors but sends just one when only one
    // key is down. Complete ONLY omitted whole array selectors with their
    // declared no-key value; never guess a partial selector or variable field.
    // output must hold reports[index].bytes() bytes. Full reports stay identical.
    bool normalizeInput(size_t index, const uint8_t* input, size_t size, uint8_t* output) const {
        if (index >= reports.size() || reports[index].type != 1) return false;
        const Report& report = reports[index];
        if (size == report.bytes()) {
            std::memcpy(output, input, size);
            return true;
        }
        if (!size || size > report.bytes() || !report.canNeutralize) return false;
        const size_t receivedBits = size * 8;
        bool omittedArray = false;
        for (const auto& field : report.fields) {
            if (field.offset < receivedBits && size_t(field.offset) + field.width > receivedBits) return false;
            if (field.offset >= receivedBits) {
                if (field.variable) return false;
                omittedArray = true;
            }
        }
        if (!omittedArray || !neutral(index, output)) return false;
        std::memcpy(output, input, size);
        return true;
    }

    // Copy the entire report, changing only the bit fields holding the two
    // Consumer volume usages. Never search arbitrary bytes for E9 or EA.
    bool filter(size_t index, const uint8_t* input, size_t size, uint8_t* output, uint8_t& volume) const {
        if (index >= reports.size() || reports[index].type != 1 || size != reports[index].bytes()) return false;
        std::memcpy(output, input, size);
        volume = 0;
        for (const auto& field : reports[index].fields) {
            const uint32_t raw = readBits(input, field.offset, field.width);
            const int64_t selector = decodeValue(raw, field.width, field.logicalMin < 0);
            uint32_t usage = field.usage;
            if (!field.variable) {
                if (selector < field.logicalMin || selector > field.logicalMax) continue;
                usage = usageAt(field.usages, uint64_t(selector - field.logicalMin));
            }
            const uint8_t bit = volumeBit(usage);
            if (bit && (!field.variable || selector != 0)) {
                volume |= bit;
                writeBits(output, field.offset, field.width, field.variable ? 0 : field.neutral);
            }
        }
        return true;
    }

    bool neutral(size_t index, uint8_t* output) const {
        const Report& report = reports[index];
        if (!report.canNeutralize) return false;
        std::memset(output, 0, report.bytes());
        for (const auto& field : report.fields)
            if (!field.variable) writeBits(output, field.offset, field.width, field.neutral);
        return true;
    }

private:
    bool fail(const std::string& message) { error = message; reports.clear(); return false; }
    static bool arrayNeutral(Field& field, bool nullState) {
        uint64_t baseIndex = 0;
        for (const auto& range : field.usages) {
            const uint32_t zeroUsage = range.first & 0xFFFF0000u;
            if (zeroUsage >= range.first && zeroUsage <= range.last) {
                const int64_t selector = field.logicalMin + int64_t(baseIndex + zeroUsage - range.first);
                if (selector <= field.logicalMax && fitsValue(selector, field.width, field.logicalMin < 0)) {
                    field.neutral = uint32_t(selector); return true;
                }
            }
            baseIndex += uint64_t(range.last) - range.first + 1;
        }
        if (nullState) {
            int64_t selector = field.logicalMax + 1;
            if (!fitsValue(selector, field.width, field.logicalMin < 0)) selector = field.logicalMin - 1;
            if (fitsValue(selector, field.width, field.logicalMin < 0)) {
                field.neutral = uint32_t(selector); return true;
            }
        }
        return false;
    }
};
} // namespace hidbridge
