#include "ib.hpp"
#include <lzma.h>
#include <picosha2.h>
#include <tomcrypt.h>
#include <zlib.h>
#undef byte
#include "../third_party/lzham/include/lzham.h"
#include <cstring>
#include <limits>

namespace lzham {
lzham_decompress_status_t lzham_lib_decompress_memory(const lzham_decompress_params *,
                                                      lzham_uint8 *, size_t *, const lzham_uint8 *,
                                                      size_t, lzham_uint32 *);
}
namespace ib {
Bytes inflate(const Bytes &input, bool raw) {
    z_stream s{};
    if (inflateInit2(&s, raw ? -15 : 15) != Z_OK)
        fail("Cannot initialize DEFLATE decoder");
    struct End {
        z_stream *s;
        ~End() { inflateEnd(s); }
    } end{&s};
    s.next_in = const_cast<uint8_t *>(input.data());
    s.avail_in = static_cast<uInt>(input.size());
    Bytes out;
    uint8_t chunk[65536];
    int status;
    do {
        s.next_out = chunk;
        s.avail_out = sizeof(chunk);
        status = ::inflate(&s, Z_NO_FLUSH);
        size_t n = sizeof(chunk) - s.avail_out;
        if (n > max_buffer - out.size())
            fail("DEFLATE page exceeds limit");
        out.insert(out.end(), chunk, chunk + n);
        if (status != Z_OK && status != Z_STREAM_END)
            fail("Invalid DEFLATE stream");
    } while (status != Z_STREAM_END);
    if (s.avail_in)
        fail("Trailing bytes in DEFLATE stream");
    return out;
}
Bytes decode_lzma(const Bytes &input) {
    lzma_stream s = LZMA_STREAM_INIT;
    if (lzma_alone_decoder(&s, 512ULL * 1024 * 1024) != LZMA_OK)
        fail("Cannot initialize LZMA decoder");
    struct End {
        lzma_stream *s;
        ~End() { lzma_end(s); }
    } end{&s};
    s.next_in = input.data();
    s.avail_in = input.size();
    Bytes out;
    if (input.size() >= 13) {
        uint64_t length = 0;
        for (unsigned i = 0; i < 8; ++i)
            length |= uint64_t(input[5 + i]) << (8 * i);
        if (length != UINT64_MAX) {
            if (length > max_buffer)
                fail("LZMA page exceeds limit");
            out.reserve(static_cast<size_t>(length));
        }
    }
    uint8_t chunk[65536];
    lzma_ret status;
    do {
        s.next_out = chunk;
        s.avail_out = sizeof(chunk);
        status = lzma_code(&s, LZMA_FINISH);
        size_t n = sizeof(chunk) - s.avail_out;
        if (n > max_buffer - out.size())
            fail("LZMA page exceeds limit");
        out.insert(out.end(), chunk, chunk + n);
        if (status != LZMA_OK && status != LZMA_STREAM_END)
            fail("Invalid LZMA stream (status " + std::to_string(status) + ")");
    } while (status != LZMA_STREAM_END);
    if (s.avail_in)
        fail("Trailing bytes in LZMA stream");
    return out;
}
Bytes decode_lzham(const Bytes &input) {
    if (input.size() < 8)
        fail("Truncated LZHAM header");
    uint32_t length = be32(input.data());
    if (length > max_buffer)
        fail("LZHAM page exceeds limit");
    Bytes out(length ? length : 1);
    size_t count = out.size();
    // Tcllzham 1.0 framing: big-endian length and Adler32, followed by a raw
    // LZHAM 1.x stream with the wrapper's fixed dictionary/table parameters.
    lzham_decompress_params params{};
    params.m_struct_size = sizeof(params);
    params.m_dict_size_log2 = 26;
    params.m_table_max_update_interval = 64;
    params.m_table_update_interval_slow_rate = 64;
    lzham_uint32 checksum = 0;
    auto status = lzham::lzham_lib_decompress_memory(&params, out.data(), &count, input.data() + 8,
                                                     input.size() - 8, &checksum);
    if (status != LZHAM_DECOMP_STATUS_SUCCESS || count != length)
        fail("Invalid LZHAM stream (status " + std::to_string(status) + ")");
    if (checksum != be32(input.data() + 4))
        fail("LZHAM checksum mismatch");
    out.resize(count);
    return out;
}
Bytes sha256(const Bytes &data) {
    Bytes out(32);
    picosha2::hash256(data.begin(), data.end(), out.begin(), out.end());
    return out;
}
Bytes twofish(const Bytes &key, const Bytes &input, Bytes &iv, bool encrypt) {
    if (key.size() != 32 || iv.size() != 16 || input.size() % 16 || input.size() > max_buffer)
        fail("Invalid Twofish key, IV, or block length");
    symmetric_key k{};
    if (twofish_setup(key.data(), 32, 0, &k) != CRYPT_OK)
        fail("Cannot initialize Twofish");
    Bytes out(input.size());
    for (size_t offset = 0; offset < input.size(); offset += 16) {
        uint8_t block[16];
        if (encrypt) {
            for (size_t j = 0; j < 16; ++j)
                block[j] = input[offset + j] ^ iv[j];
            if (twofish_ecb_encrypt(block, out.data() + offset, &k) != CRYPT_OK)
                fail("Twofish encryption failed");
            iv.assign(out.begin() + offset, out.begin() + offset + 16);
        } else {
            if (twofish_ecb_decrypt(input.data() + offset, block, &k) != CRYPT_OK)
                fail("Twofish decryption failed");
            for (size_t j = 0; j < 16; ++j)
                out[offset + j] = block[j] ^ iv[j];
            iv.assign(input.begin() + offset, input.begin() + offset + 16);
        }
    }
    return out;
}
void Crypto::initialize(const std::string &password, const Bytes &info) {
    Cursor c{info};
    uint32_t times = c.u32();
    auto iv = c.take(16), passwordkey = c.take(32), encryptedkey = c.take(64),
         expected = c.take(32);
    auto encryptedivs = c.take(info.size() - c.pos);
    if (times > 1000000)
        fail("Password iteration count exceeds limit");
    // Tcl 8.5's binary conversion hashes the low byte of each UTF-16 code unit,
    // rather than the UTF-8 representation received from the command line.
    Bytes password_bytes;
    for (size_t p = 0; p < password.size();) {
        uint32_t cp = static_cast<uint8_t>(password[p++]);
        unsigned continuation = 0;
        if (cp >= 0xc2 && cp <= 0xdf) {
            cp &= 31;
            continuation = 1;
        } else if (cp >= 0xe0 && cp <= 0xef) {
            cp &= 15;
            continuation = 2;
        } else if (cp >= 0xf0 && cp <= 0xf4) {
            cp &= 7;
            continuation = 3;
        } else if (cp >= 0x80)
            fail("Password is not valid UTF-8");
        unsigned width = continuation;
        while (continuation--) {
            if (p == password.size())
                fail("Password is not valid UTF-8");
            auto c = static_cast<uint8_t>(password[p++]);
            if ((c & 0xc0) != 0x80)
                fail("Password is not valid UTF-8");
            cp = (cp << 6) | (c & 63);
        }
        if ((width == 1 && cp < 0x80) || (width == 2 && cp < 0x800) ||
            (width == 3 && cp < 0x10000) || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff))
            fail("Password is not valid UTF-8");
        if (cp < 0x10000)
            password_bytes.push_back(static_cast<uint8_t>(cp));
        else {
            cp -= 0x10000;
            password_bytes.push_back(static_cast<uint8_t>(0xd800 + (cp >> 10)));
            password_bytes.push_back(static_cast<uint8_t>(0xdc00 + (cp & 1023)));
        }
    }
    auto derived = sha256(password_bytes);
    for (uint32_t i = 0; i < times; ++i)
        derived = twofish(passwordkey, derived, iv, true);
    derived = sha256(derived);
    Bytes zero(16);
    auto recovered = twofish(derived, encryptedkey, zero, false);
    key_ = Bytes(recovered.begin() + 32, recovered.end());
    for (uint32_t i = 0; i < times; i += 64) {
        zero.assign(16, 0);
        encryptedivs = twofish(key_, encryptedivs, zero, false);
    }
    if (encryptedivs.size() < 32)
        fail("Corrupt password metadata");
    ivs_.assign(encryptedivs.begin() + 32, encryptedivs.end());
    if (sha256(ivs_) != expected)
        fail("Invalid payload password");
    if (ivs_.size() != 4096)
        fail("Invalid payload IV table size");
}
Bytes Crypto::decrypt(const Bytes &input) const {
    if (key_.size() != 32 || ivs_.size() != 4096)
        fail("Password metadata must be initialized before encrypted pages");
    Cursor c{input};
    uint32_t expected = c.u32();
    size_t index = c.byte();
    Bytes iv(ivs_.begin() + index * 16, ivs_.begin() + (index + 1) * 16);
    auto recovered = twofish(key_, c.take(input.size() - c.pos), iv, false);
    if (crc32(0, recovered.data(), static_cast<uInt>(recovered.size())) != expected)
        fail("Encrypted payload CRC mismatch");
    if (recovered.size() < 36)
        fail("Truncated encrypted payload");
    Cursor payload{recovered, 32};
    auto length = payload.u32();
    return payload.take(length);
}
} // namespace ib
