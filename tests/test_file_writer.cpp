#include "output/AsyncFileWriter.h"

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>

TEST_CASE("unbuffered writer produces the exact bytes across buffer boundaries") {
    const auto path = std::filesystem::temp_directory_path() / "recordion_writer_test.bin";
    std::filesystem::remove(path);

    std::mt19937 rng(42);
    std::vector<uint8_t> expected;
    rec::AsyncFileWriter w;
    REQUIRE(w.open(path));
    // Odd-sized chunks so writes straddle the 4 MB buffers and the final sector is partial.
    for (size_t chunk : {1u, 4095u, 3u * 1024 * 1024 + 17, 5u * 1024 * 1024 + 1, 777u, 2u * 1024 * 1024}) {
        std::vector<uint8_t> data(chunk);
        for (auto& b : data) b = static_cast<uint8_t>(rng());
        REQUIRE(w.write(data));
        expected.insert(expected.end(), data.begin(), data.end());
    }
    CHECK(w.position() == expected.size());
    REQUIRE(w.close());

    std::ifstream in(path, std::ios::binary);
    const std::vector<uint8_t> actual{std::istreambuf_iterator<char>(in), {}};
    in.close();
    CHECK(actual.size() == expected.size());
    CHECK(actual == expected);
    std::filesystem::remove(path);
}

TEST_CASE("writer refuses to overwrite and discard deletes the partial file") {
    const auto path = std::filesystem::temp_directory_path() / "recordion_writer_discard.bin";
    std::filesystem::remove(path);
    {
        rec::AsyncFileWriter w;
        REQUIRE(w.open(path));
        std::vector<uint8_t> data(10'000, 7);
        REQUIRE(w.write(data));
        w.discard();
    }
    CHECK_FALSE(std::filesystem::exists(path));

    std::ofstream(path) << "existing";
    rec::AsyncFileWriter w;
    CHECK_FALSE(w.open(path)); // CREATE_NEW: never clobber an existing clip
    std::filesystem::remove(path);
}
