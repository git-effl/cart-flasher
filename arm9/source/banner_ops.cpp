#include "banner_ops.h"

#include <cstring>
#include <new>

#include "device.h"
#include "nds_platform.h"

namespace banner_ops {
namespace {

using flashcart_core::Flashcart;
using flashcart_core::LOG_ERR;
using flashcart_core::LOG_NOTICE;
using flashcart_core::platform::logMessage;

enum class Profile {
    None,
    Ace3DSAl3e,
    R4iSdhc20xx,
};

enum class TargetFormat {
    Invalid,
    V1,
    V3,
};

constexpr std::uint32_t kAceMapSize = 0x4000;
constexpr std::uint32_t kAceHeaderVirtualOffset = 0x000000;
constexpr std::uint32_t kAceBannerOffset = 0xF5E00;
constexpr std::uint32_t kAceBannerVirtualOffset = 0x1A1E00;
constexpr std::uint32_t kAceBlockStart = 0xF5000;
constexpr std::uint32_t kAceBlockSize = 0x1000;
constexpr std::uint32_t kAceBlockRange = kAceBlockSize * 2;
constexpr std::uint64_t kAceMapFingerprint = 0x23D20EA2938A4E50ULL;

constexpr std::uint32_t kR4HeaderOffset = 0x1F0000;
constexpr std::uint32_t kR4BannerOffset = 0x1A6600;
constexpr std::uint32_t kR4TargetBannerSize = 0xA40;
constexpr std::uint32_t kR4BlockStart = 0x1A6000;
constexpr std::uint32_t kR4BlockSize = 0x1000;
constexpr std::uint64_t kR4HeaderFingerprint = 0x67E10B79824109C0ULL;

Profile ProfileForCart(Flashcart *cart) {
    if (std::strcmp(cart->getShortName(), "Ace3DSPlus") == 0) {
        return Profile::Ace3DSAl3e;
    }
    if (std::strcmp(cart->getShortName(), "r4isdhc") == 0) {
        return Profile::R4iSdhc20xx;
    }
    return Profile::None;
}

bool ReadFlashQuiet(Flashcart *cart, std::uint32_t address,
                    std::uint32_t length, std::uint8_t *buffer) {
    SetDriverProgressSuppressed(true);
    const bool result = cart->readFlash(address, length, buffer);
    SetDriverProgressSuppressed(false);
    return result;
}

bool WriteFlashQuiet(Flashcart *cart, std::uint32_t address,
                     std::uint32_t length, const std::uint8_t *buffer) {
    SetDriverProgressSuppressed(true);
    const bool result = cart->writeFlash(address, length, buffer);
    SetDriverProgressSuppressed(false);
    return result;
}

std::uint16_t Crc16(const std::uint8_t *data, size_t size) {
    std::uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ ((crc & 1) ? 0xA001 : 0);
        }
    }
    return crc;
}

std::uint64_t Fingerprint(const std::uint8_t *data, size_t size) {
    std::uint64_t fingerprint = 0xCBF29CE484222325ULL;
    for (size_t i = 0; i < size; ++i) {
        fingerprint ^= data[i];
        fingerprint *= 0x100000001B3ULL;
    }
    return fingerprint;
}

std::uint32_t AceMapEntry(const std::uint8_t *map, std::uint32_t index) {
    return static_cast<std::uint32_t>(map[index * 2])
        | (static_cast<std::uint32_t>(map[index * 2 + 1]) << 8);
}

bool ResolveAceLayout(Flashcart *cart) {
    if (cart->getFlashCapacityCode() != 0x15) {
        return false;
    }

    std::uint8_t *map = new(std::nothrow) std::uint8_t[kAceMapSize];
    if (!map) {
        logMessage(LOG_ERR, "Ace3DSPlus banner: page-map buffer unavailable");
        return false;
    }
    if (!ReadFlashQuiet(cart, 0, kAceMapSize, map)) {
        delete[] map;
        logMessage(LOG_NOTICE, "Ace3DSPlus banner: page-map read failed; option disabled");
        return false;
    }
    if (Fingerprint(map, kAceMapSize) != kAceMapFingerprint) {
        delete[] map;
        logMessage(LOG_NOTICE,
            "Ace3DSPlus banner: page map is not the known AL3E layout");
        return false;
    }

    const std::uint32_t headerPage = AceMapEntry(map, kAceHeaderVirtualOffset >> 12);
    const std::uint32_t headerOffset = headerPage << 12;
    const std::uint32_t bannerPage = AceMapEntry(map, kAceBannerVirtualOffset >> 12);
    const std::uint32_t bannerOffset = (bannerPage << 12)
        | (kAceBannerVirtualOffset & 0xFFF);
    delete[] map;

    if (headerOffset != 0xA000 || bannerOffset != kAceBannerOffset) {
        logMessage(LOG_NOTICE,
            "Ace3DSPlus banner: page map resolves unexpected header or banner location");
        return false;
    }

    std::uint8_t header[0x200];
    if (!ReadFlashQuiet(cart, headerOffset, sizeof(header), header)) {
        logMessage(LOG_NOTICE, "Ace3DSPlus banner: target header read failed; option disabled");
        return false;
    }
    const std::uint32_t virtualBanner = static_cast<std::uint32_t>(header[0x68])
        | (static_cast<std::uint32_t>(header[0x69]) << 8)
        | (static_cast<std::uint32_t>(header[0x6A]) << 16)
        | (static_cast<std::uint32_t>(header[0x6B]) << 24);
    if (virtualBanner != kAceBannerVirtualOffset) {
        logMessage(LOG_NOTICE,
            "Ace3DSPlus banner: header has an unknown virtual banner pointer");
        return false;
    }
    return true;
}

