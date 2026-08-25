// Datel Slot-1 carts: Games N' Music, Max Media Player, Action Replay DS and
// Action Replay DSi Media Edition.
//
// The Datel protocols use AUXSPI transactions with data-dependent toggle-bit
// polling. Ported from ApacheThunder and edo9300's GPL-3.0 datelTool.

#include <algorithm>
#include <cstring>
#include <new>

#include <ncgcpp/ntrcard.h>

#include "../device.h"

namespace flashcart_core {
using platform::logMessage;
using platform::showProgress;

namespace {
enum DatelProduct : uint8_t {
    GAMES_N_MUSIC = 1,
    ACTION_REPLAY_DS = 2,
    ACTION_REPLAY_DSIME = 4
};

enum class Protocol { None, GNM, ActionReplay };
enum class EraseLayout { Uniform, En29LvTopBoot, En29LvBottomBoot };

struct FlashChip {
    uint16_t id;
    const char *name;
    uint8_t products;
    uint16_t cmdSeqAddr;
    uint8_t sectorEraseCmd;
    uint32_t capacity;
    uint32_t uniformEraseSize;
    EraseLayout eraseLayout;
    bool backupSupported;
    bool restoreSupported;
};

constexpr uint32_t kPollLimit = 0x100000;
constexpr uint32_t kReadTransactionSize = 0x1000;

// SST39VF1681 is physically 2 MiB, but the tested Action Replay ASIC mirrors
// its second MiB. Keep every operation in the independently addressable first
// MiB. EN29LV parts remain detection-only until their top/bottom boot
// geometries are tested.
const FlashChip kChips[] = {
    { 0xC8BF, "SST39VF1681", ACTION_REPLAY_DSIME | ACTION_REPLAY_DS, 0x0AAA, 0x50,
      0x100000, 0x1000, EraseLayout::Uniform, true, true },
    { 0xC9BF, "SST39VF1682", ACTION_REPLAY_DS, 0x0AAA, 0x50,
      0x200000, 0x1000, EraseLayout::Uniform, true, true },
    { 0xC41C, "EN29LV160BT", ACTION_REPLAY_DSIME, 0x0AAA, 0x30,
      0x200000, 0, EraseLayout::En29LvTopBoot, false, false },
    { 0x491C, "EN29LV160BB", ACTION_REPLAY_DSIME, 0x0AAA, 0x30,
      0x200000, 0, EraseLayout::En29LvBottomBoot, false, false },
    { 0xF61C, "EN29LV320AT", ACTION_REPLAY_DSIME, 0x0AAA, 0x30,
      0x400000, 0, EraseLayout::En29LvTopBoot, false, false },
    { 0xF91C, "EN29LV320AB", ACTION_REPLAY_DSIME, 0x0AAA, 0x30,
      0x400000, 0, EraseLayout::En29LvBottomBoot, false, false },
    { 0xD4BF, "SST39LF/VF512", GAMES_N_MUSIC, 0x5555, 0x30,
      0x010000, 0x1000, EraseLayout::Uniform, true, true },
    { 0xD5BF, "SST39LF/VF010", GAMES_N_MUSIC, 0x5555, 0x30,
      0x020000, 0x1000, EraseLayout::Uniform, true, true },
    { 0xD6BF, "SST39LF/VF020", GAMES_N_MUSIC, 0x5555, 0x30,
      0x040000, 0x1000, EraseLayout::Uniform, true, true },
    { 0xD7BF, "SST39LF/VF040", GAMES_N_MUSIC, 0x5555, 0x30,
      0x080000, 0x1000, EraseLayout::Uniform, true, true },
};

const FlashChip *findChip(uint16_t id) {
    for (const FlashChip &chip : kChips) {
        if (chip.id == id) {
            return &chip;
        }
    }
    return nullptr;
}
}

class Datel : Flashcart {
    Protocol m_protocol = Protocol::None;
    const FlashChip *m_chip = nullptr;

    bool txn(const uint8_t *out, uint32_t outLen, uint8_t *in, uint32_t inLen) {
        if (!outLen && !inLen) {
            logMessage(LOG_ERR, "Datel: empty SPI transaction");
            return false;
        }

        bool held = false;
        for (uint32_t i = 0; i < outLen; ++i) {
            const bool last = !inLen && (i + 1) == outLen;
            if (m_card->sendSpiByte(out[i], nullptr, last)) {
                logMessage(LOG_ERR, "Datel: SPI write failed at byte %lu", (unsigned long)i);
                if (held) {
                    m_card->endSpiTransaction();
                }
                return false;
            }
            held = !last;
        }
        for (uint32_t i = 0; i < inLen; ++i) {
            const bool last = (i + 1) == inLen;
            if (m_card->sendSpiByte(0, in ? in + i : nullptr, last)) {
                logMessage(LOG_ERR, "Datel: SPI read failed at byte %lu", (unsigned long)i);
                if (held) {
                    m_card->endSpiTransaction();
                }
                return false;
            }
            held = !last;
        }
        return true;
    }

