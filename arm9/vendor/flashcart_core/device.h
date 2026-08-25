#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>

#include <ncgcpp/ntrcard.h>

#include "platform.h"

using std::uint8_t;
using std::uint16_t;
using std::uint32_t;

// Utility -- s must be a power of two and have no side effects.
#define PAGE_ROUND_UP(x, s) ( ((x) + (s)-1)  & (~((s)-1)) )
#define PAGE_ROUND_DOWN(x, s) ( (x) & (~((s)-1)) )

// Spelled exactly as libnds spells it in ndstypes.h, unsigned included. An
// identical macro redefinition is legal and silent, so this collides benignly
// no matter which header lands first -- the signed version warned whenever a
// .cpp reached device.h before <nds.h>. Unsigned is also just correct: 1 << 31
// is signed overflow. The #ifndef stays so a platform that already defines BIT
// keeps its own.
#ifndef BIT
#define BIT(n) (1u << (n))
#endif
namespace flashcart_core {
struct BannerWriteProfile {
    uint32_t bannerSize;
};

class Flashcart {
public:
    Flashcart(const char* name, const size_t max_length);
    Flashcart(const char* name, const char* short_name, const size_t max_length);

    inline bool initialize(ncgc::NTRCard *card) {
        m_card = card;
        prepareNormalInitialization();
        return initialize();
    }
    virtual bool hasRecoveryProfile() const { return false; }
    virtual const char *getRecoveryPrompt() const { return nullptr; }
    virtual bool initializeRecovery(ncgc::NTRCard *card) {
        (void)card;
        return false;
    }
    virtual void shutdown() = 0;

    virtual bool readFlash(uint32_t address, uint32_t length, uint8_t *buffer) = 0;
    virtual bool writeFlash(uint32_t address, uint32_t length, const uint8_t *buffer) = 0;
    virtual bool injectNtrBoot(uint8_t *blowfish_key, uint8_t *firm, uint32_t firm_size) = 0;
    // Banner writes are opt-in because their target geometry is cart-specific.
    // The platform validates the shared NDS v1 banner file before the driver
    // performs its own target-layout check and read-modify-write.
    virtual const BannerWriteProfile *getBannerWriteProfile() const { return nullptr; }
    virtual bool writeBanner(const uint8_t *banner, uint32_t bannerSize) {
        (void)banner;
        (void)bannerSize;
        return false;
    }

    const char *getName() { return m_name; }
    const char *getShortName() { return m_short_name; }
    virtual const char *getAuthor() { return "unknown"; }
    virtual const char *getDescription() { return ""; }
    virtual size_t getMaxLength() { return m_max_length; }

protected:
    const char* m_name;
    const char* m_short_name;
    const size_t m_max_length;
    ncgc::NTRCard *m_card;

    virtual void prepareNormalInitialization() {}
    virtual bool initialize() = 0;
};

extern std::vector<Flashcart*> *flashcart_list;
}
