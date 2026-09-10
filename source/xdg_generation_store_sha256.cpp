#include "xdg_generation_store.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <unistd.h>

// Shared byte/FD SHA-256 implementation used by the store, Slice 4 archive
// evidence, and the privileged artifact transport. No store I/O lives here.
namespace {
constexpr std::size_t CONTENT_DIGEST_HEX_SIZE = 64;

std::uint32_t sha256_rotr(std::uint32_t value, unsigned bits) {
    return (value >> bits) | (value << (32U - bits));
}

void sha256_process_block(
    std::array<std::uint32_t, 8>& hash, const std::uint8_t* block) {
    static constexpr std::array<std::uint32_t, 64> round_constants = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b,
        0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
        0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7,
        0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
        0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152,
        0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
        0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
        0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819,
        0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
        0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
        0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
        0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    std::array<std::uint32_t, 64> schedule{};
    for(std::size_t i = 0; i < 16; ++i) {
        schedule[i] =
            (static_cast<std::uint32_t>(block[i * 4]) << 24) |
            (static_cast<std::uint32_t>(block[i * 4 + 1]) << 16) |
            (static_cast<std::uint32_t>(block[i * 4 + 2]) << 8) |
            static_cast<std::uint32_t>(block[i * 4 + 3]);
    }
    for(std::size_t i = 16; i < 64; ++i) {
        const std::uint32_t s0 = sha256_rotr(schedule[i - 15], 7) ^
                                 sha256_rotr(schedule[i - 15], 18) ^
                                 (schedule[i - 15] >> 3);
        const std::uint32_t s1 = sha256_rotr(schedule[i - 2], 17) ^
                                 sha256_rotr(schedule[i - 2], 19) ^
                                 (schedule[i - 2] >> 10);
        schedule[i] = schedule[i - 16] + s0 + schedule[i - 7] + s1;
    }
    std::uint32_t a = hash[0];
    std::uint32_t b = hash[1];
    std::uint32_t c = hash[2];
    std::uint32_t d = hash[3];
    std::uint32_t e = hash[4];
    std::uint32_t f = hash[5];
    std::uint32_t g = hash[6];
    std::uint32_t h = hash[7];
    for(std::size_t i = 0; i < 64; ++i) {
        const std::uint32_t s1 = sha256_rotr(e, 6) ^ sha256_rotr(e, 11) ^
                                 sha256_rotr(e, 25);
        const std::uint32_t ch =
            (e & f) ^ (static_cast<std::uint32_t>(~e) & g);
        const std::uint32_t temp1 =
            h + s1 + ch + round_constants[i] + schedule[i];
        const std::uint32_t s0 = sha256_rotr(a, 2) ^ sha256_rotr(a, 13) ^
                                 sha256_rotr(a, 22);
        const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const std::uint32_t temp2 = s0 + maj;
        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }
    hash[0] += a;
    hash[1] += b;
    hash[2] += c;
    hash[3] += d;
    hash[4] += e;
    hash[5] += f;
    hash[6] += g;
    hash[7] += h;
}

class ContentSha256 final {
public:
    void update(std::string_view data) {
        if(data.size() >
           std::numeric_limits<std::uint64_t>::max() - total_size_) {
            throw std::length_error("SHA-256 input length overflow.");
        }
        total_size_ += static_cast<std::uint64_t>(data.size());
        while(!data.empty()) {
            const std::size_t copied = std::min(
                data.size(), block_.size() - block_used_);
            for(std::size_t index = 0; index < copied; ++index) {
                block_[block_used_ + index] =
                    static_cast<std::uint8_t>(
                        static_cast<unsigned char>(data[index]));
            }
            block_used_ += copied;
            data.remove_prefix(copied);
            if(block_used_ == block_.size()) {
                sha256_process_block(hash_, block_.data());
                block_used_ = 0;
            }
        }
    }

    [[nodiscard]] std::string finish() {
        const std::uint64_t bit_length = total_size_ * 8U;
        consume_padding_byte(0x80U);
        if(block_used_ > 56) {
            while(block_used_ != 0)
                consume_padding_byte(0);
        }
        while(block_used_ < 56)
            consume_padding_byte(0);
        for(std::size_t index = 0; index < 8; ++index) {
            consume_padding_byte(static_cast<std::uint8_t>(
                bit_length >> (8U * (7U - index))));
        }

        std::string hex(CONTENT_DIGEST_HEX_SIZE, '0');
        constexpr char digits[] = "0123456789abcdef";
        for(std::size_t index = 0; index < hash_.size(); ++index) {
            const std::uint32_t word = hash_[index];
            hex[index * 8] = digits[(word >> 28) & 0x0fU];
            hex[index * 8 + 1] = digits[(word >> 24) & 0x0fU];
            hex[index * 8 + 2] = digits[(word >> 20) & 0x0fU];
            hex[index * 8 + 3] = digits[(word >> 16) & 0x0fU];
            hex[index * 8 + 4] = digits[(word >> 12) & 0x0fU];
            hex[index * 8 + 5] = digits[(word >> 8) & 0x0fU];
            hex[index * 8 + 6] = digits[(word >> 4) & 0x0fU];
            hex[index * 8 + 7] = digits[word & 0x0fU];
        }
        return hex;
    }

private:
    void consume_padding_byte(std::uint8_t byte) noexcept {
        block_[block_used_++] = byte;
        if(block_used_ == block_.size()) {
            sha256_process_block(hash_, block_.data());
            block_used_ = 0;
        }
    }

    std::array<std::uint32_t, 8> hash_ = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::array<std::uint8_t, 64> block_{};
    std::size_t block_used_ = 0;
    std::uint64_t total_size_ = 0;
};

} // namespace

std::string xdg_generation_store_raw_contents_sha256(
    std::string_view raw_contents) {
    ContentSha256 digest;
    digest.update(raw_contents);
    return digest.finish();
}

std::string xdg_generation_store_file_descriptor_sha256(
    int descriptor,
    std::uintmax_t expected_size,
    std::uintmax_t maximum_size) {
    if(descriptor < 0 || expected_size > maximum_size ||
       expected_size > static_cast<std::uintmax_t>(
                           std::numeric_limits<off_t>::max())) {
        throw std::invalid_argument(
            "SHA-256 descriptor input exceeds its fixed boundary.");
    }
    ContentSha256 digest;
    std::array<char, 64U * 1024U> buffer{};
    std::uintmax_t offset = 0;
    while(offset < expected_size) {
        const std::size_t requested = static_cast<std::size_t>(std::min(
            expected_size - offset,
            static_cast<std::uintmax_t>(buffer.size())));
        ssize_t read_size;
        do {
            read_size = ::pread(
                descriptor, buffer.data(), requested,
                static_cast<off_t>(offset));
        } while(read_size < 0 && errno == EINTR);
        if(read_size < 0) {
            throw std::system_error(
                errno, std::generic_category(),
                "Failed to read SHA-256 descriptor input");
        }
        if(read_size == 0) {
            throw std::runtime_error(
                "SHA-256 descriptor input ended before its expected size.");
        }
        digest.update(std::string_view(
            buffer.data(), static_cast<std::size_t>(read_size)));
        offset += static_cast<std::uintmax_t>(read_size);
    }
    return digest.finish();
}