    bool txnWrite(const uint8_t *out, uint32_t outLen) {
        return txn(out, outLen, nullptr, 0);
    }

    bool setSpiMode(uint8_t first, uint8_t second) {
        const uint8_t cmd[8] = { first, second, 0, 0, 0, 0, 0, 0 };
        const ncgc::Err err = m_card->sendCommand(cmd, nullptr, 0, 0xA0586000, true);
        if (err) {
            logMessage(LOG_ERR, "Datel: setSpiMode failed: %d", err.errNo());
            return false;
        }
        return true;
    }

    bool eraseBlockAt(uint32_t address, uint32_t *size) const {
        if (!m_chip || !size || address >= m_chip->capacity) {
            return false;
        }
        if (m_chip->eraseLayout == EraseLayout::Uniform) {
            if (!m_chip->uniformEraseSize || (address % m_chip->uniformEraseSize) != 0) {
                return false;
            }
            *size = m_chip->uniformEraseSize;
            return true;
        }

        bool topBoot;
        switch (m_chip->eraseLayout) {
            case EraseLayout::En29LvTopBoot:
                topBoot = true;
                break;
            case EraseLayout::En29LvBottomBoot:
                topBoot = false;
                break;
            default:
                return false;
        }

        const uint32_t bootBase = topBoot ? m_chip->capacity - 0x10000 : 0;
        if ((topBoot && address < bootBase) || (!topBoot && address >= 0x10000)) {
            if ((address % 0x10000) != 0) {
                return false;
            }
            *size = 0x10000;
            return true;
        }

        const uint32_t offset = address - bootBase;
        if (topBoot) {
            switch (offset) {
                case 0x0000: *size = 0x8000; return true;
                case 0x8000: *size = 0x2000; return true;
                case 0xA000: *size = 0x2000; return true;
                case 0xC000: *size = 0x4000; return true;
                default: return false;
            }
        }
        switch (offset) {
            case 0x0000: *size = 0x4000; return true;
            case 0x4000: *size = 0x2000; return true;
            case 0x6000: *size = 0x2000; return true;
            case 0x8000: *size = 0x8000; return true;
            default: return false;
        }
    }

    bool gnmWriteAddr(uint32_t address) {
        const uint8_t cmd[4] = {
            0xE3,
            static_cast<uint8_t>(address >> 16),
            static_cast<uint8_t>(address >> 8),
            static_cast<uint8_t>(address)
        };
        return txnWrite(cmd, sizeof(cmd));
    }

    bool gnmWriteByte(uint8_t value) {
        const uint8_t cmd[2] = { 0xE4, value };
        return txnWrite(cmd, sizeof(cmd));
    }

    bool gnmReadBytes(uint8_t *out, uint32_t len) {
        static const uint8_t cmd = 0xE7;
        return txn(&cmd, 1, out, len);
    }

    bool gnmWaitWriteDone() {
        if (m_card->sendSpiByte(0xE8, nullptr, false)) {
            logMessage(LOG_ERR, "Datel: GNM poll opcode failed");
            return false;
        }
        uint8_t previous = 0;
        if (m_card->sendSpiByte(0, &previous, false)) {
            m_card->endSpiTransaction();
            return false;
        }
        previous &= 0x40;
        for (uint32_t i = 0; i < kPollLimit; ++i) {
            uint8_t current = 0;
            if (m_card->sendSpiByte(0, &current, false)) {
                m_card->endSpiTransaction();
                return false;
            }
            current &= 0x40;
            if (current == previous) {
                m_card->endSpiTransaction();
                return true;
            }
            previous = current;
        }
        m_card->endSpiTransaction();
        logMessage(LOG_ERR, "Datel: GNM write timed out");
        return false;
    }

    bool gnmUnlock(uint8_t command) {
        if (!m_chip) {
            return false;
        }
        const uint32_t main = m_chip->cmdSeqAddr;
        const uint32_t other = main / 2;
        return gnmWriteAddr(main) && gnmWriteByte(0xAA)
            && gnmWriteAddr(other) && gnmWriteByte(0x55)
            && gnmWriteAddr(main) && gnmWriteByte(command);
    }

