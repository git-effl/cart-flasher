#include <cstring>
#include <algorithm>
#include <new>
#include <ncgcpp/ntrcard.h>

#include "../device.h"
#include "../flash_util.h"

namespace flashcart_core {
using platform::logMessage;
using platform::showProgress;

namespace {
union CmdBuf4 {
        uint32_t u32;
        uint8_t u8[4];
};
static_assert(sizeof(CmdBuf4) == 4, "Wrong union size");

constexpr uint64_t norCmd(const uint8_t outlen, const uint8_t inlen, const uint8_t cmd, const uint32_t addr, const uint8_t d1 = 0, const uint8_t d2 = 0) {
    // DD DD AA AA AA CC XY 99
    return 0x99ull | ((outlen & 0xFull) << 12) | ((inlen & 0xFull) << 8) |
        (cmd << 16) |
        ((addr & 0xFF0000ull) << 8) | ((addr & 0xFF00ull) << 24) | ((addr & 0xFFull) << 40) |
        (static_cast<uint64_t>(d1) << 48) | (static_cast<uint64_t>(d2) << 56);
}
static_assert(norCmd(2, 5, 0x3B, 0xABCDEF, 0x12, 0x34) == 0x3412EFCDAB3B2599, "norCmd result is wrong");

// i have no idea what else to name this...
constexpr uint64_t norRaw(const uint8_t d1, const uint8_t d2, const uint8_t fpgaParam = 0) {
    // 00 00 00 00 D2 D1 XY 99
    return 0x99ull | (static_cast<uint64_t>(fpgaParam) << 8) |
        (static_cast<uint64_t>(d1) << 16) | (static_cast<uint64_t>(d2) << 24);
}
static_assert(norRaw(0x34, 0x56, 0x12) == 0x56341299, "norRaw result is wrong");
}

class R4iSDHC : Flashcart {
    enum class CartType1Check {
        Match,
        NoMatch,
        Error,
    };

    bool norRead(const uint32_t address, uint32_t *result) {
        CmdBuf4 buf = {};
        const ncgc::Err err = m_card->sendCommand(norCmd(2, 5, 0x3B, address), buf.u8, 4, 0x180000);
        if (err) {
            logMessage(LOG_ERR, "r4isdhc: NOR read at %X failed: %d", address, err.errNo());
            return false;
        }

        logMessage(LOG_DEBUG, "R4ISDHC: NOR read at %X returned %X", address, buf.u32);
        *result = buf.u32;
        return true;
    }

    bool norRead(uint32_t addr, uint32_t size, void *dest) {
        uint32_t res;
        if (!norRead(addr, &res)) {
            return false;
        }
        std::memcpy(dest, &res, std::min<uint32_t>(size, 4));

        return true;
    }

    bool norWriteEnable() {
        const ncgc::Err err = m_card->sendCommand(norCmd(0, 1, 6, 0), nullptr, 4, 0x180000);
        if (err) {
            logMessage(LOG_ERR, "r4isdhc: NOR write enable failed: %d", err.errNo());
            return false;
        }
        ncgc::delay(0x60000);
        return true;
    }

    bool norErase4k(const uint32_t address) {
        if (!norWriteEnable()) {
            return false;
        }
        const ncgc::Err err = m_card->sendCommand(norCmd(0, 4, 0x20, address), nullptr, 4, 0x180000);
        if (err) {
            logMessage(LOG_ERR, "r4isdhc: NOR erase at %X failed: %d", address, err.errNo());
            return false;
        }
        ncgc::delay(41000000);

        // now ideally if i could read the NOR status register, i'd do the memcpy here
        // while the NOR does the sector erase, then just wait on it at the end. BUT NOPE!
        // (and the datasheet doesn't say this chip can read while writing, so that's not an option)

        bool success = false;
        uint32_t retry = 0;
        while (retry < 10) {
            // some sanity checks..
            uint32_t first;
            uint32_t last;
            if (!norRead(address, &first) || !norRead(address + 0x1000 - 4, &last)) {
                return false;
            }
            success = first == 0xFFFFFFFF && last == 0xFFFFFFFF;
            if (success) {
                break;
            }

            ++retry;
            logMessage(LOG_WARN, "r4isdhc: norErase4k: start or end isn't FF");
            ncgc::delay(41000000);
        }

        if (!success) {
            logMessage(LOG_ERR, "r4isdhc: NOR erase at %X did not verify", address);
        }
        return success;
    }

