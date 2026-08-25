#include <cstring>

#include "device.h"
#include "debug_simulated_banner_bin.h"

namespace flashcart_core {
namespace {

class DebugSimulatedCart : public Flashcart {
    static constexpr uint32_t kFlashSize = 0x200000;
    static constexpr uint32_t kBannerOffset = 0x5000;
    static constexpr uint32_t kBannerBlockSize = 0x1000;
    static constexpr uint32_t kBannerSize = 0x840;

    uint8_t m_bannerBlock[kBannerBlockSize];
    bool m_initialized;

    static uint8_t fakeByte(uint32_t address) {
        return static_cast<uint8_t>((address * 33u) ^ (address >> 8) ^ 0xA5u);
    }

    void resetBanner() {
        static_assert(debug_simulated_banner_bin_size == kBannerSize,
            "Debug banner must be an NDS v1 record");
        for (uint32_t i = 0; i < kBannerBlockSize; ++i) {
            m_bannerBlock[i] = fakeByte(kBannerOffset + i);
        }
        std::memcpy(m_bannerBlock, debug_simulated_banner_bin, kBannerSize);
    }

    void copyRead(uint32_t address, uint32_t length, uint8_t *buffer) {
        for (uint32_t offset = 0; offset < length; ++offset) {
            buffer[offset] = fakeByte(address + offset);
        }

        const uint32_t readEnd = address + length;
        const uint32_t blockEnd = kBannerOffset + kBannerBlockSize;
        const uint32_t overlapStart = address > kBannerOffset ? address : kBannerOffset;
        const uint32_t overlapEnd = readEnd < blockEnd ? readEnd : blockEnd;
        if (overlapStart < overlapEnd) {
            std::memcpy(buffer + overlapStart - address,
                m_bannerBlock + overlapStart - kBannerOffset,
                overlapEnd - overlapStart);
        }
    }

    void copyWrite(uint32_t address, uint32_t length, const uint8_t *buffer) {
        const uint32_t writeEnd = address + length;
        const uint32_t blockEnd = kBannerOffset + kBannerBlockSize;
        const uint32_t overlapStart = address > kBannerOffset ? address : kBannerOffset;
        const uint32_t overlapEnd = writeEnd < blockEnd ? writeEnd : blockEnd;
        if (overlapStart < overlapEnd) {
            std::memcpy(m_bannerBlock + overlapStart - kBannerOffset,
                buffer + overlapStart - address,
                overlapEnd - overlapStart);
        }
    }

public:
    DebugSimulatedCart()
        : Flashcart("Debug simulated cart", "debug-simulated", kFlashSize),
          m_initialized(false) {}

    const char *getAuthor() override {
        return "Cart-Flasher";
    }

    const char *getDescription() override {
        return
            "Test Cart-Flasher without a cart.\n"
            "Nothing is written to Slot-1.";
    }

    bool requiresCardInitialization() const override {
        return false;
    }

    bool initialize() override {
        if (!m_initialized) {
            resetBanner();
            m_initialized = true;
        }
        platform::logMessage(LOG_NOTICE,
            "DebugSim: initialized without cart hardware");
        return true;
    }

    void shutdown() override {}

    bool readFlash(uint32_t address, uint32_t length, uint8_t *buffer) override {
        if (!buffer || address > kFlashSize || length > kFlashSize - address) {
            return false;
        }
        copyRead(address, length, buffer);
        platform::showProgress(length, length, "Reading flash");
        platform::logMessage(LOG_DEBUG,
            "DebugSim: generated read at %08lX size=%08lX",
            static_cast<unsigned long>(address), static_cast<unsigned long>(length));
        return true;
    }

    bool writeFlash(uint32_t address, uint32_t length,
                    const uint8_t *buffer) override {
        if (!buffer || address > kFlashSize || length > kFlashSize - address) {
            return false;
        }
        copyWrite(address, length, buffer);
        platform::showProgress(length, length, "Writing flash");
        platform::logMessage(LOG_DEBUG,
            "DebugSim: accepted write at %08lX size=%08lX",
            static_cast<unsigned long>(address), static_cast<unsigned long>(length));
        return true;
    }

    bool injectNtrBoot(uint8_t *blowfishKey, uint8_t *firm,
                       uint32_t firmSize) override {
        (void)blowfishKey;
        (void)firm;
        (void)firmSize;
        platform::logMessage(LOG_ERR,
            "DebugSim: ntrboot injection is not simulated");
        return false;
    }
};

DebugSimulatedCart debugSimulatedCart;

} // namespace
} // namespace flashcart_core
