#include "storage/recoverable_file.hpp"

namespace engine::storage {

RecoverableFile::RecoverableFile(const std::string& path, size_t parity_bytes)
    : m_path(path), 
      m_parity_bytes(parity_bytes), 
      m_rs_encoder(1, parity_bytes) {
    m_file.open(m_path, std::ios::binary | std::ios::app);
}

RecoverableFile::~RecoverableFile() {
    if (m_file.is_open()) {
        m_file.flush();
        m_file.close();
    }
}

void RecoverableFile::write_record(std::span<const uint8_t> payload) {
    m_rs_encoder.encode(payload, m_parity_buffer);

    RecordHeader header;
    header.payload_size = static_cast<uint32_t>(payload.size());
    header.parity_size = static_cast<uint32_t>(m_parity_buffer.size());

    m_file.write(reinterpret_cast<const char*>(&header), sizeof(header));
    m_file.write(reinterpret_cast<const char*>(payload.data()), payload.size());
    m_file.write(reinterpret_cast<const char*>(m_parity_buffer.data()), m_parity_buffer.size());
}

bool RecoverableFile::read_and_recover_all(std::vector<std::vector<uint8_t>>& out_records) {
    std::ifstream file(m_path, std::ios::binary);
    if (!file.is_open()) return false;

    while (file.peek() != EOF) {
        RecordHeader header;
        file.read(reinterpret_cast<char*>(&header), sizeof(header));
        if (header.magic != 0x45564E54) return false;

        std::vector<uint8_t> payload(header.payload_size);
        std::vector<uint8_t> parity(header.parity_size);

        file.read(reinterpret_cast<char*>(payload.data()), header.payload_size);
        file.read(reinterpret_cast<char*>(parity.data()), header.parity_size);

        if (!m_rs_encoder.repair(payload, parity)) {
            return false;
        }

        out_records.push_back(std::move(payload));
    }
    return true;
}

} // namespace engine::storage