bool IsR4Header(const std::uint8_t *header, size_t size) {
    if (!header || size != 0x200
        || std::memcmp(header, "BOMBERMANLND", 12) != 0
        || std::memcmp(header + 0x0C, "ABXK", 4) != 0
        || std::memcmp(header + 0x10, "01", 2) != 0) {
        return false;
    }
    const std::uint16_t expectedCrc = static_cast<std::uint16_t>(header[0x15E])
        | (static_cast<std::uint16_t>(header[0x15F]) << 8);
    return Crc16(header, 0x15E) == expectedCrc
        && Fingerprint(header, size) == kR4HeaderFingerprint;
}

bool IsTargetV1(const std::uint8_t *banner, size_t size) {
    if (!banner || size < kSourceBannerSize
        || banner[0] != 0x01 || banner[1] != 0x00) {
        return false;
    }
    const std::uint16_t expectedCrc = static_cast<std::uint16_t>(banner[2])
        | (static_cast<std::uint16_t>(banner[3]) << 8);
    return Crc16(banner + 0x20, kSourceBannerSize - 0x20) == expectedCrc;
}

bool IsTargetV3(const std::uint8_t *banner, size_t size) {
    if (!banner || size != kR4TargetBannerSize
        || banner[0] != 0x03 || banner[1] != 0x00) {
        return false;
    }
    const std::uint16_t crcV1 = static_cast<std::uint16_t>(banner[2])
        | (static_cast<std::uint16_t>(banner[3]) << 8);
    const std::uint16_t crcV2 = static_cast<std::uint16_t>(banner[4])
        | (static_cast<std::uint16_t>(banner[5]) << 8);
    const std::uint16_t crcV3 = static_cast<std::uint16_t>(banner[6])
        | (static_cast<std::uint16_t>(banner[7]) << 8);
    return Crc16(banner + 0x20, 0x840 - 0x20) == crcV1
        && Crc16(banner + 0x20, 0x940 - 0x20) == crcV2
        && Crc16(banner + 0x20, 0xA40 - 0x20) == crcV3;
}

TargetFormat R4TargetFormat(const std::uint8_t *banner, size_t size) {
    if (IsTargetV3(banner, size)) {
        return TargetFormat::V3;
    }
    if (IsTargetV1(banner, size)) {
        return TargetFormat::V1;
    }
    return TargetFormat::Invalid;
}

const char *R4TargetFailure(const std::uint8_t *banner, size_t size) {
    if (!banner || size < kSourceBannerSize) {
        return "banner record is too short";
    }
    if (banner[0] == 0x01 && banner[1] == 0x00) {
        return "v1 banner checksum is invalid";
    }
    if (banner[0] != 0x03 || banner[1] != 0x00) {
        return "banner version is neither v3 nor v1";
    }
    if (size != kR4TargetBannerSize) {
        return "v3 banner record has the wrong size";
    }
    const std::uint16_t crcV1 = static_cast<std::uint16_t>(banner[2])
        | (static_cast<std::uint16_t>(banner[3]) << 8);
    const std::uint16_t crcV2 = static_cast<std::uint16_t>(banner[4])
        | (static_cast<std::uint16_t>(banner[5]) << 8);
    const std::uint16_t crcV3 = static_cast<std::uint16_t>(banner[6])
        | (static_cast<std::uint16_t>(banner[7]) << 8);
    if (Crc16(banner + 0x20, 0x840 - 0x20) != crcV1) {
        return "v3 primary checksum is invalid";
    }
    if (Crc16(banner + 0x20, 0x940 - 0x20) != crcV2) {
        return "v3 Chinese checksum is invalid";
    }
    if (Crc16(banner + 0x20, 0xA40 - 0x20) != crcV3) {
        return "v3 Korean checksum is invalid";
    }
    return nullptr;
}