    bool gnmReadChipId(uint32_t oddAddr, uint16_t *out) {
        const uint32_t other = oddAddr / 2;
        if (!(gnmWriteAddr(oddAddr) && gnmWriteByte(0xAA)
           && gnmWriteAddr(other) && gnmWriteByte(0x55)
           && gnmWriteAddr(oddAddr) && gnmWriteByte(0x90)
           && gnmWriteAddr(0))) {
            return false;
        }

        uint8_t id[2] = { 0, 0 };
        if (!gnmReadBytes(id, sizeof(id)) || !gnmWriteByte(0xF0)) {
            return false;
        }
        *out = static_cast<uint16_t>(id[0] | (id[1] << 8));
        return true;
    }

    bool gnmEraseBlock(uint32_t blockAddr) {
        if (!m_chip) {
            return false;
        }
        const uint32_t main = m_chip->cmdSeqAddr;
        const uint32_t other = main / 2;
        return gnmWriteAddr(main) && gnmWriteByte(0xAA)
            && gnmWriteAddr(other) && gnmWriteByte(0x55)
            && gnmWriteAddr(main) && gnmWriteByte(0x80)
            && gnmWriteAddr(main) && gnmWriteByte(0xAA)
            && gnmWriteAddr(other) && gnmWriteByte(0x55)
            && gnmWriteAddr(blockAddr) && gnmWriteByte(m_chip->sectorEraseCmd)
            && gnmWaitWriteDone();
    }

    bool gnmWriteBlock(uint32_t blockAddr, uint32_t blockSize, const uint8_t *buffer,
                       uint32_t progressBase, uint32_t progressTotal) {
        for (uint32_t i = 0; i < blockSize; ++i) {
            if (!gnmUnlock(0xA0) || !gnmWriteAddr(blockAddr + i)
                || !gnmWriteByte(buffer[i]) || !gnmWaitWriteDone()) {
                return false;
            }
            showProgress(progressBase + i + 1, progressTotal, "Writing flash");
        }
        return true;
    }

    static constexpr uint8_t kArSetAddress = 0;
    static constexpr uint8_t kArCommandSequence = 1;
    static constexpr uint8_t kArAutoIncrement = 1;
    static constexpr uint8_t kArWriteEnable = 2;
    static constexpr uint8_t kArReadEnable = 4;
    static constexpr uint8_t kArBegin = 6;
    static constexpr uint8_t kArEnd = 7;

    bool arBegin() {
        const uint8_t mode = kArBegin;
        return txnWrite(&mode, 1);
    }

    bool arEnd() {
        const uint8_t mode = kArEnd;
        return txnWrite(&mode, 1);
    }

    bool arSetAddress(uint32_t address) {
        const uint8_t cmd[4] = {
            kArSetAddress,
            static_cast<uint8_t>(address),
            static_cast<uint8_t>(address >> 8),
            static_cast<uint8_t>(address >> 16)
        };
        return txnWrite(cmd, sizeof(cmd));
    }

    bool arCommandSequence(const uint8_t *sequence, uint32_t length) {
        if (length > 5) {
            logMessage(LOG_ERR, "Datel: AR command sequence too long");
            return false;
        }
        uint8_t cmd[6] = { kArCommandSequence };
        std::memcpy(cmd + 1, sequence, length);
        return txnWrite(cmd, length + 1);
    }

    bool arWriteByte(uint8_t value) {
        const uint8_t cmd[2] = { kArWriteEnable, value };
        return txnWrite(cmd, sizeof(cmd));
    }

    bool arReadBytes(uint8_t *out, uint32_t length) {
        const uint8_t mode = kArReadEnable | kArAutoIncrement;
        return txn(&mode, 1, out, length);
    }

    bool arWaitWriteDone() {
        if (m_card->sendSpiByte(kArReadEnable, nullptr, false)) {
            return false;
        }
        uint8_t previous = 0;
        if (m_card->sendSpiByte(0, &previous, false)) {
            m_card->endSpiTransaction();
            return false;
        }
        previous &= 0x40;
        for (uint32_t i = 0; i < kPollLimit; ++i) {
            uint8_t current = 0;
            if (m_card->sendSpiByte(0, &current, false)) {
                m_card->endSpiTransaction();
                return false;
            }
            current &= 0x40;
            if (current == previous) {
                m_card->endSpiTransaction();
                return true;
            }
            previous = current;
        }
        m_card->endSpiTransaction();
        logMessage(LOG_ERR, "Datel: AR write timed out");
        return false;
    }