    bool norWrite256(const uint32_t address, const void *src) {
        const uint8_t *bytes = static_cast<const uint8_t *>(src);
        if (!norWriteEnable()) {
            return false;
        }
        ncgc::Err err = m_card->sendCommand(norCmd(0, 6, 2, address, bytes[0], bytes[1]), nullptr, 4, 0x180000);
        if (err) {
            logMessage(LOG_ERR, "r4isdhc: NOR write at %X failed: %d", address, err.errNo());
            return false;
        }
        for (uint32_t cur = 2; cur < 0x100; cur += 2) {
            if ((err = m_card->sendCommand(norRaw(bytes[cur], bytes[cur+1]), nullptr, 4, 0x180000))) {
                logMessage(LOG_ERR, "r4isdhc: NOR write at %X failed: %d", address + cur, err.errNo());
                return false;
            }
        }
        if ((err = m_card->sendCommand(norRaw(bytes[0], bytes[1], 0xF0), nullptr, 4, 0x180000))) {
            logMessage(LOG_ERR, "r4isdhc: NOR write commit at %X failed: %d", address, err.errNo());
            return false;
        }
        ncgc::delay(0x60000);

        return true;
    }

    CartType1Check checkCartType1() {
        CmdBuf4 buf = {};
        // this is actually the NOR write disable command
        // the r4isdhc will respond to cart commands with 0xFFFFFFFF if
        // the "magic" command hasn't been sent, so we check for that
        ncgc::Err err = m_card->sendCommand(0x40199, buf.u8, 4, 0x180000);
        if (err) {
            logMessage(LOG_ERR, "r4isdhc: checkCartType1: pre-test failed: %d", err.errNo());
            return CartType1Check::Error;
        }
        if (m_card->state() == ncgc::NTRState::Raw) {
            if (buf.u32 != 0xFFFFFFFF) {
                logMessage(LOG_ERR, "r4isdhc: checkCartType1: pre-test returned 0x%08X", buf.u32);
                return CartType1Check::NoMatch;
            }
        }

        err = m_card->init();
        if (err && !err.unsupported()) {
            logMessage(LOG_ERR, "r4isdhc: checkCartType1: ntrcard::init failed: %d", err.errNo());
            return CartType1Check::Error;
        }

        // only type 1 carts support 0x68 command
        if ((err = m_card->sendCommand(0x68, nullptr, 4, 0x180000, true))) {
            logMessage(LOG_ERR, "r4isdhc: checkCartType1: magic command failed: %d", err.errNo());
            return CartType1Check::Error;
        }

        // now it will return zeroes
        if ((err = m_card->sendCommand(0x40199, buf.u8, 4, 0x180000, true))) {
            logMessage(LOG_ERR, "r4isdhc: checkCartType1: post-test failed: %d", err.errNo());
            return CartType1Check::Error;
        }
        if (buf.u32 == 0) {
            m_card->state(ncgc::NTRState::Raw);
            return CartType1Check::Match;
        }

        logMessage(LOG_ERR, "r4isdhc: checkCartType1: post-test returned 0x%08X", buf.u32);
        return CartType1Check::NoMatch;
    }

    bool checkCartType2() {
        // this check only work on the activated BF key2
        if (m_card->state() != ncgc::NTRState::Key2) {
            logMessage(LOG_ERR, "r4isdhc: checkCartType2: status (%d) not KEY2",
                static_cast<uint32_t>(m_card->state()));
            return false;
        }

        CmdBuf4 buf = {};
        ncgc::Err err = m_card->sendCommand(0x66, nullptr, 4, 0x586000, true);
        if (err) {
            logMessage(LOG_ERR, "r4isdhc: checkCartType2: magic command failed: %d", err.errNo());
            return false;
        }
        if ((err = m_card->sendCommand(0x40199, buf.u8, 4, 0x180000, true))) {
            logMessage(LOG_ERR, "r4isdhc: checkCartType2: post-test failed: %d", err.errNo());
            return false;
        }

        // FIXME this is a really poor check
        // a non-r4isdhc cart will stay in KEY2 and likely return something that isn't all-FF
        if (buf.u32 != 0xFFFFFFFF) {
            m_card->state(ncgc::NTRState::Raw);
            return true;
        }

        logMessage(LOG_ERR, "r4isdhc: checkCartType2: post-test returned 0x%08X", buf.u32);
        // i'm not sure if there's any point to this
        // the encryption state's probably desynced if the previous command returned 0xFFFFFFFF
        return false;
    }

