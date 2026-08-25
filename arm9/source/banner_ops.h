#pragma once

#include <cstddef>
#include <cstdint>

namespace flashcart_core {
class Flashcart;
}

namespace banner_ops {

constexpr std::uint32_t kSourceBannerSize = 0x840;

enum class SourceValidation {
    Valid,
    WrongSize,
    WrongVersion,
    WrongCrc,
};

SourceValidation ValidateSourceBanner(const std::uint8_t *banner, size_t size);
bool HasAvailableOperation(flashcart_core::Flashcart *cart);
bool ReadBanner(flashcart_core::Flashcart *cart, std::uint8_t *banner,
                std::uint32_t bannerSize);
bool WriteBanner(flashcart_core::Flashcart *cart, const std::uint8_t *banner,
                 std::uint32_t bannerSize);

} // namespace banner_ops
