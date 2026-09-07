#include "storage/reed_solomon.hpp"
#include "storage/recoverable_file.hpp"
#include <cassert>
#include <iostream>
#include <filesystem>

void test_rs_encode_verify() {
    engine::storage::ReedSolomonEncoder encoder(1, 8);
    std::vector<uint8_t> data = {0x10, 0x20, 0x30, 0x40, 0x50};
    std::vector<uint8_t> parity;

    encoder.encode(data, parity);
    assert(parity.size() == 8);
    assert(encoder.verify(data, parity) == true);

    std::cout << "[PASSED] RS Encode & Verify Test\n";
}

void test_rs_single_byte_corruption_repair() {
    engine::storage::ReedSolomonEncoder encoder(1, 8);
    std::vector<uint8_t> data = {0xA1, 0xB2, 0xC3, 0xD4};
    std::vector<uint8_t> parity;
    encoder.encode(data, parity);

    // Вносим искусственное повреждение в один байт
    data[2] ^= 0xFF; 
    assert(encoder.verify(data, parity) == false);

    // Восстанавливаем
    bool repaired = encoder.repair(data, parity);
    assert(repaired == true);
    assert(data[2] == 0xC3);

    std::cout << "[PASSED] RS Repair Single Byte Corruption Test\n";
}

void test_recoverable_file_io() {
    const std::string test_filename = "test_flight_log.bin";
    if (std::filesystem::exists(test_filename)) {
        std::filesystem::remove(test_filename);
    }

    {
        engine::storage::RecoverableFile rec_file(test_filename, 8);
        std::vector<uint8_t> record1 = {1, 2, 3, 4, 5};
        std::vector<uint8_t> record2 = {9, 8, 7, 6, 5, 4};
        rec_file.write_record(record1);
        rec_file.write_record(record2);
    }

    // Чтение и валидация
    engine::storage::RecoverableFile rec_file_reader(test_filename, 8);
    std::vector<std::vector<uint8_t>> records;
    bool ok = rec_file_reader.read_and_recover_all(records);

    assert(ok == true);
    assert(records.size() == 2);
    assert(records[0] == std::vector<uint8_t>({1, 2, 3, 4, 5}));

    std::filesystem::remove(test_filename);
    std::cout << "[PASSED] RecoverableFile IO & Parse Test\n";
}

int main() {
    std::cout << "--- RUNNING STORAGE TESTS ---\n";
    test_rs_encode_verify();
    test_rs_single_byte_corruption_repair();
    test_recoverable_file_io();
    std::cout << "All Storage Tests Passed!\n";
    return 0;
}
