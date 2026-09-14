#ifndef COMMON_SHA1_H
#define COMMON_SHA1_H

// Minimal, dependency-free, streaming SHA-1 implementation (RFC 3174).
//
// The assignment forbids "high-level" / external libraries (and we'd rather
// not depend on OpenSSL being present & linkable on the grading machine), so
// we implement the well-known SHA-1 algorithm directly. It is used for both
// per-piece (512KB) integrity hashes and whole-file hashes. The streaming
// update() API lets callers hash a 1GB file 512KB at a time instead of
// loading it entirely into memory.

#include <cstdint>
#include <cstring>
#include <string>
#include <sstream>
#include <iomanip>
#include <algorithm>

class SHA1 {
public:
    SHA1() { reset(); }

    void reset() {
        h0 = 0x67452301; h1 = 0xEFCDAB89; h2 = 0x98BADCFE;
        h3 = 0x10325476; h4 = 0xC3D2E1F0;
        bitlen = 0;
        bufferLen = 0;
    }

    void update(const uint8_t *data, size_t len) {
        bitlen += static_cast<uint64_t>(len) * 8;
        while (len > 0) {
            size_t toCopy = std::min(len, (size_t)64 - bufferLen);
            memcpy(buffer + bufferLen, data, toCopy);
            bufferLen += toCopy;
            data += toCopy;
            len -= toCopy;
            if (bufferLen == 64) {
                processBlock(buffer);
                bufferLen = 0;
            }
        }
    }

    void update(const std::string &s) {
        update(reinterpret_cast<const uint8_t *>(s.data()), s.size());
    }

    // Finalizes the digest and returns it as a 40-char lowercase hex string.
    // Do not call update() again after this without reset() first.
    std::string finalHex() {
        uint64_t savedBitlen = bitlen;

        uint8_t pad = 0x80;
        appendRaw(&pad, 1);
        uint8_t zero = 0x00;
        while (bufferLen != 56) {
            appendRaw(&zero, 1);
        }
        uint8_t lenBytes[8];
        for (int i = 0; i < 8; i++) {
            lenBytes[i] = (uint8_t)(savedBitlen >> (56 - 8 * i));
        }
        appendRaw(lenBytes, 8); // brings bufferLen to exactly 64 -> processed

        std::ostringstream oss;
        uint32_t hs[5] = {h0, h1, h2, h3, h4};
        for (int i = 0; i < 5; i++) {
            oss << std::hex << std::setw(8) << std::setfill('0') << hs[i];
        }
        return oss.str();
    }

    static std::string hash(const uint8_t *data, size_t len) {
        SHA1 sha;
        sha.update(data, len);
        return sha.finalHex();
    }
    static std::string hash(const std::string &s) {
        return hash(reinterpret_cast<const uint8_t *>(s.data()), s.size());
    }

private:
    uint32_t h0, h1, h2, h3, h4;
    uint64_t bitlen;
    uint8_t buffer[64];
    size_t bufferLen;

    // Like update() but does not touch bitlen (used only for padding, whose
    // bytes must not count toward the message length that gets encoded).
    void appendRaw(const uint8_t *data, size_t len) {
        while (len > 0) {
            size_t toCopy = std::min(len, (size_t)64 - bufferLen);
            memcpy(buffer + bufferLen, data, toCopy);
            bufferLen += toCopy;
            data += toCopy;
            len -= toCopy;
            if (bufferLen == 64) {
                processBlock(buffer);
                bufferLen = 0;
            }
        }
    }

    static uint32_t rol(uint32_t v, int bits) {
        return (v << bits) | (v >> (32 - bits));
    }

    void processBlock(const uint8_t *p) {
        uint32_t w[80];
        for (int i = 0; i < 16; i++) {
            w[i] = (uint32_t(p[i * 4]) << 24) | (uint32_t(p[i * 4 + 1]) << 16) |
                   (uint32_t(p[i * 4 + 2]) << 8) | uint32_t(p[i * 4 + 3]);
        }
        for (int i = 16; i < 80; i++) {
            w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        }
        uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20) { f = (b & c) | ((~b) & d); k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6; }
            uint32_t temp = rol(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rol(b, 30); b = a; a = temp;
        }
        h0 += a; h1 += b; h2 += c; h3 += d; h4 += e;
    }
};

#endif // COMMON_SHA1_H