    bool arReadChipId(uint16_t *out) {
        if (!arBegin()) {
            return false;
        }
        const uint8_t sequence[3] = { 0xAA, 0x55, 0x90 };
        uint8_t id[2] = { 0, 0 };
        bool ok = arSetAddress(0) && arCommandSequence(sequence, sizeof(sequence))
            && arReadBytes(id, sizeof(id)) && arWriteByte(0xF0);
        ok = arEnd() && ok;
        if (ok) {
            *out = static_cast<uint16_t>(id[0] | (id[1] << 8));
        }
        return ok;
    }

    bool arReadChipIdEon(uint16_t *out) {
        const uint8_t sequence[3] = { 0xAA, 0x55, 0x90 };
        uint8_t manufacturer = 0;
        uint8_t device = 0;

        if (!arBegin()) {
            return false;
        }
        bool ok = arSetAddress(0x200) && arCommandSequence(sequence, sizeof(sequence))
            && arReadBytes(&manufacturer, 1) && arWriteByte(0xF0);
        ok = arEnd() && ok;
        if (!ok || !arBegin()) {
            return false;
        }
        ok = arSetAddress(2) && arCommandSequence(sequence, sizeof(sequence))
            && arReadBytes(&device, 1) && arWriteByte(0xF0);
        ok = arEnd() && ok;
        if (ok) {
            *out = static_cast<uint16_t>((manufacturer << 8) | device);
        }
        return ok;
    }

    bool arEraseBlock(uint32_t blockAddr) {
        if (!m_chip || !arBegin()) {
            return false;
        }
        const uint8_t sequence[5] = { 0xAA, 0x55, 0x80, 0xAA, 0x55 };
        bool ok = arSetAddress(blockAddr) && arCommandSequence(sequence, sizeof(sequence))
            && arWriteByte(m_chip->sectorEraseCmd) && arWaitWriteDone();
        return arEnd() && ok;
    }

    bool arWriteBlock(uint32_t blockAddr, uint32_t blockSize, const uint8_t *buffer,
                      uint32_t progressBase, uint32_t progressTotal) {
        if (!arBegin()) {
            return false;
        }
        const uint8_t sequence[3] = { 0xAA, 0x55, 0xA0 };
        bool ok = arSetAddress(blockAddr);
        for (uint32_t i = 0; ok && i < blockSize; ++i) {
            ok = arCommandSequence(sequence, sizeof(sequence));
            const uint8_t cmd[2] = { kArWriteEnable | kArAutoIncrement, buffer[i] };
            ok = ok && txnWrite(cmd, sizeof(cmd));
            ok = ok && arWaitWriteDone();
            showProgress(progressBase + i + 1, progressTotal, "Writing flash");
        }
        return arEnd() && ok;
    }

    bool arReadFrom(uint32_t address, uint8_t *out, uint32_t length) {
        if (!arBegin()) {
            return false;
        }
        const bool ok = arSetAddress(address) && arReadBytes(out, length);
        return arEnd() && ok;
    }

public:
    Datel() : Flashcart("Datel", "datel", 0) {}

    const char *getAuthor() override {
        return "ApacheThunder, edo9300, tasken";
    }

    const char *getDescription() override {
        return
            "Works with:\n"
            " * Games N' Music\n"
            " * Max Media Player\n"
            " * Action Replay DS\n"
            " * Action Replay DSi Media Edition";
    }

    size_t getMaxLength() override {
        return m_chip && m_chip->backupSupported ? m_chip->capacity : 0;
    }

    bool initialize() override {
        logMessage(LOG_INFO, "Datel: Init");
        m_chip = nullptr;
        m_protocol = Protocol::None;

        if (setSpiMode(0xF0, 0x01)) {
            m_protocol = Protocol::ActionReplay;
            uint16_t id = 0;
            if (arReadChipId(&id)) {
                logMessage(LOG_NOTICE, "Datel: AR chip id = %04X", id);
                m_chip = findChip(id);
            }
            if (!m_chip && arReadChipIdEon(&id)) {
                logMessage(LOG_NOTICE, "Datel: AR EON chip id = %04X", id);
                m_chip = findChip(id);
            }
        }

        if (!m_chip && setSpiMode(0xF2, 0)) {
            m_protocol = Protocol::GNM;
            uint16_t id = 0;
            if (gnmReadChipId(0x5555, &id)) {
                logMessage(LOG_NOTICE, "Datel: GNM chip id (5555) = %04X", id);
                m_chip = findChip(id);
            }
            if (!m_chip && gnmReadChipId(0x0AAA, &id)) {
                logMessage(LOG_NOTICE, "Datel: GNM chip id (0AAA) = %04X", id);
                m_chip = findChip(id);
            }
        }

        if (!m_chip) {
            m_protocol = Protocol::None;
            logMessage(LOG_ERR, "Datel: no known flash chip found");
            return false;
        }
        const char *access = !m_chip->backupSupported ? " (backup/restore blocked)"
            : !m_chip->restoreSupported ? " (restore blocked)" : "";
        logMessage(LOG_NOTICE, "Datel: %s, nominal %lu KB%s", m_chip->name,
            (unsigned long)(m_chip->capacity / 1024), access);
        return true;
    }

