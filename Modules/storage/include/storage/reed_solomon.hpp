#pragma once
#include <vector>
#include <cstdint>
#include <span>

namespace engine::storage {

class ReedSolomonEncoder {
public:
    ReedSolomonEncoder(size_t data_shards, size_t parity_shards);

    void encode(std::span<const uint8_t> data, std::vector<uint8_t>& out_parity) const;
    bool verify(std::span<const uint8_t> data, std::span<const uint8_t> parity) const;
    bool repair(std::span<uint8_t> data, std::span<const uint8_t> parity) const;

private:
    size_t m_k;
    size_t m_p;
    std::vector<uint8_t> m_gf_exp;
    std::vector<uint8_t> m_gf_log;
    std::vector<uint8_t> m_gen_poly; // Генераторный полином

    void init_galois_field();
    void build_generator_poly();
    
    uint8_t gf_mul(uint8_t a, uint8_t b) const noexcept;
    uint8_t gf_add(uint8_t a, uint8_t b) const noexcept { return a ^ b; }
};

} // namespace engine::storage
