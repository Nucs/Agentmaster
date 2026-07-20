// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster: SHA-256 (FIPS 180-4) over a byte buffer -- pure, header-only, dependency-free.
//
// WHY HAND-ROLLED (the Base64Encode precedent in ClaudeSpawn.cpp): the engine TUs are plain C++
// compiled three ways -- the app lib (TerminalAppLib.vcxproj), the standalone test harness
// (AgentMaster/tests/run-m5-tests.bat) and the CLI (AgentMaster/cli) -- so a bcrypt.lib /
// crypt32.lib dependency would have to be threaded through all three build wirings. ~90 lines of
// FIPS-180-4 answer it with none, and being header-only it needs no vcxproj entry either (the
// PromptAnchor.h / PendingInput.h idiom).
//
// WHAT IT IS FOR: the /handover + /handover-here shipped command DEFINITIONS (COMMANDS.md section 6)
// are auto-upgraded only when the on-disk file is byte-identical to a version WE shipped -- i.e.
// pristine, never user-edited. Recognizing "a version we shipped" needs only an IDENTITY of those
// bytes, not the bytes themselves, so the shipped history is a list of SHA-256 digests
// (ShippedHandoverCommandHashes) instead of every superseded document's full text.
//
// NOT a security primitive here: it is used for content IDENTITY (did this file come out of one of
// our writes, unmodified?), where a 256-bit digest is overwhelming. It is a plain, unoptimized
// reference implementation -- correctness and portability over speed (the inputs are a few KB, read
// once at engine init).
//
// Verified against the FIPS 180-4 / NIST vectors plus block-boundary lengths in the engine harness
// (tests_commands.cpp), including the 55/56/63/64/65-byte padding edges and the 1,000,000 x 'a'
// multi-block case.

#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace Agentmaster
{
    // The raw 32-byte digest.
    using Sha256Digest = std::array<unsigned char, 32>;

    namespace detail
    {
        // FIPS 180-4 section 4.2.2 round constants (first 32 bits of the fractional parts of the
        // cube roots of the first 64 primes).
        inline constexpr uint32_t kSha256K[64] = {
            0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
            0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
            0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
            0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
            0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
            0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
            0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
            0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
        };

        // Rotate right. `n` is always in [1, 31] at every call site below (a 0 or 32 shift would be UB).
        constexpr uint32_t Sha256Ror(uint32_t v, unsigned int n) noexcept
        {
            return (v >> n) | (v << (32u - n));
        }
    }

    // The digest of `len` bytes at `data`. A null/empty buffer hashes the empty message
    // (e3b0c442...b855), which is what a zero-length read should produce.
    inline Sha256Digest Sha256(const void* data, size_t len) noexcept
    {
        uint32_t h[8] = { 0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au, 0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u };

        // One 64-byte block through the compression function (FIPS 180-4 section 6.2.2).
        const auto compress = [&h](const unsigned char* block) noexcept {
            uint32_t w[64]{};
            for (size_t i = 0; i < 16; ++i)
            {
                const size_t b = i * 4;
                w[i] = (static_cast<uint32_t>(block[b]) << 24) |
                       (static_cast<uint32_t>(block[b + 1]) << 16) |
                       (static_cast<uint32_t>(block[b + 2]) << 8) |
                       static_cast<uint32_t>(block[b + 3]);
            }
            for (size_t i = 16; i < 64; ++i)
            {
                const uint32_t s0 = detail::Sha256Ror(w[i - 15], 7) ^ detail::Sha256Ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
                const uint32_t s1 = detail::Sha256Ror(w[i - 2], 17) ^ detail::Sha256Ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
                w[i] = w[i - 16] + s0 + w[i - 7] + s1;
            }

            uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
            for (size_t i = 0; i < 64; ++i)
            {
                const uint32_t S1 = detail::Sha256Ror(e, 6) ^ detail::Sha256Ror(e, 11) ^ detail::Sha256Ror(e, 25);
                const uint32_t ch = (e & f) ^ (~e & g);
                const uint32_t t1 = hh + S1 + ch + detail::kSha256K[i] + w[i];
                const uint32_t S0 = detail::Sha256Ror(a, 2) ^ detail::Sha256Ror(a, 13) ^ detail::Sha256Ror(a, 22);
                const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
                const uint32_t t2 = S0 + maj;
                hh = g;
                g = f;
                f = e;
                e = d + t1;
                d = c;
                c = b;
                b = a;
                a = t1 + t2;
            }
            h[0] += a;
            h[1] += b;
            h[2] += c;
            h[3] += d;
            h[4] += e;
            h[5] += f;
            h[6] += g;
            h[7] += hh;
        };

        const auto* const p = static_cast<const unsigned char*>(data);
        size_t off = 0;
        for (; p != nullptr && off + 64 <= len; off += 64)
        {
            compress(p + off);
        }

        // Padding (FIPS 180-4 section 5.1.1): the 0x80 terminator, zero fill, then the 64-bit
        // big-endian message length in BITS. When the remainder leaves no room for the length
        // (>= 56 bytes) it spills into a second, all-zero-but-length block.
        unsigned char block[64]{};
        const size_t rem = (p != nullptr) ? (len - off) : 0;
        if (rem != 0)
        {
            std::memcpy(block, p + off, rem);
        }
        block[rem] = 0x80u;
        if (rem >= 56)
        {
            compress(block);
            std::memset(block, 0, sizeof(block));
        }
        const uint64_t bits = static_cast<uint64_t>((p != nullptr) ? len : 0u) * 8u;
        for (size_t i = 0; i < 8; ++i)
        {
            block[63 - i] = static_cast<unsigned char>((bits >> (8u * i)) & 0xFFu);
        }
        compress(block);

        Sha256Digest out{};
        for (size_t i = 0; i < 8; ++i)
        {
            out[i * 4] = static_cast<unsigned char>((h[i] >> 24) & 0xFFu);
            out[i * 4 + 1] = static_cast<unsigned char>((h[i] >> 16) & 0xFFu);
            out[i * 4 + 2] = static_cast<unsigned char>((h[i] >> 8) & 0xFFu);
            out[i * 4 + 3] = static_cast<unsigned char>(h[i] & 0xFFu);
        }
        return out;
    }

    // The digest as 64 LOWERCASE hex chars -- the form the shipped-version histories store and
    // compare (a plain std::string == against a std::string_view entry).
    inline std::string Sha256Hex(const void* data, size_t len)
    {
        static constexpr char kHex[] = "0123456789abcdef";
        const Sha256Digest d = Sha256(data, len);
        std::string out(d.size() * 2, '\0');
        for (size_t i = 0; i < d.size(); ++i)
        {
            out[i * 2] = kHex[(d[i] >> 4) & 0x0Fu];
            out[i * 2 + 1] = kHex[d[i] & 0x0Fu];
        }
        return out;
    }

    inline std::string Sha256Hex(std::string_view bytes)
    {
        return Sha256Hex(bytes.data(), bytes.size());
    }
}
