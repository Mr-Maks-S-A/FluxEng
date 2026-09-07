#pragma once
#include "storage/reed_solomon.hpp"
#include <fstream>
#include <string>
#include <vector>
#include <span>

namespace engine::storage {

#pragma pack(push, 1)
struct RecordHeader {
    uint32_t magic{0x45564E54}; // 'EVNT'
    uint32_t payload_size{0};
    uint32_t parity_size{0};
};
#pragma pack(pop)

class RecoverableFile {
public:
    explicit RecoverableFile(const std::string& path, size_t parity_bytes = 16);
    ~RecoverableFile();

    void write_record(std::span<const uint8_t> payload);
    bool read_and_recover_all(std::vector<std::vector<uint8_t>>& out_records);

private:
    std::string m_path;
    size_t m_parity_bytes;
    ReedSolomonEncoder m_rs_encoder;
    
    std::ofstream m_file;
    std::vector<uint8_t> m_parity_buffer;
};

} // namespace engine::storage