bool ValidateAceTarget(Flashcart *cart, bool announce) {
    if (!ResolveAceLayout(cart)) {
        return false;
    }
    std::uint8_t banner[kSourceBannerSize];
    if (!ReadFlashQuiet(cart, kAceBannerOffset, sizeof(banner), banner)) {
        logMessage(LOG_NOTICE, "Ace3DSPlus banner: target banner read failed; option disabled");
        return false;
    }
    const SourceValidation validation = ValidateSourceBanner(banner, sizeof(banner));
    if (validation != SourceValidation::Valid) {
        const char *reason = validation == SourceValidation::WrongSize
            ? "banner record is not 2,112 bytes"
            : validation == SourceValidation::WrongVersion
                ? "banner version is not Regular DS v1"
                : "banner checksum is invalid";
        logMessage(LOG_NOTICE, "Ace3DSPlus banner: %s; option disabled", reason);
        return false;
    }
    if (announce) {
        logMessage(LOG_NOTICE, "Ace3DSPlus banner: target geometry verified");
    }
    return true;
}

bool ValidateR4Target(Flashcart *cart, TargetFormat *format, bool announce) {
    std::uint8_t header[0x200];
    if (!ReadFlashQuiet(cart, kR4HeaderOffset, sizeof(header), header)) {
        logMessage(LOG_NOTICE, "r4isdhc banner: target header read failed; option disabled");
        return false;
    }
    if (!IsR4Header(header, sizeof(header))) {
        logMessage(LOG_NOTICE, "r4isdhc banner: target is not the known 20XX layout");
        return false;
    }
    std::uint8_t banner[kR4TargetBannerSize];
    if (!ReadFlashQuiet(cart, kR4BannerOffset, sizeof(banner), banner)) {
        logMessage(LOG_NOTICE, "r4isdhc banner: target banner read failed; option disabled");
        return false;
    }
    *format = R4TargetFormat(banner, sizeof(banner));
    if (*format == TargetFormat::Invalid) {
        logMessage(LOG_NOTICE, "r4isdhc banner: %s; option disabled",
            R4TargetFailure(banner, sizeof(banner)));
        return false;
    }
    if (announce) {
        logMessage(LOG_NOTICE, "r4isdhc banner: 20XX target geometry verified (current v%d)",
            *format == TargetFormat::V3 ? 3 : 1);
    }
    return true;
}

} // namespace

SourceValidation ValidateSourceBanner(const std::uint8_t *banner, size_t size) {
    if (!banner || size != kSourceBannerSize) {
        return SourceValidation::WrongSize;
    }
    if (banner[0] != 0x01 || banner[1] != 0x00) {
        return SourceValidation::WrongVersion;
    }
    const std::uint16_t expectedCrc = static_cast<std::uint16_t>(banner[2])
        | (static_cast<std::uint16_t>(banner[3]) << 8);
    return Crc16(banner + 0x20, size - 0x20) == expectedCrc
        ? SourceValidation::Valid : SourceValidation::WrongCrc;
}

bool HasAvailableOperation(Flashcart *cart) {
    switch (ProfileForCart(cart)) {
        case Profile::Ace3DSAl3e:
            return ValidateAceTarget(cart, true);
        case Profile::R4iSdhc20xx: {
            TargetFormat format;
            return ValidateR4Target(cart, &format, true);
        }
        default:
            return false;
    }
}

bool ReadBanner(Flashcart *cart, std::uint8_t *banner, std::uint32_t bannerSize) {
    if (bannerSize != kSourceBannerSize) {
        return false;
    }
    switch (ProfileForCart(cart)) {
        case Profile::Ace3DSAl3e:
            if (!ValidateAceTarget(cart, false)
                || !ReadFlashQuiet(cart, kAceBannerOffset, bannerSize, banner)) {
                return false;
            }
            logMessage(LOG_NOTICE, "Ace3DSPlus banner: exported current v1 banner");
            return true;
        case Profile::R4iSdhc20xx: {
            TargetFormat format;
            if (!ValidateR4Target(cart, &format, false)) {
                return false;
            }
            std::uint8_t target[kR4TargetBannerSize];
            if (!ReadFlashQuiet(cart, kR4BannerOffset, sizeof(target), target)) {
                return false;
            }
            std::memcpy(banner, target, bannerSize);
            if (format == TargetFormat::V3) {
                banner[0] = 0x01;
                banner[1] = 0x00;
            }
            if (ValidateSourceBanner(banner, bannerSize) != SourceValidation::Valid) {
                return false;
            }
            logMessage(LOG_NOTICE, "r4isdhc banner: exported current v%d banner as v1",
                format == TargetFormat::V3 ? 3 : 1);
            return true;
        }
        default:
            return false;
    }
}