    bool trySecureInit(BlowfishKey key) {
        ncgc::Err err = m_card->init();
        if (err && !err.unsupported()) {
            logMessage(LOG_ERR, "r4isdhc: trySecureInit: ntrcard::init failed");
            return false;
        } else if (m_card->state() != ncgc::NTRState::Raw) {
            logMessage(LOG_ERR, "r4isdhc: trySecureInit: status (%d) not RAW and cannot reset",
                static_cast<uint32_t>(m_card->state()));
            return false;
        }

        ncgc::c::ncgc_ncard_t& state = m_card->rawState();
        state.hdr.key1_romcnt = state.key1.romcnt = 0x81808F8;
        state.hdr.key2_romcnt = state.key2.romcnt = 0x416657;
        state.key2.seed_byte = 0;
        m_card->setBlowfishState(platform::getBlowfishKey(key), key != BlowfishKey::NTR);

        if ((err = m_card->beginKey1())) {
            logMessage(LOG_ERR, "r4isdhc: trySecureInit: init key1 (key = %d) failed: %d", static_cast<int>(key), err.errNo());
            return false;
        }
        if ((err = m_card->beginKey2())) {
            logMessage(LOG_ERR, "r4isdhc: trySecureInit: init key2 failed: %d", err.errNo());
            return false;
        }

        return checkCartType2();
    }

    uint8_t cart_type;
    bool m_bannerLayoutValidated;

    static const uint32_t kBannerHeaderOffset = 0x1F0000;
    static const uint32_t kBannerOffset = 0x1A6600;
    static const uint32_t kTargetBannerSize = 0xA40;
    static const uint32_t kSourceBannerSize = 0x840;
    static const uint32_t kBannerBlockStart = 0x1A6000;
    static const uint32_t kBannerBlockSize = 0x1000;
    static const std::uint64_t kHeaderFingerprint = 0x67E10B79824109C0ULL;
    static const BannerWriteProfile kBannerWriteProfile;

    static std::uint64_t headerFingerprint(const uint8_t *data, size_t size) {
        std::uint64_t fingerprint = 0xCBF29CE484222325ULL;
        for (size_t i = 0; i < size; ++i) {
            fingerprint ^= data[i];
            fingerprint *= 0x100000001B3ULL;
        }
        return fingerprint;
    }

    static uint16_t crc16(const uint8_t *data, size_t size) {
        uint16_t crc = 0xFFFF;
        for (size_t i = 0; i < size; ++i) {
            crc ^= data[i];
            for (int bit = 0; bit < 8; ++bit) {
                crc = (crc >> 1) ^ ((crc & 1) ? 0xA001 : 0);
            }
        }
        return crc;
    }

    static bool isExpectedHeader(const uint8_t *header, size_t size) {
        if (!header || size != 0x200
            || std::memcmp(header, "BOMBERMANLND", 12) != 0
            || std::memcmp(header + 0x0C, "ABXK", 4) != 0
            || std::memcmp(header + 0x10, "01", 2) != 0) {
            return false;
        }
        const uint16_t expectedCrc = static_cast<uint16_t>(header[0x15E])
            | (static_cast<uint16_t>(header[0x15F]) << 8);
        return crc16(header, 0x15E) == expectedCrc
            && headerFingerprint(header, size) == kHeaderFingerprint;
    }

    static bool isSourceBannerV1(const uint8_t *banner, size_t size) {
        if (!banner || size != kSourceBannerSize
            || banner[0] != 0x01 || banner[1] != 0x00) {
            return false;
        }
        const uint16_t expectedCrc = static_cast<uint16_t>(banner[2])
            | (static_cast<uint16_t>(banner[3]) << 8);
        return crc16(banner + 0x20, size - 0x20) == expectedCrc;
    }

    static bool isTargetBannerV1(const uint8_t *banner, size_t size) {
        if (!banner || size < kSourceBannerSize
            || banner[0] != 0x01 || banner[1] != 0x00) {
            return false;
        }
        const uint16_t expectedCrc = static_cast<uint16_t>(banner[2])
            | (static_cast<uint16_t>(banner[3]) << 8);
        return crc16(banner + 0x20, kSourceBannerSize - 0x20) == expectedCrc;
    }

