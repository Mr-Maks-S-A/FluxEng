#include <EventLog/ErasureCode.hpp>

#include <Math/Assert.hpp>

#include <algorithm>
#include <array>
#include <cstdint>

namespace EventLog {

namespace gf256 {
namespace {

struct Tables {
    std::array<unsigned char, 512> exp{};  // удвоенная таблица: не нужен % 255 при сложении логарифмов
    std::array<unsigned char, 256> log{};
    std::array<std::array<unsigned char, 256>, 256> product{}; // 64 КиБ: умножение одним обращением

    Tables() {
        unsigned x = 1;
        for (int i = 0; i < 255; ++i) {
            exp[static_cast<std::size_t>(i)] = static_cast<unsigned char>(x);
            log[x] = static_cast<unsigned char>(i);
            x <<= 1;
            if (x & 0x100u) x ^= 0x11Du; // примитивный полином x⁸ + x⁴ + x³ + x² + 1
        }
        for (int i = 255; i < 512; ++i) exp[static_cast<std::size_t>(i)] = exp[static_cast<std::size_t>(i - 255)];
        for (int a = 1; a < 256; ++a) {
            for (int b = 1; b < 256; ++b) {
                product[static_cast<std::size_t>(a)][static_cast<std::size_t>(b)] = exp[static_cast<std::size_t>(log[static_cast<std::size_t>(a)]) + log[static_cast<std::size_t>(b)]];
            }
        }
    }
};

const Tables& tables() {
    static const Tables t; // потокобезопасная инициализация при первом использовании
    return t;
}

} // namespace

unsigned char mul(unsigned char a, unsigned char b) noexcept { return tables().product[a][b]; }

unsigned char inv(unsigned char a) noexcept {
    if (a == 0) return 0;
    const Tables& t = tables();
    return t.exp[255 - t.log[a]];
}

void mul_add(std::span<std::byte> dst, std::span<const std::byte> src, unsigned char coefficient) noexcept {
    if (coefficient == 0) return;
    const auto& row = tables().product[coefficient];
    for (std::size_t i = 0; i < dst.size(); ++i) dst[i] ^= static_cast<std::byte>(row[static_cast<std::uint8_t>(src[i])]);
}

} // namespace gf256

ErasureCode::ErasureCode(int data_blocks, int parity_blocks) : m_k(data_blocks), m_m(parity_blocks) {
    FLUX_ASSERT(data_blocks >= 1 && parity_blocks >= 1 && data_blocks + parity_blocks <= 256, "ErasureCode: нужно k ≥ 1, m ≥ 1, k + m ≤ 256");
    // Матрица Коши C[j][i] = 1 / (x_j + y_i), x_j = j, y_i = m + i: все x и y различны, поэтому любая квадратная
    // подматрица [единичная; C] обратима — ровно это и даёт восстановление любых m стираний.
    m_matrix.resize(static_cast<std::size_t>(m_m) * static_cast<std::size_t>(m_k));
    for (int j = 0; j < m_m; ++j)
        for (int i = 0; i < m_k; ++i) m_matrix[static_cast<std::size_t>(j * m_k + i)] = gf256::inv(static_cast<unsigned char>(j ^ (m_m + i)));
}

void ErasureCode::encode(std::span<const std::span<const std::byte>> data, std::span<const std::span<std::byte>> parity) const {
    FLUX_ASSERT(static_cast<int>(data.size()) == m_k && static_cast<int>(parity.size()) == m_m, "ErasureCode::encode: неверное число блоков");
    for (int j = 0; j < m_m; ++j) {
        std::ranges::fill(parity[static_cast<std::size_t>(j)], std::byte{0});
        for (int i = 0; i < m_k; ++i) gf256::mul_add(parity[static_cast<std::size_t>(j)], data[static_cast<std::size_t>(i)], m_matrix[static_cast<std::size_t>(j * m_k + i)]);
    }
}

bool ErasureCode::reconstruct(std::span<const std::span<std::byte>> blocks, std::span<const bool> present) const {
    const int n = m_k + m_m;
    FLUX_ASSERT(static_cast<int>(blocks.size()) == n && static_cast<int>(present.size()) == n, "ErasureCode::reconstruct: нужно k + m блоков");
    std::vector<int> missing_data, missing_parity;
    for (int b = 0; b < n; ++b) {
        if (present[static_cast<std::size_t>(b)]) continue;
        (b < m_k ? missing_data : missing_parity).push_back(b);
    }
    if (static_cast<int>(missing_data.size() + missing_parity.size()) > m_m) return false;
    if (missing_data.empty() && missing_parity.empty()) return true;

    if (!missing_data.empty()) {
        // k уцелевших блоков: сначала данные, затем чётность. Строка матрицы: единичная для данных, Коши для чётности.
        std::vector<int> chosen;
        for (int b = 0; b < n && static_cast<int>(chosen.size()) < m_k; ++b) {
            if (present[static_cast<std::size_t>(b)]) chosen.push_back(b);
        }
        const auto k = static_cast<std::size_t>(m_k);
        std::vector<unsigned char> a(k * k, 0), inverse(k * k, 0);
        for (std::size_t r = 0; r < k; ++r) {
            const int b = chosen[r];
            if (b < m_k) a[r * k + static_cast<std::size_t>(b)] = 1;
            else for (std::size_t i = 0; i < k; ++i) a[r * k + i] = m_matrix[static_cast<std::size_t>(b - m_k) * k + i];
            inverse[r * k + r] = 1;
        }
        // Гаусс—Жордан над GF(2⁸): [a | единичная] → [единичная | a⁻¹].
        for (std::size_t col = 0; col < k; ++col) {
            std::size_t pivot = col;
            while (pivot < k && a[pivot * k + col] == 0) ++pivot;
            if (pivot == k) return false; // у матрицы Коши этого не бывает; защита от неверных данных
            if (pivot != col)
                for (std::size_t i = 0; i < k; ++i) {
                    std::swap(a[pivot * k + i], a[col * k + i]);
                    std::swap(inverse[pivot * k + i], inverse[col * k + i]);
                }
            const unsigned char scale = gf256::inv(a[col * k + col]);
            for (std::size_t i = 0; i < k; ++i) {
                a[col * k + i] = gf256::mul(a[col * k + i], scale);
                inverse[col * k + i] = gf256::mul(inverse[col * k + i], scale);
            }
            for (std::size_t r = 0; r < k; ++r) {
                const unsigned char factor = a[r * k + col];
                if (r == col || factor == 0) continue;
                for (std::size_t i = 0; i < k; ++i) {
                    a[r * k + i] ^= gf256::mul(factor, a[col * k + i]);
                    inverse[r * k + i] ^= gf256::mul(factor, inverse[col * k + i]);
                }
            }
        }
        // data_d = Σ inverse[d][r] · chosen_block_r
        for (const int d : missing_data) {
            std::ranges::fill(blocks[static_cast<std::size_t>(d)], std::byte{0});
            for (std::size_t r = 0; r < k; ++r) gf256::mul_add(blocks[static_cast<std::size_t>(d)], blocks[static_cast<std::size_t>(chosen[r])], inverse[static_cast<std::size_t>(d) * k + r]);
        }
    }
    // Потерянная чётность — пересчётом из (теперь полных) данных.
    for (const int p : missing_parity) {
        const auto j = static_cast<std::size_t>(p - m_k);
        std::ranges::fill(blocks[static_cast<std::size_t>(p)], std::byte{0});
        for (std::size_t i = 0; i < static_cast<std::size_t>(m_k); ++i) gf256::mul_add(blocks[static_cast<std::size_t>(p)], blocks[i], m_matrix[j * static_cast<std::size_t>(m_k) + i]);
    }
    return true;
}

} // namespace EventLog