bool WriteBanner(Flashcart *cart, const std::uint8_t *banner,
                 std::uint32_t bannerSize) {
    if (ValidateSourceBanner(banner, bannerSize) != SourceValidation::Valid) {
        return false;
    }
    switch (ProfileForCart(cart)) {
        case Profile::Ace3DSAl3e: {
            if (!ValidateAceTarget(cart, false)) {
                return false;
            }
            std::uint8_t *expected = new(std::nothrow) std::uint8_t[kAceBlockRange];
            std::uint8_t *verified = new(std::nothrow) std::uint8_t[kAceBlockRange];
            if (!expected || !verified) {
                delete[] expected;
                delete[] verified;
                logMessage(LOG_ERR, "Ace3DSPlus banner: verification buffers unavailable");
                return false;
            }
            if (!ReadFlashQuiet(cart, kAceBlockStart, kAceBlockRange, expected)) {
                delete[] expected;
                delete[] verified;
                return false;
            }
            const std::uint32_t offset = kAceBannerOffset - kAceBlockStart;
            std::memcpy(expected + offset, banner, bannerSize);
            logMessage(LOG_NOTICE, "Ace3DSPlus banner: writing blocks %08lX-%08lX",
                static_cast<unsigned long>(kAceBlockStart),
                static_cast<unsigned long>(kAceBlockStart + kAceBlockRange - 1));
            const bool written = WriteFlashQuiet(cart, kAceBlockStart, kAceBlockRange,
                expected);
            const bool verifiedOk = written
                && ReadFlashQuiet(cart, kAceBlockStart, kAceBlockRange, verified)
                && std::memcmp(expected, verified, kAceBlockRange) == 0;
            delete[] expected;
            delete[] verified;
            if (!verifiedOk) {
                logMessage(LOG_ERR, "Ace3DSPlus banner: full erase-block verification failed");
                return false;
            }
            logMessage(LOG_NOTICE, "Ace3DSPlus banner: write verified");
            return true;
        }
        case Profile::R4iSdhc20xx: {
            TargetFormat format;
            if (!ValidateR4Target(cart, &format, false)) {
                return false;
            }
            const std::uint32_t affectedSize = format == TargetFormat::V3
                ? kR4BlockSize * 2 : kR4BlockSize;
            std::uint8_t *expected = new(std::nothrow) std::uint8_t[affectedSize];
            std::uint8_t *verified = new(std::nothrow) std::uint8_t[affectedSize];
            if (!expected || !verified) {
                delete[] expected;
                delete[] verified;
                logMessage(LOG_ERR, "r4isdhc banner: verification buffers unavailable");
                return false;
            }
            if (!ReadFlashQuiet(cart, kR4BlockStart, affectedSize, expected)) {
                delete[] expected;
                delete[] verified;
                return false;
            }
            const std::uint32_t offset = kR4BannerOffset - kR4BlockStart;
            std::memcpy(expected + offset, banner, bannerSize);
            if (format == TargetFormat::V3) {
                std::memset(expected + offset + bannerSize, 0,
                    kR4TargetBannerSize - bannerSize);
                logMessage(LOG_NOTICE,
                    "r4isdhc banner: writing blocks %08lX-%08lX (v3 to v1; clearing 512-byte extension)",
                    static_cast<unsigned long>(kR4BlockStart),
                    static_cast<unsigned long>(kR4BlockStart + affectedSize - 1));
            } else {
                logMessage(LOG_NOTICE,
                    "r4isdhc banner: writing block %08lX-%08lX (v1 to v1)",
                    static_cast<unsigned long>(kR4BlockStart),
                    static_cast<unsigned long>(kR4BlockStart + affectedSize - 1));
            }
            const bool written = WriteFlashQuiet(cart, kR4BlockStart, affectedSize,
                expected);
            const bool verifiedOk = written
                && ReadFlashQuiet(cart, kR4BlockStart, affectedSize, verified)
                && std::memcmp(expected, verified, affectedSize) == 0;
            delete[] expected;
            delete[] verified;
            if (!verifiedOk) {
                logMessage(LOG_ERR, "r4isdhc banner: full erase-block verification failed");
                return false;
            }
            logMessage(LOG_NOTICE, "r4isdhc banner: write verified");
            return true;
        }
        default:
            return false;
    }
}

} // namespace banner_ops