    static bool isTargetBannerV3(const uint8_t *banner, size_t size) {
        if (!banner || size != kTargetBannerSize
            || banner[0] != 0x03 || banner[1] != 0x00) {
            return false;
        }
        const uint16_t crcV1 = static_cast<uint16_t>(banner[2])
            | (static_cast<uint16_t>(banner[3]) << 8);
        const uint16_t crcV2 = static_cast<uint16_t>(banner[4])
            | (static_cast<uint16_t>(banner[5]) << 8);
        const uint16_t crcV3 = static_cast<uint16_t>(banner[6])
            | (static_cast<uint16_t>(banner[7]) << 8);
        return crc16(banner + 0x20, 0x840 - 0x20) == crcV1
            && crc16(banner + 0x20, 0x940 - 0x20) == crcV2
            && crc16(banner + 0x20, 0xA40 - 0x20) == crcV3;
    }

    enum class TargetBannerFormat {
        Invalid,
        V1,
        V3,
    };

    static TargetBannerFormat targetBannerFormat(const uint8_t *banner, size_t size) {
        if (isTargetBannerV3(banner, size)) {
            return TargetBannerFormat::V3;
        }
        if (isTargetBannerV1(banner, size)) {
            return TargetBannerFormat::V1;
        }
        return TargetBannerFormat::Invalid;
    }

    static const char *targetBannerFailure(const uint8_t *banner, size_t size) {
        if (!banner || size < kSourceBannerSize) {
            return "banner record is too short";
        }
        if (banner[0] == 0x01 && banner[1] == 0x00) {
            return isTargetBannerV1(banner, size) ? nullptr
                : "v1 banner checksum is invalid";
        }
        if (banner[0] != 0x03 || banner[1] != 0x00) {
            return "banner version is neither v3 nor v1";
        }
        if (size != kTargetBannerSize) {
            return "v3 banner record has the wrong size";
        }
        const uint16_t crcV1 = static_cast<uint16_t>(banner[2])
            | (static_cast<uint16_t>(banner[3]) << 8);
        const uint16_t crcV2 = static_cast<uint16_t>(banner[4])
            | (static_cast<uint16_t>(banner[5]) << 8);
        const uint16_t crcV3 = static_cast<uint16_t>(banner[6])
            | (static_cast<uint16_t>(banner[7]) << 8);
        if (crc16(banner + 0x20, 0x840 - 0x20) != crcV1) {
            return "v3 primary checksum is invalid";
        }
        if (crc16(banner + 0x20, 0x940 - 0x20) != crcV2) {
            return "v3 Chinese checksum is invalid";
        }
        if (crc16(banner + 0x20, 0xA40 - 0x20) != crcV3) {
            return "v3 Korean checksum is invalid";
        }
        return nullptr;
    }

    using Util = FlashUtil<R4iSDHC, 2, &R4iSDHC::norRead, 12, &R4iSDHC::norErase4k, 8, &R4iSDHC::norWrite256>;

public:
    // Name & Size of Flash Memory
    R4iSDHC() : Flashcart("R4iSDHC family", "r4isdhc", 0x200000),
        cart_type(1), m_bannerLayoutValidated(false) { }

    const char* getAuthor() {
        return "handsomematt, Rai-chan, Kitlith, stuckpixel, angelsl";
    }

    const char* getDescription() {
        return
            "A family of DSTT clones. Works with:\n"
            " * R4iSDHC RTS Lite (r4isdhc.com)\n"
            " * R4i-SDHC 3DS RTS (r4i-sdhc.com)\n"
            " * R4i-SDHC B9S (r4i-sdhc.com)\n"
            "\n"
            "Not the Dual-Core 2013 variant.";
    }

