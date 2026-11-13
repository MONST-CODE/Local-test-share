#pragma once
#include <cstdint>
#include <span>

namespace plpt {
class IRandomSource {
public:
    virtual ~IRandomSource() = default;
    virtual bool fill(std::span<std::uint8_t> out) = 0;
};
// Entropie OS uniquement. Pas d'algorithme de jeton/empreinte dans le socle.
class SecureRandom final : public IRandomSource {
public:
    bool fill(std::span<std::uint8_t> out) override;
};
} // namespace plpt
