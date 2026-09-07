#include "storage/reed_solomon.hpp"
#include <numeric>
#include <algorithm>

namespace engine::storage {

ReedSolomonEncoder::ReedSolomonEncoder(size_t data_shards, size_t parity_shards)
    : m_k(data_shards), m_p(parity_shards), m_gf_exp(512), m_gf_log(256) {
    init_galois_field();
    build_generator_poly();
}

void ReedSolomonEncoder::init_galois_field() {
    uint8_t x = 1;
    for (size_t i = 0; i < 255; ++i) {
        m_gf_exp[i] = x;
        m_gf_exp[i + 255] = x;
        m_gf_log[x] = static_cast<uint8_t>(i);
        x = static_cast<uint8_t>(x ^ (x << 1) ^ ((x & 0x80) ? 0x11D : 0)); // Primitive polynomial 0x11D
    }
}

uint8_t ReedSolomonEncoder::gf_mul(uint8_t a, uint8_t b) const noexcept {
    if (a == 0 || b == 0) return 0;
    return m_gf_exp[m_gf_log[a] + m_gf_log[b]];
}

// Генераторный полином g(x) = (x - alpha^0)(x - alpha^1)...(x - alpha^(p-1))
void ReedSolomonEncoder::build_generator_poly() {
    m_gen_poly.assign(m_p + 1, 0);
    m_gen_poly[0] = 1;

    for (size_t i = 0; i < m_p; ++i) {
        uint8_t root = m_gf_exp[i];
        for (size_t j = i + 1; j > 0; --j) {
            m_gen_poly[j] = gf_add(m_gen_poly[j], gf_mul(m_gen_poly[j - 1], root));
        }
    }
}

// Систематическое кодирование через деление D(x) * x^p на G(x)
void ReedSolomonEncoder::encode(std::span<const uint8_t> data, std::vector<uint8_t>& out_parity) const {
    out_parity.assign(m_p, 0);

    for (uint8_t byte : data) {
        uint8_t feedback = gf_add(byte, out_parity[0]);
        for (size_t j = 0; j < m_p - 1; ++j) {
            out_parity[j] = gf_add(out_parity[j + 1], gf_mul(feedback, m_gen_poly[m_p - j]));
        }
        out_parity[m_p - 1] = gf_mul(feedback, m_gen_poly[1]);
    }
}

bool ReedSolomonEncoder::verify(std::span<const uint8_t> data, std::span<const uint8_t> parity) const {
    if (parity.size() != m_p) return false;

    std::vector<uint8_t> expected_parity(m_p, 0);
    encode(data, expected_parity);

    return std::equal(expected_parity.begin(), expected_parity.end(), parity.begin());
}

bool ReedSolomonEncoder::repair(std::span<uint8_t> data, std::span<const uint8_t> parity) const {
    if (verify(data, parity)) return true;

    // Восстановление 1-байтовой ошибки
    for (size_t i = 0; i < data.size(); ++i) {
        uint8_t original = data[i];
        for (int candidate = 0; candidate <= 255; ++candidate) {
            if (candidate == original) continue;
            data[i] = static_cast<uint8_t>(candidate);
            if (verify(data, parity)) {
                return true; 
            }
        }
        data[i] = original; 
    }
    return false;
}

} // namespace engine::storage