    bool initialize() {
        m_bannerLayoutValidated = false;
        const CartType1Check type1Check = checkCartType1();
        if (type1Check == CartType1Check::Error) {
            return false;
        }
        if (type1Check == CartType1Check::Match) {
            cart_type = 1;
        } else {
            switch (m_card->state()) {
                case ncgc::NTRState::Raw:
                    if (!trySecureInit(BlowfishKey::NTR)
                        && !trySecureInit(BlowfishKey::B9Retail)
                        && !trySecureInit(BlowfishKey::B9Dev)) {
                        logMessage(LOG_ERR, "r4isdhc: type 2 init from RAW failed");
                        return false;
                    }
                    break;
                case ncgc::NTRState::Key2:
                    if (!checkCartType2()) {
                        logMessage(LOG_ERR, "r4isdhc: type 2 init from KEY2 failed");
                        return false;
                    }
                    break;
                default:
                    logMessage(LOG_ERR, "r4isdhc: Unexpected encryption status %d", m_card->state());
                    return false;
            }
            cart_type = 2;
        }

        uint32_t read1;
        uint32_t read2;
        if (!norRead(0, &read1) || !norRead(0, &read2)) {
            return false;
        }
        if (read1 != read2) {
            logMessage(LOG_ERR, "r4isdhc: two reads from flash @ 0 returned 0x%08lX and 0x%08lX", read1, read2);
            return false;
        }

        uint8_t targetHeader[0x200];
        uint8_t targetBanner[kTargetBannerSize];
        if (!Util::read(this, kBannerHeaderOffset, sizeof(targetHeader), targetHeader)) {
            logMessage(LOG_NOTICE,
                "r4isdhc banner: target header read failed; option disabled");
        } else if (!isExpectedHeader(targetHeader, sizeof(targetHeader))) {
            logMessage(LOG_NOTICE,
                "r4isdhc banner: target is not the known 20XX layout");
        } else if (!Util::read(this, kBannerOffset, sizeof(targetBanner), targetBanner)) {
            logMessage(LOG_NOTICE,
                "r4isdhc banner: target banner read failed; option disabled");
        } else {
            const TargetBannerFormat format = targetBannerFormat(targetBanner,
                sizeof(targetBanner));
            if (format == TargetBannerFormat::Invalid) {
                logMessage(LOG_NOTICE,
                    "r4isdhc banner: %s; option disabled",
                    targetBannerFailure(targetBanner, sizeof(targetBanner)));
            } else {
                m_bannerLayoutValidated = true;
                logMessage(LOG_NOTICE,
                    "r4isdhc banner: 20XX target geometry verified (current v%d)",
                    format == TargetBannerFormat::V3 ? 3 : 1);
            }
        }

        logMessage(LOG_ERR, "r4isdhc: found type %d cart", cart_type);
        return true;
    }

    void shutdown() { }

    bool readFlash(const uint32_t address, const uint32_t length, uint8_t *const buffer) override {
        return Util::read(this, address, length, buffer, true);
    }

    bool writeFlash(const uint32_t address, const uint32_t length, const uint8_t *const buffer) override {
        return Util::write(this, address, length, buffer, true);
    }

    const BannerWriteProfile *getBannerWriteProfile() const override {
        return m_bannerLayoutValidated ? &kBannerWriteProfile : nullptr;
    }