    void shutdown() override {}

    bool readFlash(uint32_t address, uint32_t length, uint8_t *buffer) override {
        // writeFlash() uses this for every 4 KiB pre-erase comparison and
        // post-program verification. INFO would reopen the FAT log for each
        // one, turning the safety readback into avoidable SD-card overhead.
        logMessage(LOG_DEBUG, "Datel: readFlash(addr=0x%08lx, size=0x%lx)",
            (unsigned long)address, (unsigned long)length);
        if (!m_chip || !m_chip->backupSupported || !buffer
            || address > m_chip->capacity || length > m_chip->capacity - address) {
            return false;
        }

        for (uint32_t offset = 0; offset < length;) {
            const uint32_t count = std::min<uint32_t>(kReadTransactionSize, length - offset);
            switch (m_protocol) {
                case Protocol::GNM:
                    if (!gnmWriteAddr(address + offset) || !gnmReadBytes(buffer + offset, count)) {
                        return false;
                    }
                    break;
                case Protocol::ActionReplay:
                    if (!arReadFrom(address + offset, buffer + offset, count)) {
                        return false;
                    }
                    break;
                default:
                    return false;
            }
            offset += count;
        }
        return true;
    }

    bool writeFlash(uint32_t address, uint32_t length, const uint8_t *buffer) override {
        logMessage(LOG_INFO, "Datel: writeFlash(addr=0x%08lx, size=0x%lx)",
            (unsigned long)address, (unsigned long)length);
        if (!m_chip || !m_chip->backupSupported || !buffer
            || address > m_chip->capacity || length > m_chip->capacity - address) {
            return false;
        }
        if (!m_chip->restoreSupported) {
            logMessage(LOG_ERR, "Datel: restore is blocked for this flash chip");
            return false;
        }

        uint8_t *verify = new (std::nothrow) uint8_t[0x10000];
        if (!verify) {
            logMessage(LOG_ERR, "Datel: verification buffer allocation failed");
            return false;
        }

        for (uint32_t offset = 0; offset < length;) {
            uint32_t blockSize = 0;
            const uint32_t block = address + offset;
            if (!eraseBlockAt(block, &blockSize) || blockSize > length - offset) {
                logMessage(LOG_ERR, "Datel: write is not erase-block aligned");
                delete[] verify;
                return false;
            }

            if (!readFlash(block, blockSize, verify)) {
                delete[] verify;
                return false;
            }
            if (!std::memcmp(verify, buffer + offset, blockSize)) {
                offset += blockSize;
                showProgress(offset, length, "Writing flash");
                continue;
            }

            bool programmed = false;
            switch (m_protocol) {
                case Protocol::GNM:
                    programmed = gnmEraseBlock(block)
                        && gnmWriteBlock(block, blockSize, buffer + offset, offset, length);
                    break;
                case Protocol::ActionReplay:
                    programmed = arEraseBlock(block)
                        && arWriteBlock(block, blockSize, buffer + offset, offset, length);
                    break;
                default:
                    delete[] verify;
                    return false;
            }
            if (!programmed || !readFlash(block, blockSize, verify)
                || std::memcmp(verify, buffer + offset, blockSize)) {
                logMessage(LOG_ERR, "Datel: block verification failed at 0x%08lx",
                    (unsigned long)block);
                delete[] verify;
                return false;
            }

            offset += blockSize;
            showProgress(offset, length, "Writing flash");
        }

        delete[] verify;
        return true;
    }

    bool injectNtrBoot(uint8_t *blowfish_key, uint8_t *firm, uint32_t firm_size) override {
        (void)blowfish_key;
        (void)firm;
        (void)firm_size;
        logMessage(LOG_ERR, "Datel: ntrboot injection is not implemented");
        return false;
    }
};

Datel datel;
}
