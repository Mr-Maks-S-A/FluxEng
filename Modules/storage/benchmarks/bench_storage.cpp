#include "storage/reed_solomon.hpp"
#include "storage/recoverable_file.hpp"
#include <chrono>
#include <iostream>
#include <numeric>
#include <filesystem>

void bench_rs_encoding_speed() {
    constexpr size_t PAYLOAD_SIZE = 4096; // 4 KB
    constexpr size_t ITERATIONS = 10000;
    
    std::vector<uint8_t> dummy_payload(PAYLOAD_SIZE);
    std::iota(dummy_payload.begin(), dummy_payload.end(), 0);

    engine::storage::ReedSolomonEncoder encoder(1, 16);
    std::vector<uint8_t> parity;

    auto start = std::chrono::high_resolution_clock::now();

    for (size_t i = 0; i < ITERATIONS; ++i) {
        encoder.encode(dummy_payload, parity);
    }

    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::milli> duration = end - start;

    double total_bytes = static_cast<double>(PAYLOAD_SIZE * ITERATIONS);
    double mb_per_sec = (total_bytes / (1024.0 * 1024.0)) / (duration.count() / 1000.0);

    std::cout << "[BENCHMARK] RS Encoding Speed (4KB payload, 16 Parity Bytes):\n"
              << "  Total Time: " << duration.count() << " ms\n"
              << "  Throughput: " << mb_per_sec << " MB/s\n\n";
}

void bench_file_write_throughput() {
    const std::string bench_file = "bench_write.bin";
    if (std::filesystem::exists(bench_file)) {
        std::filesystem::remove(bench_file);
    }

    constexpr size_t RECORD_SIZE = 1024; // 1 KB (типичный размер пакета событий за кадр)
    constexpr size_t RECORDS_COUNT = 5000;

    std::vector<uint8_t> payload(RECORD_SIZE, 0xAB);
    
    {
        engine::storage::RecoverableFile rec_file(bench_file, 16);
        auto start = std::chrono::high_resolution_clock::now();

        for (size_t i = 0; i < RECORDS_COUNT; ++i) {
            rec_file.write_record(payload);
        }

        auto end = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double, std::milli> duration = end - start;

        std::cout << "[BENCHMARK] RecoverableFile Sequential Write (1KB record, 16 Parity Bytes):\n"
                  << "  Total Time: " << duration.count() << " ms\n"
                  << "  Records/sec: " << (RECORDS_COUNT / (duration.count() / 1000.0)) << " rec/s\n\n";
    }

    std::filesystem::remove(bench_file);
}

int main() {
    std::cout << "--- STORAGE BENCHMARKS ---\n";
    bench_rs_encoding_speed();
    bench_file_write_throughput();
    return 0;
}