    bool writeBanner(const uint8_t *banner, uint32_t bannerSize) override {
        if (!getBannerWriteProfile() || !isSourceBannerV1(banner, bannerSize)) {
            logMessage(LOG_ERR, "r4isdhc banner: refusing unsupported write");
            return false;
        }

        uint8_t targetHeader[0x200];
        if (!Util::read(this, kBannerHeaderOffset, sizeof(targetHeader), targetHeader)
            || !isExpectedHeader(targetHeader, sizeof(targetHeader))) {
            logMessage(LOG_ERR,
                "r4isdhc banner: target header no longer matches the 20XX layout");
            return false;
        }

        uint8_t targetBanner[kTargetBannerSize];
        if (!Util::read(this, kBannerOffset, kTargetBannerSize, targetBanner)) {
            logMessage(LOG_ERR,
                "r4isdhc banner: target banner read failed");
            return false;
        }
        const TargetBannerFormat targetFormat = targetBannerFormat(targetBanner,
            sizeof(targetBanner));
        if (targetFormat == TargetBannerFormat::Invalid) {
            logMessage(LOG_ERR,
                "r4isdhc banner: %s",
                targetBannerFailure(targetBanner, sizeof(targetBanner)));
            return false;
        }

        // A v3 record extends 64 bytes into the next 4 KiB block. When
        // converting it to v1, zero that now-unused extension as well; both
        // pages must be read-modify-written and compared in full.
        const uint32_t affectedSize = targetFormat == TargetBannerFormat::V3
            ? kBannerBlockSize * 2 : kBannerBlockSize;
        uint8_t *expectedBlocks = new(std::nothrow) uint8_t[affectedSize];
        uint8_t *verifiedBlocks = new(std::nothrow) uint8_t[affectedSize];
        if (!expectedBlocks || !verifiedBlocks) {
            delete[] expectedBlocks;
            delete[] verifiedBlocks;
            logMessage(LOG_ERR, "r4isdhc banner: verification buffers unavailable");
            return false;
        }
        if (!Util::read(this, kBannerBlockStart, affectedSize, expectedBlocks)) {
            delete[] expectedBlocks;
            delete[] verifiedBlocks;
            logMessage(LOG_ERR, "r4isdhc banner: couldn't read target blocks");
            return false;
        }

        const uint32_t bannerInBlock = kBannerOffset - kBannerBlockStart;
        std::memcpy(expectedBlocks + bannerInBlock, banner, kSourceBannerSize);
        if (targetFormat == TargetBannerFormat::V3) {
            std::memset(expectedBlocks + bannerInBlock + kSourceBannerSize, 0,
                kTargetBannerSize - kSourceBannerSize);
            logMessage(LOG_NOTICE,
                "r4isdhc banner: writing blocks %08lX-%08lX (v3 to v1; clearing 512-byte extension)",
                static_cast<unsigned long>(kBannerBlockStart),
                static_cast<unsigned long>(kBannerBlockStart + affectedSize - 1));
        } else {
            logMessage(LOG_NOTICE,
                "r4isdhc banner: writing block %08lX-%08lX (v1 to v1)",
                static_cast<unsigned long>(kBannerBlockStart),
                static_cast<unsigned long>(kBannerBlockStart + affectedSize - 1));
        }

        if (!Util::write(this, kBannerBlockStart, affectedSize, expectedBlocks, false)) {
            delete[] expectedBlocks;
            delete[] verifiedBlocks;
            logMessage(LOG_ERR, "r4isdhc banner: write verification failed");
            return false;
        }
        if (!Util::read(this, kBannerBlockStart, affectedSize, verifiedBlocks)
            || std::memcmp(expectedBlocks, verifiedBlocks, affectedSize) != 0) {
            delete[] expectedBlocks;
            delete[] verifiedBlocks;
            logMessage(LOG_ERR,
                "r4isdhc banner: full erase-block verification failed");
            return false;
        }

        delete[] expectedBlocks;
        delete[] verifiedBlocks;
        logMessage(LOG_NOTICE, "r4isdhc banner: write verified");
        return true;
    }

    bool injectNtrBoot(uint8_t *blowfish_key, uint8_t *firm, uint32_t firm_size) override {
        // FIRM is written at 0x7E00; blowfish key at 0x1F1000
        // N.B. this doesn't necessarily mean that the cart's ROM => NOR mapping will
        // allow a FIRM of this size (i.e. old carts), it's just so we don't overwrite
        // the blowfish key
        if (firm_size > (0x1F1000 - 0x7E00)) {
            showProgress(0, 1, "FIRM too big (max 2003456 bytes)");
            return false;
        }

        uint8_t map[0x100] = {0};
        // set the 2nd ROM map to some high value (0x7FFFFFFF in big-endian)
        map[4] = 0x7F; map[5] = 0xFF; map[6] = 0xFF; map[7] = 0xFF;
        return
            // 1:1 map the ROM <=> NOR (unless it's an "old" cart - those don't seem to have
            // a mapping in the NOR)
            Util::write(this, 0x1000, 0x48, blowfish_key, true, "Writing Blowfish key (1)") && // blowfish P array
            Util::write(this, 0x2000, 0x1000, blowfish_key+0x48, true, "Writing Blowfish key (2)") && // blowfish S boxes
            (
                (cart_type == 1 && Util::write(this, 0x40, 0x100, map, true, "Writing ROM <=> NOR map")) ||
                (cart_type == 2 && true) // type 2 not need ROM-NOR map
            ) &&
            Util::write(this, 0x1F1000, 0x48, blowfish_key, true, "Writing Blowfish key (3)") && // blowfish P array
            Util::write(this, 0x1F2000, 0x1000, blowfish_key+0x48, true, "Writing Blowfish key (4)") && // blowfish S boxes
            Util::write(this, 0x7E00, firm_size, firm, true, "Writing FIRM (1)") && // FIRM
            // type2 carts read 0x8000-0x10000 from 0x1F8000-0x200000 instead of from 0x8000
            Util::write(this, 0x1F7E00, std::min<uint32_t>(firm_size, (cart_type == 1 ? 0x200 : 0x8200)), firm, true,
                "Writing FIRM (2)"); // FIRM header
    }
};

const BannerWriteProfile R4iSDHC::kBannerWriteProfile = {
    R4iSDHC::kSourceBannerSize,
};

R4iSDHC r4isdhc;
}
