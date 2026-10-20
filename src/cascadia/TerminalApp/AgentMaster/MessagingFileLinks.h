// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster: the Telegram FILE LINKS (MESSAGING.md §21.32.3, as built: §21.32.4) -- the PURE half.
//
// What the feature is: Claude writes every file it mentions as a Markdown link to file:///K:/... (the owner's rule for
// replies). In Telegram such a link is dead: a phone cannot open a path of this PC. With file links on, the bot rewrites
// each one, when it posts the message, into a link the phone CAN download:
//
//     <base>/station-<station>/<hash>/<exact path>
//     https://cc.sysd.cc/station-homepc/3f9a...c2/K/source/Agentmaster/doc/agentmaster/MESSAGING.md
//
//   <base>     the LAN URL of this PC's own file server (picked in the cog from the addresses it listens on), or the
//              hosted name https://cc.sysd.cc (Cloudflare -> a tunnel -> the fastrun relay -> this PC's server).
//   <station>  this PC in the path ("homepc"; the dev install "homepc-dev"): the relay routes on it.
//   <hash>     the SESSION's key: a custom SHA-256 of "<station>:<session id>", 64 lowercase hex characters
//              (Sha256WithInitialState with eight SECRET initial words -- the owner's "custom modified sha256").
//   <path>     the file's own path, the drive letter as the first segment, each segment percent-encoded.
//
// The file server (MessagingFileServer.cpp) serves a request only when the hash is one of ITS sessions' hashes AND the
// path is one the bot LINKED in that session (the linked set, FileLinkStore below) -- so editing a link's path never opens
// another file -- and the file still exists and does not look like a secret.
//
// Everything here is pure C++ (no Windows header), so the engine harness tests it (tests_messaging_files.cpp) and the
// hosted relay's rules can be checked against it. The Windows half -- the socket server, Credential Manager, the network
// adapters -- is MessagingFileServer.{h,cpp}; the service glue is Messaging.Files.cpp.

#pragma once

#include "Json.h"
#include "Sha256.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace Agentmaster::Msg
{
    // ===== constants ====================================================================================================

    /// <summary>The session key's length: a SHA-256 digest as lowercase hex (the edge rule and the relay check exactly this).</summary>
    inline constexpr size_t kFileLinkHashChars = 64;
    /// <summary>The first path segment's prefix: "/station-&lt;station&gt;/..." (the relay routes on what follows it).</summary>
    inline constexpr std::string_view kFileLinkStationPrefix = "/station-";
    /// <summary>The longest station name (the relay's and the edge's checks assume at most this).</summary>
    inline constexpr size_t kFileLinkMaxStationChars = 32;
    /// <summary>The release install's station name, the owner's ("cc.sysd.cc/station-homepc/...").</summary>
    inline constexpr std::wstring_view kDefaultFileLinkStation = L"homepc";
    /// <summary>The dev install's station name: its own server, its own port, its own links (both installs run together).</summary>
    inline constexpr std::wstring_view kDefaultFileLinkStationDev = L"homepc-dev";
    /// <summary>The hosted base, the owner's choice of 2026-10-19 ("Lets do cc.sysd.cc").</summary>
    inline constexpr std::wstring_view kDefaultFileLinkHostedBase = L"https://cc.sysd.cc";
    /// <summary>The release install's server port (beside the catalog's 32593; the relay's station map names it).</summary>
    inline constexpr uint16_t kDefaultFileLinkPortRelease = 32594;
    /// <summary>The dev install's server port.</summary>
    inline constexpr uint16_t kDefaultFileLinkPortDev = 32595;
    /// <summary>The longest request target the server reads (a deep Windows path, percent-encoded, fits easily).</summary>
    inline constexpr size_t kFileLinkMaxTargetBytes = 8192;
    /// <summary>How long a linked file stays downloadable after the bot last linked it (a re-link refreshes it).</summary>
    inline constexpr int64_t kFileLinkLifetimeMs = 30ll * 24 * 60 * 60 * 1000;
    /// <summary>The most files one session's linked set keeps (the oldest go first past it).</summary>
    inline constexpr size_t kFileLinkMaxFilesPerSession = 4000;
    /// <summary>The biggest file "📤 Send as file" uploads: the Bot API's multipart limit for a document (50 MB).</summary>
    inline constexpr uint64_t kFileLinkMaxUploadBytes = 50ull * 1024 * 1024;

    /// <summary>Where the bot's file links point (AppSettings::messagingFileLinksMode, a word in settings.json).</summary>
    enum class FileLinkMode
    {
        /// <summary>No server, no rewrite: Claude's file:/// links stay as they are (dead on a phone). The default.</summary>
        Off,
        /// <summary>This PC's own server on the LAN: works at home and through the sysd VPN, nowhere else.</summary>
        Lan,
        /// <summary>The hosted name (cc.sysd.cc): works anywhere the edge rules allow; the PC must be on.</summary>
        Hosted,
    };

    /// <summary>Reads a mode word; anything but "lan" or "hosted" (any case) is Off, so a damaged setting never opens a server.</summary>
    /// <param name="word">The stored word.</param>
    /// <returns>The mode.</returns>
    inline FileLinkMode ParseFileLinkMode(std::wstring_view word) noexcept
    {
        // A hand-edited settings.json may carry spaces around the word: they are not part of it.
        while (!word.empty() && (word.front() == L' ' || word.front() == L'\t'))
        {
            word.remove_prefix(1);
        }
        while (!word.empty() && (word.back() == L' ' || word.back() == L'\t'))
        {
            word.remove_suffix(1);
        }
        const auto eq = [word](std::wstring_view w) noexcept {
            if (word.size() != w.size())
            {
                return false;
            }
            for (size_t i = 0; i < w.size(); ++i)
            {
                wchar_t c = word[i];
                if (c >= L'A' && c <= L'Z')
                {
                    c = static_cast<wchar_t>(c - L'A' + L'a');
                }
                if (c != w[i])
                {
                    return false;
                }
            }
            return true;
        };
        if (eq(L"lan"))
        {
            return FileLinkMode::Lan;
        }
        if (eq(L"hosted"))
        {
            return FileLinkMode::Hosted;
        }
        return FileLinkMode::Off;
    }

    /// <summary>The word a mode is stored as ("off" / "lan" / "hosted").</summary>
    /// <param name="mode">The mode.</param>
    /// <returns>A static word.</returns>
    inline std::wstring_view FileLinkModeWord(FileLinkMode mode) noexcept
    {
        switch (mode)
        {
        case FileLinkMode::Lan:
            return L"lan";
        case FileLinkMode::Hosted:
            return L"hosted";
        default:
            return L"off";
        }
    }

    /// <summary>The install's default server port: each install runs its own server, so the two must differ.</summary>
    /// <param name="devInstall">True for the AgentmasterDev package.</param>
    /// <returns>32595 for dev, 32594 for the release install (and an unpackaged run).</returns>
    inline uint16_t DefaultFileLinkPort(bool devInstall) noexcept
    {
        return devInstall ? kDefaultFileLinkPortDev : kDefaultFileLinkPortRelease;
    }

    /// <summary>The install's default station name ("homepc", the dev install "homepc-dev").</summary>
    /// <param name="devInstall">True for the AgentmasterDev package.</param>
    /// <returns>The name, already valid (<see cref="IsValidFileLinkStation"/>).</returns>
    /// <exception cref="std::bad_alloc">The string could not be allocated.</exception>
    inline std::wstring DefaultFileLinkStation(bool devInstall)
    {
        return std::wstring{ devInstall ? kDefaultFileLinkStationDev : kDefaultFileLinkStation };
    }

    // ===== UTF-8 and small text helpers =================================================================================

    namespace detail
    {
        /// <summary>UTF-16 to UTF-8; a lone surrogate becomes U+FFFD (a path with one cannot round-trip, and is refused later).</summary>
        /// <param name="w">The text.</param>
        /// <returns>The UTF-8 bytes.</returns>
        /// <exception cref="std::bad_alloc">The string could not be allocated.</exception>
        inline std::string FlUtf8(std::wstring_view w)
        {
            std::string out;
            out.reserve(w.size());
            for (size_t i = 0; i < w.size(); ++i)
            {
                uint32_t cp = static_cast<uint16_t>(w[i]);
                if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < w.size())
                {
                    const uint32_t lo = static_cast<uint16_t>(w[i + 1]);
                    if (lo >= 0xDC00 && lo <= 0xDFFF)
                    {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        ++i;
                    }
                    else
                    {
                        cp = 0xFFFD;
                    }
                }
                else if (cp >= 0xD800 && cp <= 0xDFFF)
                {
                    cp = 0xFFFD;
                }
                if (cp < 0x80)
                {
                    out.push_back(static_cast<char>(cp));
                }
                else if (cp < 0x800)
                {
                    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
                    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                }
                else if (cp < 0x10000)
                {
                    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                }
                else
                {
                    out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
                    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
                    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                }
            }
            return out;
        }

        /// <summary>STRICT UTF-8 to UTF-16: an overlong form, a surrogate, a code point past U+10FFFF or a torn sequence is refused.</summary>
        /// <remarks>Strict on purpose: a decoded URL path names a file, and two byte strings that decode to one name must not
        /// both be accepted (an overlong '/' was the classic way through a path filter).</remarks>
        /// <param name="s">The bytes.</param>
        /// <returns>The text, or nullopt when the bytes are not valid UTF-8.</returns>
        /// <exception cref="std::bad_alloc">The string could not be allocated.</exception>
        inline std::optional<std::wstring> FlFromUtf8(std::string_view s)
        {
            std::wstring out;
            out.reserve(s.size());
            for (size_t i = 0; i < s.size();)
            {
                const uint8_t b0 = static_cast<uint8_t>(s[i]);
                uint32_t cp = 0;
                size_t n = 0;
                if (b0 < 0x80)
                {
                    cp = b0;
                    n = 1;
                }
                else if (b0 >= 0xC2 && b0 <= 0xDF)
                {
                    cp = b0 & 0x1F;
                    n = 2;
                }
                else if (b0 >= 0xE0 && b0 <= 0xEF)
                {
                    cp = b0 & 0x0F;
                    n = 3;
                }
                else if (b0 >= 0xF0 && b0 <= 0xF4)
                {
                    cp = b0 & 0x07;
                    n = 4;
                }
                else
                {
                    return std::nullopt;
                }
                if (i + n > s.size())
                {
                    return std::nullopt;
                }
                for (size_t k = 1; k < n; ++k)
                {
                    const uint8_t b = static_cast<uint8_t>(s[i + k]);
                    if ((b & 0xC0) != 0x80)
                    {
                        return std::nullopt;
                    }
                    cp = (cp << 6) | (b & 0x3F);
                }
                // Overlong forms, surrogates and the top limit.
                if ((n == 3 && cp < 0x800) || (n == 4 && (cp < 0x10000 || cp > 0x10FFFF)) || (cp >= 0xD800 && cp <= 0xDFFF))
                {
                    return std::nullopt;
                }
                if (cp >= 0x10000)
                {
                    cp -= 0x10000;
                    out.push_back(static_cast<wchar_t>(0xD800 + (cp >> 10)));
                    out.push_back(static_cast<wchar_t>(0xDC00 + (cp & 0x3FF)));
                }
                else
                {
                    out.push_back(static_cast<wchar_t>(cp));
                }
                i += n;
            }
            return out;
        }

        /// <summary>The value of one hex digit, or -1.</summary>
        /// <param name="c">The character.</param>
        /// <returns>0..15, or -1 for a non-hex character.</returns>
        inline int FlHexValue(wchar_t c) noexcept
        {
            if (c >= L'0' && c <= L'9')
            {
                return c - L'0';
            }
            if (c >= L'a' && c <= L'f')
            {
                return c - L'a' + 10;
            }
            if (c >= L'A' && c <= L'F')
            {
                return c - L'A' + 10;
            }
            return -1;
        }

        /// <summary>Percent-decodes into BYTES, strictly: a '%' not followed by two hex digits refuses the whole text.</summary>
        /// <remarks>A narrow input's other bytes are kept as they are (a request target is bytes); a wide input's raw characters
        /// (a link Claude wrote with an unescaped "é") are taken as their UTF-8 bytes, a surrogate pair as one character.</remarks>
        /// <param name="s">The text.</param>
        /// <returns>The decoded bytes, or nullopt for a broken escape.</returns>
        /// <exception cref="std::bad_alloc">The string could not be allocated.</exception>
        template<typename Ch>
        inline std::optional<std::string> FlPercentDecodeBytes(std::basic_string_view<Ch> s)
        {
            std::string out;
            out.reserve(s.size());
            size_t runStart = 0; // the wide input's pending run of raw characters (encoded together, so a pair stays a pair)
            const auto flushRun = [&](size_t end) {
                if constexpr (sizeof(Ch) == sizeof(wchar_t))
                {
                    if (end > runStart)
                    {
                        out += FlUtf8(std::wstring_view{ reinterpret_cast<const wchar_t*>(s.data() + runStart), end - runStart });
                    }
                }
                else
                {
                    (void)end;
                }
            };
            for (size_t i = 0; i < s.size(); ++i)
            {
                const auto c = static_cast<std::make_unsigned_t<Ch>>(s[i]);
                if (c == '%')
                {
                    flushRun(i);
                    if (i + 2 >= s.size())
                    {
                        return std::nullopt;
                    }
                    const int hi = FlHexValue(static_cast<wchar_t>(s[i + 1]));
                    const int lo = FlHexValue(static_cast<wchar_t>(s[i + 2]));
                    if (hi < 0 || lo < 0)
                    {
                        return std::nullopt;
                    }
                    out.push_back(static_cast<char>((hi << 4) | lo));
                    i += 2;
                    runStart = i + 1;
                }
                else if constexpr (sizeof(Ch) == sizeof(char))
                {
                    out.push_back(static_cast<char>(c));
                }
                else
                {
                    // Wide: collected into the run, flushed at the next escape or at the end.
                }
            }
            flushRun(s.size());
            return out;
        }

        /// <summary>True for RFC 3986's unreserved characters, the ones a path segment keeps unescaped.</summary>
        /// <param name="c">The byte.</param>
        /// <returns>True for A-Z a-z 0-9 - . _ ~.</returns>
        inline bool FlUnreserved(unsigned char c) noexcept
        {
            return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' || c == '~';
        }

        /// <summary>Percent-encodes UTF-8 bytes for a URL path SEGMENT: every byte but the unreserved ones as %XX (uppercase).</summary>
        /// <param name="bytes">The segment's UTF-8 bytes.</param>
        /// <returns>The encoded segment (ASCII).</returns>
        /// <exception cref="std::bad_alloc">The string could not be allocated.</exception>
        inline std::string FlPercentEncodeSegment(std::string_view bytes)
        {
            static constexpr char kHex[] = "0123456789ABCDEF";
            std::string out;
            out.reserve(bytes.size() * 3);
            for (const char ch : bytes)
            {
                const auto c = static_cast<unsigned char>(ch);
                if (FlUnreserved(c))
                {
                    out.push_back(static_cast<char>(c));
                }
                else
                {
                    out.push_back('%');
                    out.push_back(kHex[c >> 4]);
                    out.push_back(kHex[c & 0x0F]);
                }
            }
            return out;
        }

        /// <summary>ASCII-lowercase of a wide string (non-ASCII kept: Windows folds more, but a key only has to be consistent).</summary>
        /// <param name="s">The text.</param>
        /// <returns>The lowered copy.</returns>
        /// <exception cref="std::bad_alloc">The string could not be allocated.</exception>
        inline std::wstring FlLowerAscii(std::wstring_view s)
        {
            std::wstring out{ s };
            for (auto& c : out)
            {
                if (c >= L'A' && c <= L'Z')
                {
                    c = static_cast<wchar_t>(c - L'A' + L'a');
                }
            }
            return out;
        }

        /// <summary>ASCII-lowercase of a narrow string.</summary>
        /// <param name="s">The text.</param>
        /// <returns>The lowered copy.</returns>
        /// <exception cref="std::bad_alloc">The string could not be allocated.</exception>
        inline std::string FlLowerAsciiA(std::string_view s)
        {
            std::string out{ s };
            for (auto& c : out)
            {
                if (c >= 'A' && c <= 'Z')
                {
                    c = static_cast<char>(c - 'A' + 'a');
                }
            }
            return out;
        }

        /// <summary>
        /// True when a name is one of Windows' reserved DEVICE names (CON, PRN, AUX, NUL, COM0-9, LPT0-9 and their superscript
        /// forms, CONIN$, CONOUT$, CLOCK$), alone or with an extension ("nul.txt"), in any case.
        /// </summary>
        /// <remarks>Win32 maps such a name to the device in any folder ("K:\x\COM1" opens a serial port), so a path holding one
        /// names no file this server may serve — and a read of a device can block a connection thread.</remarks>
        /// <param name="seg">The segment (no trailing dot or space: FlPlainSegment refuses those first).</param>
        /// <returns>True for a device name.</returns>
        inline bool FlReservedDeviceName(std::wstring_view seg) noexcept
        {
            const std::wstring_view stem = seg.substr(0, seg.find(L'.'));
            std::wstring_view s = stem;
            while (!s.empty() && s.back() == L' ')
            {
                s.remove_suffix(1);
            }
            wchar_t lower[8]{};
            if (s.size() < 3 || s.size() > 7)
            {
                return false;
            }
            for (size_t i = 0; i < s.size(); ++i)
            {
                const wchar_t c = s[i];
                lower[i] = (c >= L'A' && c <= L'Z') ? static_cast<wchar_t>(c - L'A' + L'a') : c;
            }
            const std::wstring_view name{ lower, s.size() };
            if (name == L"con" || name == L"prn" || name == L"aux" || name == L"nul" || name == L"conin$" || name == L"conout$" || name == L"clock$")
            {
                return true;
            }
            if (name.size() == 4 && (name.substr(0, 3) == L"com" || name.substr(0, 3) == L"lpt"))
            {
                const wchar_t d = name[3];
                // A digit, or the superscript one, two or three Windows also reserves ("COM¹").
                return (d >= L'0' && d <= L'9') || d == L'\x00B9' || d == L'\x00B2' || d == L'\x00B3';
            }
            return false;
        }

        /// <summary>True when a decoded path SEGMENT is a plain file or folder name Windows would accept as one segment.</summary>
        /// <remarks>Refuses "", "." and "..", any control character, every character Windows forbids in a name ('/' '\\' ':' '*'
        /// '?' '"' '&lt;' '&gt;' '|'), a name ending in a dot or a space, and a reserved device name — so a decoded segment can never
        /// climb, switch drives, name a stream (":$DATA"), hide a second segment inside an escaped separator, alias another file
        /// (Win32 strips a trailing dot or space: "key.pem." opens "key.pem", past the secret-name check) or open a device.</remarks>
        /// <param name="seg">The decoded segment.</param>
        /// <returns>True when it is one plain name.</returns>
        inline bool FlPlainSegment(std::wstring_view seg) noexcept
        {
            if (seg.empty() || seg == L"." || seg == L"..")
            {
                return false;
            }
            if (seg.back() == L'.' || seg.back() == L' ')
            {
                return false;
            }
            for (const wchar_t c : seg)
            {
                if (c < 0x20 || c == 0x7F || c == L'/' || c == L'\\' || c == L':' || c == L'*' || c == L'?' || c == L'"' || c == L'<' || c == L'>' || c == L'|')
                {
                    return false;
                }
            }
            return !FlReservedDeviceName(seg);
        }

        /// <summary>True for an ASCII letter (a drive letter).</summary>
        /// <param name="c">The character.</param>
        /// <returns>True for A-Z or a-z.</returns>
        inline bool FlDriveLetter(wchar_t c) noexcept
        {
            return (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z');
        }
    }

    // ===== the session key: a custom SHA-256 ============================================================================

    /// <summary>
    /// The eight initial hash words of the file links' custom SHA-256, read from the 32-byte secret as big-endian words.
    /// </summary>
    /// <remarks>The secret lives in Credential Manager (MessagingFileServer.cpp, LoadOrCreateFileLinkSecret), one per install;
    /// replacing it changes every session's key, so every link the bot ever posted stops working at once.</remarks>
    /// <param name="secret">The 32 secret bytes.</param>
    /// <returns>The words, in the order Sha256WithInitialState takes them.</returns>
    inline std::array<uint32_t, 8> FileLinkInitialStateFromSecret(const std::array<uint8_t, 32>& secret) noexcept
    {
        std::array<uint32_t, 8> words{};
        for (size_t i = 0; i < 8; ++i)
        {
            words[i] = (static_cast<uint32_t>(secret[i * 4]) << 24) | (static_cast<uint32_t>(secret[i * 4 + 1]) << 16) |
                       (static_cast<uint32_t>(secret[i * 4 + 2]) << 8) | static_cast<uint32_t>(secret[i * 4 + 3]);
        }
        return words;
    }

    /// <summary>
    /// A session's KEY in its file links: the custom SHA-256 (eight secret initial words) of "&lt;station&gt;:&lt;session id&gt;",
    /// both lowercased, as 64 lowercase hex characters.
    /// </summary>
    /// <remarks>
    /// The owner's choice (MESSAGING.md §21.32.3): "no hmac, use just a custom modified sha256". The code is public, so the
    /// words ARE the secret: without them nobody computes a session's key from its id (and ids are printed in many places:
    /// hooks.log, the CLI, the Sessions page). The station is part of the input, so the release and dev installs' keys for one
    /// session differ. ⚠ Not a MAC: see Sha256WithInitialState's length-extension note.
    /// </remarks>
    /// <param name="state">The eight initial words (FileLinkInitialStateFromSecret).</param>
    /// <param name="station">The station name.</param>
    /// <param name="sessionId">The session id (a GUID string).</param>
    /// <returns>64 lowercase hex characters.</returns>
    /// <exception cref="std::bad_alloc">The strings could not be allocated.</exception>
    inline std::string FileLinkSessionHash(const std::array<uint32_t, 8>& state, std::wstring_view station, std::wstring_view sessionId)
    {
        const std::string input = detail::FlUtf8(detail::FlLowerAscii(station) + L":" + detail::FlLowerAscii(sessionId));
        uint32_t words[8]{};
        std::copy(state.begin(), state.end(), words);
        return Sha256DigestHex(Sha256WithInitialState(input.data(), input.size(), words));
    }

    /// <summary>True for a well-formed session key: exactly 64 LOWERCASE hex characters (what the edge rule checks too).</summary>
    /// <param name="s">The candidate.</param>
    /// <returns>True when it has the key's form (not that it is any session's).</returns>
    inline bool IsFileLinkHash(std::string_view s) noexcept
    {
        if (s.size() != kFileLinkHashChars)
        {
            return false;
        }
        for (const char c : s)
        {
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            {
                return false;
            }
        }
        return true;
    }

    /// <summary>Formats the 32-byte secret as 64 lowercase hex characters (how Credential Manager keeps it).</summary>
    /// <param name="secret">The secret.</param>
    /// <returns>The hex text.</returns>
    /// <exception cref="std::bad_alloc">The string could not be allocated.</exception>
    inline std::wstring FormatFileLinkSecretHex(const std::array<uint8_t, 32>& secret)
    {
        static constexpr wchar_t kHex[] = L"0123456789abcdef";
        std::wstring out(64, L'0');
        for (size_t i = 0; i < 32; ++i)
        {
            out[i * 2] = kHex[secret[i] >> 4];
            out[i * 2 + 1] = kHex[secret[i] & 0x0F];
        }
        return out;
    }

    /// <summary>Reads a stored secret: exactly 64 hex characters (any case), else nothing (a damaged credential is replaced).</summary>
    /// <param name="hex">The stored text.</param>
    /// <returns>The 32 bytes, or nullopt.</returns>
    inline std::optional<std::array<uint8_t, 32>> ParseFileLinkSecretHex(std::wstring_view hex) noexcept
    {
        if (hex.size() != 64)
        {
            return std::nullopt;
        }
        std::array<uint8_t, 32> out{};
        for (size_t i = 0; i < 32; ++i)
        {
            const int hi = detail::FlHexValue(hex[i * 2]);
            const int lo = detail::FlHexValue(hex[i * 2 + 1]);
            if (hi < 0 || lo < 0)
            {
                return std::nullopt;
            }
            out[i] = static_cast<uint8_t>((hi << 4) | lo);
        }
        return out;
    }

    // ===== the station ==================================================================================================

    /// <summary>True for a valid station name: 1..32 of [a-z0-9-], not starting or ending with '-' (the relay's map keys).</summary>
    /// <param name="s">The candidate.</param>
    /// <returns>True when it is valid as it stands (no case folding).</returns>
    inline bool IsValidFileLinkStation(std::wstring_view s) noexcept
    {
        if (s.empty() || s.size() > kFileLinkMaxStationChars || s.front() == L'-' || s.back() == L'-')
        {
            return false;
        }
        for (const wchar_t c : s)
        {
            if (!((c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9') || c == L'-'))
            {
                return false;
            }
        }
        return true;
    }

    /// <summary>A typed station name, trimmed and lowercased; "" when what is left is not valid (the caller then uses the default).</summary>
    /// <param name="s">The typed text.</param>
    /// <returns>The valid name, or "".</returns>
    /// <exception cref="std::bad_alloc">The string could not be allocated.</exception>
    inline std::wstring NormalizeFileLinkStation(std::wstring_view s)
    {
        const size_t a = s.find_first_not_of(L" \t\r\n");
        if (a == std::wstring_view::npos)
        {
            return {};
        }
        const size_t b = s.find_last_not_of(L" \t\r\n");
        std::wstring out = detail::FlLowerAscii(s.substr(a, b - a + 1));
        return IsValidFileLinkStation(out) ? out : std::wstring{};
    }

    // ===== paths: file URLs, the URL path, the linked set's key ==========================================================

    /// <summary>
    /// The Windows path a file URL names: "file:///K:/a/b%20c.md" -> "K:\a\b c.md". Also takes "file://localhost/K:/...",
    /// "file:/K:/..." and "file://K:/..." (forms Claude and others write); a "#fragment" or "?query" is dropped.
    /// </summary>
    /// <remarks>Only a DRIVE path is accepted (a UNC share or a relative path names nothing this server serves), the escapes are
    /// decoded strictly (a broken one refuses), the bytes must be valid UTF-8, and every segment must be a plain name — "..",
    /// "." and an escaped separator are refused, so a link can never point above where it says.</remarks>
    /// <param name="url">The link target, as written.</param>
    /// <returns>The path with backslashes, or nullopt when it is not a drive file URL this feature takes.</returns>
    /// <exception cref="std::bad_alloc">The strings could not be allocated.</exception>
    inline std::optional<std::wstring> FileUrlToWindowsPath(std::wstring_view url)
    {
        if (url.size() < 8)
        {
            return std::nullopt;
        }
        const std::wstring head = detail::FlLowerAscii(url.substr(0, 5));
        if (head != L"file:")
        {
            return std::nullopt;
        }
        std::wstring_view rest = url.substr(5);
        // Drop a fragment and a query: "#L27" (a line), "?x".
        const size_t cut = rest.find_first_of(L"#?");
        if (cut != std::wstring_view::npos)
        {
            rest = rest.substr(0, cut);
        }
        // The authority forms: "///K:", "//localhost/K:", "//K:" (a drive written where the host goes), "/K:".
        if (rest.size() >= 2 && rest[0] == L'/' && rest[1] == L'/')
        {
            rest.remove_prefix(2);
            const std::wstring lowered = detail::FlLowerAscii(rest.substr(0, (std::min)(rest.size(), size_t{ 10 })));
            if (lowered.rfind(L"localhost/", 0) == 0)
            {
                rest.remove_prefix(9); // keeps the '/'
            }
        }
        while (!rest.empty() && rest.front() == L'/')
        {
            rest.remove_prefix(1);
        }
        // The drive: "K:" (or "K%3A") then a separator.
        if (rest.size() < 3 || !detail::FlDriveLetter(rest[0]))
        {
            return std::nullopt;
        }
        size_t afterDrive = 0;
        if (rest[1] == L':')
        {
            afterDrive = 2;
        }
        else if (rest.size() >= 5 && rest[1] == L'%' && rest[2] == L'3' && (rest[3] == L'A' || rest[3] == L'a'))
        {
            afterDrive = 4;
        }
        else
        {
            return std::nullopt;
        }
        if (afterDrive >= rest.size() || (rest[afterDrive] != L'/' && rest[afterDrive] != L'\\'))
        {
            return std::nullopt;
        }
        std::wstring path;
        path.push_back(rest[0]);
        path += L":";
        // Split the ENCODED text first, then decode each segment: an escaped separator ("%2F", "%5C") stays inside its
        // segment and is refused there, exactly as the server's DecodeFileLinkPath refuses it.
        std::wstring_view tail = rest.substr(afterDrive + 1);
        size_t segments = 0;
        while (!tail.empty())
        {
            const size_t sep = tail.find_first_of(L"/\\");
            const std::wstring_view raw = tail.substr(0, sep);
            if (!raw.empty())
            {
                const auto bytes = detail::FlPercentDecodeBytes<wchar_t>(raw);
                if (!bytes)
                {
                    return std::nullopt;
                }
                const auto seg = detail::FlFromUtf8(*bytes);
                if (!seg || !detail::FlPlainSegment(*seg))
                {
                    return std::nullopt;
                }
                path += L"\\";
                path += *seg;
                ++segments;
            }
            if (sep == std::wstring_view::npos)
            {
                break;
            }
            tail.remove_prefix(sep + 1);
        }
        if (segments == 0)
        {
            return std::nullopt; // a bare drive is not a file
        }
        return path;
    }

    /// <summary>
    /// A Windows drive path as the link's PATH part: "K:\source\x y.md" -> "K/source/x%20y.md" (the drive letter uppercased
    /// as the first segment, then each segment's UTF-8 percent-encoded; no leading slash).
    /// </summary>
    /// <param name="winPath">The path (backslashes or slashes).</param>
    /// <returns>The encoded path, or nullopt when it is not a drive path of plain segments.</returns>
    /// <exception cref="std::bad_alloc">The strings could not be allocated.</exception>
    inline std::optional<std::string> EncodeFileLinkPath(std::wstring_view winPath)
    {
        if (winPath.size() < 4 || !detail::FlDriveLetter(winPath[0]) || winPath[1] != L':' || (winPath[2] != L'\\' && winPath[2] != L'/'))
        {
            return std::nullopt;
        }
        std::string out;
        out.push_back(static_cast<char>(winPath[0] >= L'a' && winPath[0] <= L'z' ? winPath[0] - L'a' + L'A' : winPath[0]));
        std::wstring_view tail = winPath.substr(3);
        size_t segments = 0;
        while (!tail.empty())
        {
            const size_t sep = tail.find_first_of(L"/\\");
            const std::wstring_view seg = tail.substr(0, sep);
            if (!seg.empty())
            {
                if (!detail::FlPlainSegment(seg))
                {
                    return std::nullopt;
                }
                out.push_back('/');
                out += detail::FlPercentEncodeSegment(detail::FlUtf8(seg));
                ++segments;
            }
            if (sep == std::wstring_view::npos)
            {
                break;
            }
            tail.remove_prefix(sep + 1);
        }
        if (segments == 0)
        {
            return std::nullopt;
        }
        return out;
    }

    /// <summary>The inverse of <see cref="EncodeFileLinkPath"/>: "K/source/x%20y.md" -> "K:\source\x y.md".</summary>
    /// <remarks>Strict: the drive must be ONE letter, every escape well-formed, the bytes valid UTF-8, and every decoded segment a
    /// plain name (an escaped "%2F", "..", a ':' stream name or an empty segment refuses the request).</remarks>
    /// <param name="encoded">The path part of a request target (after the hash and its slash).</param>
    /// <returns>The Windows path, or nullopt for anything else.</returns>
    /// <exception cref="std::bad_alloc">The strings could not be allocated.</exception>
    inline std::optional<std::wstring> DecodeFileLinkPath(std::string_view encoded)
    {
        if (encoded.size() < 3 || encoded[1] != '/' || !detail::FlDriveLetter(static_cast<wchar_t>(static_cast<unsigned char>(encoded[0]))))
        {
            return std::nullopt;
        }
        std::wstring path;
        path.push_back(static_cast<wchar_t>(encoded[0]));
        path += L":";
        std::string_view tail = encoded.substr(2);
        size_t segments = 0;
        while (true)
        {
            const size_t sep = tail.find('/');
            const std::string_view raw = tail.substr(0, sep);
            const auto bytes = detail::FlPercentDecodeBytes<char>(raw);
            if (!bytes)
            {
                return std::nullopt;
            }
            const auto seg = detail::FlFromUtf8(*bytes);
            if (!seg || !detail::FlPlainSegment(*seg))
            {
                return std::nullopt;
            }
            path += L"\\";
            path += *seg;
            ++segments;
            if (sep == std::string_view::npos)
            {
                break;
            }
            tail.remove_prefix(sep + 1);
        }
        return segments == 0 ? std::nullopt : std::optional<std::wstring>{ std::move(path) };
    }

    /// <summary>The linked set's KEY for a path: backslashes, ASCII-lowercased (Windows names are case-insensitive).</summary>
    /// <param name="winPath">The path.</param>
    /// <returns>The key.</returns>
    /// <exception cref="std::bad_alloc">The string could not be allocated.</exception>
    inline std::wstring FileLinkPathKey(std::wstring_view winPath)
    {
        std::wstring key = detail::FlLowerAscii(winPath);
        for (auto& c : key)
        {
            if (c == L'/')
            {
                c = L'\\';
            }
        }
        return key;
    }

    /// <summary>The file name of a path (its last segment).</summary>
    /// <param name="winPath">The path.</param>
    /// <returns>The name ("" for a path ending in a separator).</returns>
    /// <exception cref="std::bad_alloc">The string could not be allocated.</exception>
    inline std::wstring FileLinkFileName(std::wstring_view winPath)
    {
        const size_t p = winPath.find_last_of(L"\\/");
        return std::wstring{ p == std::wstring_view::npos ? winPath : winPath.substr(p + 1) };
    }

    // ===== the request target ============================================================================================

    /// <summary>Why a request target was refused (for the log line; the answer is 404 or 400 either way).</summary>
    enum class FileLinkTargetError
    {
        None,
        /// <summary>Not "/station-..." at all (a probe, a stray request).</summary>
        NotOurs,
        /// <summary>The station segment is not a valid name.</summary>
        BadStation,
        /// <summary>The key is not 64 lowercase hex characters.</summary>
        BadHash,
        /// <summary>The path part does not decode to a drive path of plain names.</summary>
        BadPath,
        /// <summary>A '?' or '#' (the edge refuses a query too; a link never has one).</summary>
        HasQuery,
        /// <summary>Longer than kFileLinkMaxTargetBytes.</summary>
        TooLong,
    };

    /// <summary>A request target's word for the log.</summary>
    /// <param name="e">The error.</param>
    /// <returns>A static word.</returns>
    inline std::wstring_view FileLinkTargetErrorWord(FileLinkTargetError e) noexcept
    {
        switch (e)
        {
        case FileLinkTargetError::None:
            return L"ok";
        case FileLinkTargetError::NotOurs:
            return L"not a file link";
        case FileLinkTargetError::BadStation:
            return L"bad station";
        case FileLinkTargetError::BadHash:
            return L"bad key";
        case FileLinkTargetError::BadPath:
            return L"bad path";
        case FileLinkTargetError::HasQuery:
            return L"query";
        default:
            return L"too long";
        }
    }

    /// <summary>What a request names: its station, its session key and its file.</summary>
    struct FileLinkTarget
    {
        /// <summary>The station segment ("homepc").</summary>
        std::wstring station;
        /// <summary>The session key (64 lowercase hex).</summary>
        std::string hash;
        /// <summary>The decoded Windows path ("K:\source\x.md").</summary>
        std::wstring path;
        /// <summary>The path part as it came (still encoded), for the log.</summary>
        std::string encodedPath;
    };

    /// <summary>
    /// Reads a request target "/station-&lt;station&gt;/&lt;64 hex&gt;/&lt;encoded path&gt;" — the only shape the server answers with a file.
    /// </summary>
    /// <param name="target">The request line's target, as received.</param>
    /// <param name="why">Optional: why it was refused.</param>
    /// <returns>The parts, or nullopt (and *why) when the target is anything else.</returns>
    /// <exception cref="std::bad_alloc">The strings could not be allocated.</exception>
    inline std::optional<FileLinkTarget> ParseFileLinkTarget(std::string_view target, FileLinkTargetError* why = nullptr)
    {
        const auto fail = [why](FileLinkTargetError e) {
            if (why)
            {
                *why = e;
            }
            return std::optional<FileLinkTarget>{};
        };
        if (target.size() > kFileLinkMaxTargetBytes)
        {
            return fail(FileLinkTargetError::TooLong);
        }
        if (target.find_first_of("?#") != std::string_view::npos)
        {
            return fail(FileLinkTargetError::HasQuery);
        }
        if (target.rfind(kFileLinkStationPrefix, 0) != 0)
        {
            return fail(FileLinkTargetError::NotOurs);
        }
        std::string_view rest = target.substr(kFileLinkStationPrefix.size());
        const size_t s1 = rest.find('/');
        if (s1 == std::string_view::npos)
        {
            return fail(FileLinkTargetError::BadStation);
        }
        FileLinkTarget t;
        for (const char c : rest.substr(0, s1))
        {
            t.station.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
        }
        if (!IsValidFileLinkStation(t.station))
        {
            return fail(FileLinkTargetError::BadStation);
        }
        rest.remove_prefix(s1 + 1);
        if (rest.size() < kFileLinkHashChars + 2 || rest[kFileLinkHashChars] != '/')
        {
            return fail(FileLinkTargetError::BadHash);
        }
        t.hash = std::string{ rest.substr(0, kFileLinkHashChars) };
        if (!IsFileLinkHash(t.hash))
        {
            return fail(FileLinkTargetError::BadHash);
        }
        t.encodedPath = std::string{ rest.substr(kFileLinkHashChars + 1) };
        auto path = DecodeFileLinkPath(t.encodedPath);
        if (!path)
        {
            return fail(FileLinkTargetError::BadPath);
        }
        t.path = std::move(*path);
        if (why)
        {
            *why = FileLinkTargetError::None;
        }
        return t;
    }

    /// <summary>A file link: "&lt;base&gt;/station-&lt;station&gt;/&lt;hash&gt;/&lt;encoded path&gt;" (the base's trailing slashes dropped).</summary>
    /// <param name="base">"https://cc.sysd.cc" or "http://192.168.50.17:32594".</param>
    /// <param name="station">The station name.</param>
    /// <param name="hash">The session key.</param>
    /// <param name="encodedPath">EncodeFileLinkPath's result.</param>
    /// <returns>The URL (ASCII, no space, no parenthesis: safe inside a Markdown link).</returns>
    /// <exception cref="std::bad_alloc">The string could not be allocated.</exception>
    inline std::wstring BuildFileLinkUrl(std::wstring_view base, std::wstring_view station, std::string_view hash, std::string_view encodedPath)
    {
        while (!base.empty() && base.back() == L'/')
        {
            base.remove_suffix(1);
        }
        std::wstring url{ base };
        url += L"/station-";
        url += station;
        url += L"/";
        for (const char c : hash)
        {
            url.push_back(static_cast<wchar_t>(c));
        }
        url += L"/";
        for (const char c : encodedPath)
        {
            url.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
        }
        return url;
    }

    // ===== the Markdown rewrite ==========================================================================================

    /// <summary>What a rewrite did.</summary>
    struct FileLinkRewrite
    {
        /// <summary>The text with the links rewritten (unchanged where nothing was).</summary>
        std::wstring text;
        /// <summary>How many file links got a download link.</summary>
        size_t rewritten{ 0 };
        /// <summary>How many file links were left as they were (the resolver declined: missing, a folder, a secret...).</summary>
        size_t kept{ 0 };
    };

    /// <summary>
    /// Rewrites the file links of a Markdown text: each "[text](file:///...)" outside code whose path the resolver accepts gets
    /// the resolver's URL; everything else — the link text, other links, code spans and fenced blocks — stays byte for byte.
    /// </summary>
    /// <remarks>
    /// Code is skipped on purpose: a file URL inside `code` or a ``` block is text the author shows, not a link. The target ends
    /// at the first ')' or whitespace (a title after it is kept); "](&lt;file:///...&gt;)" is taken too. The resolver runs once per
    /// link, in order, and may have side effects (the service adds the path to the session's linked set).
    /// </remarks>
    /// <param name="md">The Markdown.</param>
    /// <param name="urlFor">The resolver: the Windows path -> the download URL, or nullopt to keep the link as it is.</param>
    /// <returns>The rewritten text and the counts.</returns>
    /// <exception cref="std::bad_alloc">The strings could not be allocated.</exception>
    /// <exception cref="std::exception">Whatever the resolver throws (the service's resolver contains its own).</exception>
    inline FileLinkRewrite RewriteFileLinksInMarkdown(std::wstring_view md, const std::function<std::optional<std::wstring>(const std::wstring& winPath)>& urlFor)
    {
        FileLinkRewrite r;
        r.text.reserve(md.size());
        bool inFence = false;
        wchar_t fenceChar = 0;
        size_t fenceLen = 0;
        size_t i = 0;
        while (i < md.size())
        {
            // At a line start: a fence opens or closes (up to 3 spaces of indent, then ``` or ~~~ runs).
            const bool lineStart = i == 0 || md[i - 1] == L'\n';
            if (lineStart)
            {
                size_t j = i;
                while (j < md.size() && j - i < 3 && md[j] == L' ')
                {
                    ++j;
                }
                if (j < md.size() && (md[j] == L'`' || md[j] == L'~'))
                {
                    const wchar_t ch = md[j];
                    size_t k = j;
                    while (k < md.size() && md[k] == ch)
                    {
                        ++k;
                    }
                    const size_t run = k - j;
                    if (run >= 3 && (!inFence || (ch == fenceChar && run >= fenceLen)))
                    {
                        if (inFence)
                        {
                            inFence = false;
                        }
                        else
                        {
                            inFence = true;
                            fenceChar = ch;
                            fenceLen = run;
                        }
                        // Copy the fence line as it is.
                        const size_t eol = md.find(L'\n', i);
                        const size_t end = eol == std::wstring_view::npos ? md.size() : eol + 1;
                        r.text.append(md.substr(i, end - i));
                        i = end;
                        continue;
                    }
                }
            }
            if (inFence)
            {
                const size_t eol = md.find(L'\n', i);
                const size_t end = eol == std::wstring_view::npos ? md.size() : eol + 1;
                r.text.append(md.substr(i, end - i));
                i = end;
                continue;
            }
            // An inline code span: a run of N backticks to the next run of exactly N (no match = literal backticks).
            if (md[i] == L'`')
            {
                size_t k = i;
                while (k < md.size() && md[k] == L'`')
                {
                    ++k;
                }
                const size_t run = k - i;
                size_t close = std::wstring_view::npos;
                for (size_t p = k; p < md.size();)
                {
                    if (md[p] == L'`')
                    {
                        size_t q = p;
                        while (q < md.size() && md[q] == L'`')
                        {
                            ++q;
                        }
                        if (q - p == run)
                        {
                            close = q;
                            break;
                        }
                        p = q;
                    }
                    else
                    {
                        ++p;
                    }
                }
                if (close == std::wstring_view::npos)
                {
                    r.text.append(md.substr(i, run));
                    i = k;
                }
                else
                {
                    r.text.append(md.substr(i, close - i));
                    i = close;
                }
                continue;
            }
            // A link target: "](" then the target.
            if (md[i] == L']' && i + 1 < md.size() && md[i + 1] == L'(')
            {
                size_t t = i + 2;
                const bool angle = t < md.size() && md[t] == L'<';
                if (angle)
                {
                    ++t;
                }
                if (md.size() - t >= 5 && detail::FlLowerAscii(md.substr(t, 5)) == L"file:")
                {
                    size_t e = t;
                    if (angle)
                    {
                        while (e < md.size() && md[e] != L'>' && md[e] != L'\n')
                        {
                            ++e;
                        }
                    }
                    else
                    {
                        while (e < md.size() && md[e] != L')' && md[e] != L' ' && md[e] != L'\t' && md[e] != L'\n')
                        {
                            ++e;
                        }
                    }
                    const bool closed = angle ? (e < md.size() && md[e] == L'>') : true;
                    std::optional<std::wstring> url;
                    if (closed)
                    {
                        if (const auto path = FileUrlToWindowsPath(md.substr(t, e - t)))
                        {
                            url = urlFor ? urlFor(*path) : std::nullopt;
                        }
                    }
                    if (url)
                    {
                        r.text += L"](";
                        r.text += *url;
                        ++r.rewritten;
                        i = angle ? e + 1 : e; // drop the angle brackets: the URL holds no space or parenthesis
                        continue;
                    }
                    ++r.kept;
                    r.text.append(md.substr(i, e - i));
                    i = e;
                    continue;
                }
            }
            r.text.push_back(md[i]);
            ++i;
        }
        return r;
    }

    // ===== files that are never linked =================================================================================

    /// <summary>
    /// True for a path that looks like a SECRET — a key, a token, a credential store, a password database — which the bot never
    /// links and the server never serves, whoever asks.
    /// </summary>
    /// <remarks>
    /// A name or folder test, not a content test: it catches the usual places (".env", "*.pem", "id_rsa", "~/.ssh",
    /// "~/.claude/.credentials.json", "kubeconfig", the sysd secrets folder, "*.token"). The linked set already limits a session
    /// to files that appeared in its own messages; this is the second fence for the files that should never leave the PC even
    /// so. The server re-checks the FINAL path after opening (a link to a symlink that points at a key is refused there).
    /// </remarks>
    /// <param name="winPath">The path.</param>
    /// <returns>True to refuse.</returns>
    /// <exception cref="std::bad_alloc">The strings could not be allocated.</exception>
    inline bool LooksLikeSecretFile(std::wstring_view winPath)
    {
        const std::wstring key = FileLinkPathKey(winPath);
        const std::wstring name = FileLinkFileName(key);
        const auto endsWith = [&name](std::wstring_view suffix) {
            return name.size() >= suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0;
        };
        const auto startsWith = [&name](std::wstring_view prefix) { return name.rfind(prefix, 0) == 0; };
        static constexpr std::wstring_view kNames[] = {
            L".env", L".git-credentials", L".netrc", L"_netrc", L".npmrc", L".pypirc", L".pgpass", L".htpasswd", L"credentials",
            L"credentials.json", L".credentials.json", L"secrets.json", L"secrets.yaml", L"secrets.yml", L"kubeconfig",
            L"authorized_keys", L"known_hosts", L"login.keychain", L"keystore", L"token", L".token", L"wallet.dat",
            L".claude.json", // Claude Code's global config: the signed-in account
        };
        for (const auto n : kNames)
        {
            if (name == n)
            {
                return true;
            }
        }
        static constexpr std::wstring_view kSuffixes[] = {
            L".pem", L".key", L".pfx", L".p12", L".kdbx", L".kdb", L".ppk", L".jks", L".keystore", L".token", L".secret",
            L".secrets", L".kubeconfig", L".asc", L".gpg", L".pgp", L".ovpn",
        };
        for (const auto s : kSuffixes)
        {
            if (endsWith(s))
            {
                return true;
            }
        }
        if (startsWith(L".env.") || startsWith(L"id_rsa") || startsWith(L"id_dsa") || startsWith(L"id_ecdsa") || startsWith(L"id_ed25519"))
        {
            return true;
        }
        // Folders: a segment anywhere in the path.
        static constexpr std::wstring_view kFolders[] = {
            L"\\.ssh\\", L"\\.aws\\", L"\\.gnupg\\", L"\\.kube\\", L"\\secrets\\", L"\\.secrets\\", L"\\.config\\sysd\\",
            L"\\microsoft\\credentials\\", L"\\microsoft\\protect\\", L"\\microsoft\\crypto\\", L"\\microsoft\\vault\\",
            L"\\.docker\\", L"\\.azure\\", L"\\.gcloud\\", L"\\gcloud\\",
        };
        for (const auto f : kFolders)
        {
            if (key.find(f) != std::wstring::npos)
            {
                return true;
            }
        }
        return false;
    }

    /// <summary>
    /// Reads the cog's "never link files under" list: one folder per line (blank lines and lines starting with '#' skipped),
    /// each normalized to a lowercase key with backslashes and a trailing backslash.
    /// </summary>
    /// <param name="text">The setting's text (AppSettings::messagingFileLinksExclude).</param>
    /// <returns>The folder keys ("k:\source\proxmox\").</returns>
    /// <exception cref="std::bad_alloc">The list could not be built.</exception>
    inline std::vector<std::wstring> ParseFileLinkExcludedFolders(std::wstring_view text)
    {
        std::vector<std::wstring> out;
        while (!text.empty())
        {
            const size_t nl = text.find_first_of(L"\r\n");
            std::wstring_view line = text.substr(0, nl);
            text.remove_prefix(nl == std::wstring_view::npos ? text.size() : nl + 1);
            const size_t a = line.find_first_not_of(L" \t\"");
            if (a == std::wstring_view::npos || line[a] == L'#')
            {
                continue;
            }
            const size_t b = line.find_last_not_of(L" \t\"");
            std::wstring key = FileLinkPathKey(line.substr(a, b - a + 1));
            while (!key.empty() && key.back() == L'\\')
            {
                key.pop_back();
            }
            if (key.size() >= 2)
            {
                out.push_back(key + L"\\");
            }
        }
        return out;
    }

    /// <summary>True when a path lies under one of the excluded folders (ParseFileLinkExcludedFolders' keys).</summary>
    /// <remarks>For a folder whose files must never leave the PC although their names look harmless — a repository whose
    /// CLAUDE.md holds passwords, say. The bot then keeps such a link as it was, and the server refuses it.</remarks>
    /// <param name="winPath">The path.</param>
    /// <param name="excludedKeys">The folder keys.</param>
    /// <returns>True to refuse.</returns>
    /// <exception cref="std::bad_alloc">The key could not be built.</exception>
    inline bool IsUnderExcludedFolder(std::wstring_view winPath, const std::vector<std::wstring>& excludedKeys)
    {
        if (excludedKeys.empty())
        {
            return false;
        }
        const std::wstring key = FileLinkPathKey(winPath);
        for (const auto& folder : excludedKeys)
        {
            if (key.size() > folder.size() && key.compare(0, folder.size(), folder) == 0)
            {
                return true;
            }
        }
        return false;
    }

    // ===== HTTP: the request head, ranges, the response headers =========================================================

    /// <summary>A request's head: the request line and the headers, as received.</summary>
    struct HttpRequestHead
    {
        /// <summary>The method ("GET", "HEAD").</summary>
        std::string method;
        /// <summary>The request target ("/station-...").</summary>
        std::string target;
        /// <summary>"HTTP/1.1" or "HTTP/1.0".</summary>
        std::string version;
        /// <summary>The headers, names as sent.</summary>
        std::vector<std::pair<std::string, std::string>> headers;

        /// <summary>The first header of a name, compared without case.</summary>
        /// <param name="name">The header name.</param>
        /// <returns>Its value, or null when absent.</returns>
        const std::string* Header(std::string_view name) const
        {
            for (const auto& [k, v] : headers)
            {
                if (k.size() == name.size() && detail::FlLowerAsciiA(k) == detail::FlLowerAsciiA(name))
                {
                    return &v;
                }
            }
            return nullptr;
        }
    };

    /// <summary>
    /// Parses a request head (everything before the blank line): "METHOD SP target SP HTTP/1.x" and "Name: value" lines.
    /// </summary>
    /// <remarks>Strict and small: a method of 1..16 uppercase letters, a target that starts with '/' with no space or control
    /// character, HTTP/1.0 or 1.1, at most 64 headers, no folded line. Anything else is a 400.</remarks>
    /// <param name="head">The bytes before "\r\n\r\n".</param>
    /// <returns>The head, or nullopt for a malformed one.</returns>
    /// <exception cref="std::bad_alloc">The strings could not be allocated.</exception>
    inline std::optional<HttpRequestHead> ParseHttpRequestHead(std::string_view head)
    {
        std::vector<std::string_view> lines;
        while (!head.empty())
        {
            const size_t nl = head.find('\n');
            std::string_view line = head.substr(0, nl);
            if (!line.empty() && line.back() == '\r')
            {
                line.remove_suffix(1);
            }
            lines.push_back(line);
            if (nl == std::string_view::npos)
            {
                break;
            }
            head.remove_prefix(nl + 1);
        }
        if (lines.empty() || lines.size() > 65)
        {
            return std::nullopt;
        }
        HttpRequestHead h;
        const std::string_view rl = lines[0];
        const size_t a = rl.find(' ');
        const size_t b = a == std::string_view::npos ? std::string_view::npos : rl.find(' ', a + 1);
        if (a == std::string_view::npos || b == std::string_view::npos || rl.find(' ', b + 1) != std::string_view::npos)
        {
            return std::nullopt;
        }
        h.method = std::string{ rl.substr(0, a) };
        h.target = std::string{ rl.substr(a + 1, b - a - 1) };
        h.version = std::string{ rl.substr(b + 1) };
        if (h.method.empty() || h.method.size() > 16 || !std::all_of(h.method.begin(), h.method.end(), [](char c) { return c >= 'A' && c <= 'Z'; }))
        {
            return std::nullopt;
        }
        if (h.target.empty() || h.target[0] != '/' || !std::all_of(h.target.begin(), h.target.end(), [](char c) { return static_cast<unsigned char>(c) > 0x20 && c != 0x7F; }))
        {
            return std::nullopt;
        }
        if (h.version != "HTTP/1.1" && h.version != "HTTP/1.0")
        {
            return std::nullopt;
        }
        for (size_t i = 1; i < lines.size(); ++i)
        {
            const std::string_view line = lines[i];
            if (line.empty())
            {
                continue;
            }
            if (line[0] == ' ' || line[0] == '\t')
            {
                return std::nullopt; // obs-fold: refused (RFC 9112 §5.2 allows a 400)
            }
            const size_t colon = line.find(':');
            if (colon == std::string_view::npos || colon == 0)
            {
                return std::nullopt;
            }
            std::string_view value = line.substr(colon + 1);
            while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
            {
                value.remove_prefix(1);
            }
            while (!value.empty() && (value.back() == ' ' || value.back() == '\t'))
            {
                value.remove_suffix(1);
            }
            h.headers.emplace_back(std::string{ line.substr(0, colon) }, std::string{ value });
        }
        return h;
    }

    /// <summary>An inclusive byte range.</summary>
    struct FileByteRange
    {
        uint64_t first{ 0 };
        uint64_t last{ 0 };
    };

    /// <summary>What a Range header asks.</summary>
    enum class FileRangeVerdict
    {
        /// <summary>No Range header, or one the server ignores (several ranges, another unit): the whole file, 200.</summary>
        Whole,
        /// <summary>One satisfiable range: 206 with Content-Range.</summary>
        Partial,
        /// <summary>A range past the end (or any range of an empty file): 416.</summary>
        Unsatisfiable,
    };

    /// <summary>
    /// Reads a Range header for a file of `size` bytes: "bytes=a-b", "bytes=a-" or "bytes=-n" (one range only).
    /// </summary>
    /// <remarks>A download manager resumes with one range; several ranges are answered with the whole file (allowed by RFC 9110
    /// §14.2: a server may ignore Range), so no multipart/byteranges body is ever built.</remarks>
    /// <param name="value">The header's value, or null when absent.</param>
    /// <param name="size">The file's size.</param>
    /// <param name="out">The range when Partial.</param>
    /// <returns>The verdict.</returns>
    inline FileRangeVerdict ParseFileByteRange(const std::string* value, uint64_t size, FileByteRange& out) noexcept
    {
        if (!value)
        {
            return FileRangeVerdict::Whole;
        }
        std::string_view v{ *value };
        if (v.size() < 6 || !(v[0] == 'b' || v[0] == 'B') || v.substr(1, 5) != "ytes=")
        {
            return FileRangeVerdict::Whole;
        }
        v.remove_prefix(6);
        if (v.find(',') != std::string_view::npos)
        {
            return FileRangeVerdict::Whole;
        }
        const size_t dash = v.find('-');
        if (dash == std::string_view::npos)
        {
            return FileRangeVerdict::Whole;
        }
        const auto num = [](std::string_view s, uint64_t& n) noexcept {
            if (s.empty() || s.size() > 19)
            {
                return false;
            }
            n = 0;
            for (const char c : s)
            {
                if (c < '0' || c > '9')
                {
                    return false;
                }
                n = n * 10 + static_cast<uint64_t>(c - '0');
            }
            return true;
        };
        const std::string_view a = v.substr(0, dash);
        const std::string_view b = v.substr(dash + 1);
        if (a.empty())
        {
            uint64_t n = 0;
            if (!num(b, n))
            {
                return FileRangeVerdict::Whole;
            }
            if (n == 0 || size == 0)
            {
                return FileRangeVerdict::Unsatisfiable;
            }
            out.first = n >= size ? 0 : size - n;
            out.last = size - 1;
            return FileRangeVerdict::Partial;
        }
        uint64_t first = 0;
        if (!num(a, first))
        {
            return FileRangeVerdict::Whole;
        }
        uint64_t last = size == 0 ? 0 : size - 1;
        if (!b.empty())
        {
            uint64_t l = 0;
            if (!num(b, l) || l < first)
            {
                return FileRangeVerdict::Whole;
            }
            last = (std::min)(l, last);
        }
        if (size == 0 || first >= size)
        {
            return FileRangeVerdict::Unsatisfiable;
        }
        out.first = first;
        out.last = last;
        return FileRangeVerdict::Partial;
    }

    /// <summary>
    /// The Content-Disposition value that makes a phone SAVE the file under its own name:
    /// attachment; filename="&lt;ASCII fallback&gt;"; filename*=UTF-8''&lt;percent-encoded&gt; (RFC 6266 / RFC 8187).
    /// </summary>
    /// <remarks>The ASCII fallback replaces every non-ASCII, quote, backslash and control character with '_' (an old client
    /// shows that); filename* carries the real name (Android's DownloadManager and browsers read it).</remarks>
    /// <param name="fileName">The file's name.</param>
    /// <returns>The header value (ASCII).</returns>
    /// <exception cref="std::bad_alloc">The string could not be allocated.</exception>
    inline std::string ContentDispositionAttachment(std::wstring_view fileName)
    {
        std::string ascii;
        for (const wchar_t c : fileName)
        {
            ascii.push_back(c >= 0x20 && c < 0x7F && c != L'"' && c != L'\\' ? static_cast<char>(c) : '_');
        }
        if (ascii.empty())
        {
            ascii = "file";
        }
        // RFC 8187 attr-char: ALPHA DIGIT ! # $ & + - . ^ _ ` | ~ ; the rest %XX.
        static constexpr char kHex[] = "0123456789ABCDEF";
        std::string ext;
        for (const char ch : detail::FlUtf8(fileName))
        {
            const auto c = static_cast<unsigned char>(ch);
            const bool keep = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '!' || c == '#' || c == '$' || c == '&' ||
                              c == '+' || c == '-' || c == '.' || c == '^' || c == '_' || c == '`' || c == '|' || c == '~';
            if (keep)
            {
                ext.push_back(static_cast<char>(c));
            }
            else
            {
                ext.push_back('%');
                ext.push_back(kHex[c >> 4]);
                ext.push_back(kHex[c & 0x0F]);
            }
        }
        return "attachment; filename=\"" + ascii + "\"; filename*=UTF-8''" + ext;
    }

    /// <summary>The Content-Type a downloaded file is sent with, by its extension (text files as UTF-8 plain text).</summary>
    /// <remarks>Text-like source files go as text/plain so any phone app opens them; the download is an attachment with
    /// nosniff either way, so an .html file is never rendered by the browser that fetched it.</remarks>
    /// <param name="fileName">The file name.</param>
    /// <returns>A static MIME type ("application/octet-stream" when unknown).</returns>
    /// <exception cref="std::bad_alloc">The string could not be allocated.</exception>
    inline std::string_view GuessFileMimeType(std::wstring_view fileName)
    {
        const std::wstring name = detail::FlLowerAscii(fileName);
        const size_t dot = name.find_last_of(L'.');
        const std::wstring ext = dot == std::wstring::npos ? std::wstring{} : name.substr(dot + 1);
        static constexpr std::pair<std::wstring_view, std::string_view> kTypes[] = {
            { L"pdf", "application/pdf" },
            { L"png", "image/png" },
            { L"jpg", "image/jpeg" },
            { L"jpeg", "image/jpeg" },
            { L"gif", "image/gif" },
            { L"webp", "image/webp" },
            { L"bmp", "image/bmp" },
            { L"svg", "image/svg+xml" },
            { L"ico", "image/x-icon" },
            { L"zip", "application/zip" },
            { L"gz", "application/gzip" },
            { L"tgz", "application/gzip" },
            { L"7z", "application/x-7z-compressed" },
            { L"tar", "application/x-tar" },
            { L"mp4", "video/mp4" },
            { L"webm", "video/webm" },
            { L"mov", "video/quicktime" },
            { L"mp3", "audio/mpeg" },
            { L"wav", "audio/wav" },
            { L"ogg", "audio/ogg" },
            { L"json", "application/json" },
            { L"csv", "text/csv; charset=utf-8" },
            { L"html", "text/html; charset=utf-8" },
            { L"htm", "text/html; charset=utf-8" },
            { L"xml", "application/xml" },
            { L"apk", "application/vnd.android.package-archive" },
            { L"exe", "application/octet-stream" },
            { L"dll", "application/octet-stream" },
            { L"docx", "application/vnd.openxmlformats-officedocument.wordprocessingml.document" },
            { L"xlsx", "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet" },
            { L"pptx", "application/vnd.openxmlformats-officedocument.presentationml.presentation" },
        };
        for (const auto& [e, t] : kTypes)
        {
            if (ext == e)
            {
                return t;
            }
        }
        static constexpr std::wstring_view kText[] = {
            L"txt", L"md", L"markdown", L"log", L"ini", L"cfg", L"conf", L"toml", L"yaml", L"yml", L"c", L"cc", L"cpp", L"cxx",
            L"h", L"hh", L"hpp", L"hxx", L"cs", L"py", L"js", L"mjs", L"ts", L"tsx", L"jsx", L"java", L"kt", L"go", L"rs", L"rb",
            L"php", L"sh", L"bash", L"ps1", L"psm1", L"bat", L"cmd", L"sql", L"css", L"scss", L"idl", L"xaml", L"props",
            L"targets", L"vcxproj", L"csproj", L"sln", L"slnx", L"filters", L"gitignore", L"gitattributes", L"editorconfig",
            L"dockerfile", L"tf", L"lua", L"r", L"swift", L"m", L"mm", L"diff", L"patch", L"jsonl", L"tsv", L"rst", L"tex",
        };
        for (const auto e : kText)
        {
            if (ext == e)
            {
                return "text/plain; charset=utf-8";
            }
        }
        return "application/octet-stream";
    }

    /// <summary>The reason phrase of a status the server answers with.</summary>
    /// <param name="status">200, 206, 400, 403, 404, 405, 413, 416, 500, 503.</param>
    /// <returns>The phrase ("Error" for any other).</returns>
    inline std::string_view HttpReasonPhrase(int status) noexcept
    {
        switch (status)
        {
        case 200:
            return "OK";
        case 206:
            return "Partial Content";
        case 400:
            return "Bad Request";
        case 403:
            return "Forbidden";
        case 404:
            return "Not Found";
        case 405:
            return "Method Not Allowed";
        case 413:
            return "Content Too Large";
        case 416:
            return "Range Not Satisfiable";
        case 500:
            return "Internal Server Error";
        case 503:
            return "Service Unavailable";
        default:
            return "Error";
        }
    }

    // ===== the linked set ================================================================================================

    /// <summary>One linked file of a session.</summary>
    struct FileLinkEntry
    {
        /// <summary>The path as linked (its case kept for the download's name).</summary>
        std::wstring path;
        /// <summary>When the bot last linked it (unix ms): the link expires kFileLinkLifetimeMs later.</summary>
        int64_t linkedMs{ 0 };
    };

    /// <summary>
    /// The LINKED SET: per session, every file the bot linked in its messages (and /download asked for). The server serves a
    /// session's file only when it is here — so a link names exactly one file, and editing its path opens nothing.
    /// </summary>
    /// <remarks>Persisted beside the bot's state (&lt;profile&gt;\messaging\telegram\filelinks.json) so links survive a restart. Not
    /// thread-safe: the service guards it with its own lock.</remarks>
    struct FileLinkStore
    {
        /// <summary>Session id -> (path key -> entry).</summary>
        std::map<std::wstring, std::map<std::wstring, FileLinkEntry>> sessions;

        /// <summary>Adds a file to a session's set, or refreshes its time.</summary>
        /// <param name="sessionId">The session.</param>
        /// <param name="path">The file's path.</param>
        /// <param name="nowMs">Now (unix ms).</param>
        /// <returns>True when the file was new to the session (the caller then saves the store).</returns>
        /// <exception cref="std::bad_alloc">The map could not grow.</exception>
        bool Add(const std::wstring& sessionId, const std::wstring& path, int64_t nowMs)
        {
            auto& files = sessions[sessionId];
            auto [it, inserted] = files.try_emplace(FileLinkPathKey(path));
            it->second.path = path;
            it->second.linkedMs = nowMs;
            return inserted;
        }

        /// <summary>The entry of a session's file, or null when the session never linked it.</summary>
        /// <param name="sessionId">The session.</param>
        /// <param name="path">The file's path (any case, either slash).</param>
        /// <returns>The entry, or null.</returns>
        /// <exception cref="std::bad_alloc">The key could not be built.</exception>
        const FileLinkEntry* Find(const std::wstring& sessionId, std::wstring_view path) const
        {
            const auto s = sessions.find(sessionId);
            if (s == sessions.end())
            {
                return nullptr;
            }
            const auto f = s->second.find(FileLinkPathKey(path));
            return f == s->second.end() ? nullptr : &f->second;
        }

        /// <summary>Drops expired links and, past the cap, a session's oldest; drops sessions left empty.</summary>
        /// <param name="nowMs">Now (unix ms).</param>
        /// <param name="lifetimeMs">How long a link lives after it was last linked.</param>
        /// <param name="maxPerSession">The most files a session keeps.</param>
        /// <returns>How many files were dropped.</returns>
        /// <exception cref="std::bad_alloc">The temporary order could not be built.</exception>
        size_t Prune(int64_t nowMs, int64_t lifetimeMs, size_t maxPerSession)
        {
            size_t dropped = 0;
            for (auto s = sessions.begin(); s != sessions.end();)
            {
                auto& files = s->second;
                for (auto f = files.begin(); f != files.end();)
                {
                    if (nowMs - f->second.linkedMs > lifetimeMs)
                    {
                        f = files.erase(f);
                        ++dropped;
                    }
                    else
                    {
                        ++f;
                    }
                }
                if (files.size() > maxPerSession)
                {
                    std::vector<std::pair<int64_t, std::wstring>> order;
                    order.reserve(files.size());
                    for (const auto& [k, e] : files)
                    {
                        order.emplace_back(e.linkedMs, k);
                    }
                    std::sort(order.begin(), order.end());
                    const size_t excess = files.size() - maxPerSession;
                    for (size_t i = 0; i < excess; ++i)
                    {
                        files.erase(order[i].second);
                        ++dropped;
                    }
                }
                if (files.empty())
                {
                    s = sessions.erase(s);
                }
                else
                {
                    ++s;
                }
            }
            return dropped;
        }

        /// <summary>The number of linked files over every session.</summary>
        /// <returns>The count.</returns>
        size_t FileCount() const noexcept
        {
            size_t n = 0;
            for (const auto& [sid, files] : sessions)
            {
                n += files.size();
            }
            return n;
        }
    };

    /// <summary>The store as JSON: {"v":1,"sessions":{"&lt;id&gt;":[["&lt;path&gt;",&lt;ms&gt;],...]}}.</summary>
    /// <param name="store">The store.</param>
    /// <returns>The JSON value (json::Dump makes the text).</returns>
    /// <exception cref="std::bad_alloc">The value could not be built.</exception>
    inline json::Value FileLinkStoreToJson(const FileLinkStore& store)
    {
        auto root = json::Value::MkObj();
        root.Set(L"v", json::Value::MkNum(1));
        auto sessions = json::Value::MkObj();
        for (const auto& [sid, files] : store.sessions)
        {
            auto list = json::Value::MkArr();
            for (const auto& [key, e] : files)
            {
                auto pair = json::Value::MkArr();
                pair.Push(json::Value::MkStr(e.path));
                pair.Push(json::Value::MkNum(static_cast<double>(e.linkedMs)));
                list.Push(std::move(pair));
            }
            sessions.Set(sid, std::move(list));
        }
        root.Set(L"sessions", std::move(sessions));
        return root;
    }

    /// <summary>Reads a store written by <see cref="FileLinkStoreToJson"/>; anything malformed is skipped, never fatal.</summary>
    /// <param name="v">The parsed JSON.</param>
    /// <returns>The store (empty for an unknown version or shape).</returns>
    /// <exception cref="std::bad_alloc">The store could not be built.</exception>
    inline FileLinkStore FileLinkStoreFromJson(const json::Value& v)
    {
        FileLinkStore store;
        if (v.type != json::Value::Type::Obj || v.I64At(L"v", 0) != 1)
        {
            return store;
        }
        const auto* sessions = v.Find(L"sessions");
        if (!sessions || sessions->type != json::Value::Type::Obj)
        {
            return store;
        }
        for (const auto& [sid, list] : sessions->members)
        {
            if (sid.empty() || list.type != json::Value::Type::Arr)
            {
                continue;
            }
            for (const auto& pair : list.arr)
            {
                if (pair.type != json::Value::Type::Arr || pair.arr.size() != 2 || pair.arr[0].type != json::Value::Type::Str || pair.arr[0].str.empty())
                {
                    continue;
                }
                store.Add(sid, pair.arr[0].str, pair.arr[1].AsI64(0));
            }
        }
        return store;
    }

    // ===== the LAN addresses the cog offers ==============================================================================

    /// <summary>One IPv4 address of a network adapter of this PC.</summary>
    struct FileLinkLanAddress
    {
        /// <summary>The adapter's name ("Wi-Fi 2", "vEthernet (WSL)").</summary>
        std::wstring adapter;
        /// <summary>The dotted address ("192.168.50.17").</summary>
        std::wstring ipv4;
        /// <summary>A virtual adapter (Hyper-V, WSL, a VPN's TAP): offered after the physical ones, labeled so.</summary>
        bool virtualAdapter{ false };
    };

    /// <summary>One choice of the cog's "links point to" dropdown.</summary>
    struct FileLinkUrlChoice
    {
        /// <summary>The base URL ("http://192.168.50.17:32594").</summary>
        std::wstring url;
        /// <summary>What the dropdown shows.</summary>
        std::wstring label;
    };

    /// <summary>True for an address the server talks to: loopback 127/8 or private 10/8, 172.16/12, 192.168/16.</summary>
    /// <remarks>The server's peer filter: the relay (k3s nodes), the LAN and the sysd VPN's NAT address are all private; a
    /// public peer means a port forward or a misrouted packet, and gets nothing.</remarks>
    /// <param name="hostOrder">The IPv4 address in host byte order.</param>
    /// <returns>True when the peer may be served.</returns>
    inline bool IsAllowedFileLinkPeer(uint32_t hostOrder) noexcept
    {
        const uint32_t a = hostOrder >> 24;
        const uint32_t b = (hostOrder >> 16) & 0xFF;
        return a == 127 || a == 10 || (a == 172 && b >= 16 && b <= 31) || (a == 192 && b == 168);
    }

    /// <summary>
    /// The dropdown's choices: "http://&lt;address&gt;:&lt;port&gt;" for each private address (physical adapters first), then the PC's
    /// name ("http://&lt;name&gt;:&lt;port&gt;", which needs the LAN DNS to know it). Duplicates dropped.
    /// </summary>
    /// <param name="addresses">The adapters' addresses (MessagingFileServer.cpp, EnumerateFileLinkLanAddresses).</param>
    /// <param name="hostName">The PC's DNS host name ("" = none).</param>
    /// <param name="port">The server's port.</param>
    /// <returns>The choices, in the order the dropdown lists them.</returns>
    /// <exception cref="std::bad_alloc">The list could not be built.</exception>
    inline std::vector<FileLinkUrlChoice> ComposeFileLinkLanChoices(const std::vector<FileLinkLanAddress>& addresses, std::wstring_view hostName, uint16_t port)
    {
        std::vector<FileLinkUrlChoice> out;
        const std::wstring portText = std::to_wstring(port);
        const auto add = [&out](std::wstring url, std::wstring label) {
            for (const auto& c : out)
            {
                if (c.url == url)
                {
                    return;
                }
            }
            out.push_back(FileLinkUrlChoice{ std::move(url), std::move(label) });
        };
        for (const bool wantVirtual : { false, true })
        {
            for (const auto& a : addresses)
            {
                if (a.virtualAdapter != wantVirtual || a.ipv4.empty())
                {
                    continue;
                }
                add(L"http://" + a.ipv4 + L":" + portText,
                    L"http://" + a.ipv4 + L":" + portText + L"  \x2014  " + a.adapter + (a.virtualAdapter ? L" (virtual)" : L""));
            }
        }
        if (!hostName.empty())
        {
            const std::wstring host = detail::FlLowerAscii(hostName);
            add(L"http://" + host + L":" + portText, L"http://" + host + L":" + portText + L"  \x2014  this PC's name (the LAN DNS must know it)");
        }
        return out;
    }

    /// <summary>
    /// A typed base URL (the cog's LAN pick or hosted base), made safe to build links on: trimmed, the scheme lowercased, the
    /// trailing slashes dropped — or "" when it is not "http://" or "https://" followed by a host and only characters a URL
    /// needs no escape for (letters, digits and "-._~:/[]").
    /// </summary>
    /// <remarks>
    /// Every link is the base plus "/station-…", and the bot writes it into a Markdown link and an HTML attribute: a space, a
    /// quote, a parenthesis or an angle bracket in the base would break the message's markup, and another scheme (javascript:,
    /// file:) must never become a link the phone opens. A refused base makes the service fall back to the default one (and the
    /// cog shows the box red), never a broken link.
    /// </remarks>
    /// <param name="s">The typed text.</param>
    /// <returns>The base ("https://cc.sysd.cc", "http://192.168.50.17:32594"), or "".</returns>
    /// <exception cref="std::bad_alloc">The string could not be allocated.</exception>
    inline std::wstring NormalizeFileLinkBase(std::wstring_view s)
    {
        const size_t a = s.find_first_not_of(L" \t\r\n");
        if (a == std::wstring_view::npos)
        {
            return {};
        }
        const size_t b = s.find_last_not_of(L" \t\r\n");
        std::wstring_view t = s.substr(a, b - a + 1);
        while (!t.empty() && t.back() == L'/')
        {
            t.remove_suffix(1);
        }
        std::wstring out;
        const std::wstring head = detail::FlLowerAscii(t.substr(0, (std::min)(t.size(), size_t{ 8 })));
        if (head.rfind(L"https://", 0) == 0)
        {
            out = L"https://";
        }
        else if (head.rfind(L"http://", 0) == 0)
        {
            out = L"http://";
        }
        else
        {
            return {};
        }
        const std::wstring_view rest = t.substr(out.size());
        // A host is required: the part up to the first '/' must hold more than a port.
        const std::wstring_view host = rest.substr(0, rest.find(L'/'));
        if (host.empty() || host.front() == L':')
        {
            return {};
        }
        for (const wchar_t c : rest)
        {
            const bool safe = (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9') || c == L'-' || c == L'.' ||
                              c == L'_' || c == L'~' || c == L':' || c == L'/' || c == L'[' || c == L']';
            if (!safe)
            {
                return {};
            }
        }
        out += rest;
        return out;
    }

    /// <summary>The base every link gets for the mode: the LAN pick (else the first LAN choice), or the hosted base.</summary>
    /// <param name="mode">The mode.</param>
    /// <param name="lanPick">The cog's dropdown pick ("" = not picked).</param>
    /// <param name="lanChoices">The current LAN choices (the first is the fallback).</param>
    /// <param name="hostedBase">The hosted base ("" = kDefaultFileLinkHostedBase).</param>
    /// <returns>The base, or "" when the mode is Off or no LAN address exists.</returns>
    /// <exception cref="std::bad_alloc">The string could not be allocated.</exception>
    inline std::wstring ChooseFileLinkBase(FileLinkMode mode, std::wstring_view lanPick, const std::vector<FileLinkUrlChoice>& lanChoices, std::wstring_view hostedBase)
    {
        switch (mode)
        {
        case FileLinkMode::Lan:
            if (!lanPick.empty())
            {
                return std::wstring{ lanPick };
            }
            return lanChoices.empty() ? std::wstring{} : lanChoices.front().url;
        case FileLinkMode::Hosted:
            return std::wstring{ hostedBase.empty() ? kDefaultFileLinkHostedBase : hostedBase };
        default:
            return {};
        }
    }

    // ===== /download's argument ==========================================================================================

    /// <summary>
    /// The file a "/download &lt;path&gt;" names: an absolute drive path, a file:/// URL, or a path RELATIVE to the session's folder
    /// ("src\x.cpp", "../notes.md"); quotes, backticks and angle brackets around it are dropped, slashes become backslashes, and
    /// "." / ".." are resolved lexically (never above the drive).
    /// </summary>
    /// <param name="arg">What the user typed after /download.</param>
    /// <param name="sessionDir">The session's working folder (for a relative path; "" refuses one).</param>
    /// <returns>The path, or nullopt when it names no drive file.</returns>
    /// <exception cref="std::bad_alloc">The strings could not be allocated.</exception>
    inline std::optional<std::wstring> ResolveDownloadArgument(std::wstring_view arg, std::wstring_view sessionDir)
    {
        const size_t a = arg.find_first_not_of(L" \t\r\n");
        if (a == std::wstring_view::npos)
        {
            return std::nullopt;
        }
        const size_t b = arg.find_last_not_of(L" \t\r\n");
        std::wstring_view t = arg.substr(a, b - a + 1);
        if (t.find(L'\n') != std::wstring_view::npos)
        {
            t = t.substr(0, t.find(L'\n')); // the first line only
            while (!t.empty() && (t.back() == L' ' || t.back() == L'\r'))
            {
                t.remove_suffix(1);
            }
        }
        while (t.size() >= 2 && ((t.front() == L'"' && t.back() == L'"') || (t.front() == L'\'' && t.back() == L'\'') ||
                                 (t.front() == L'`' && t.back() == L'`') || (t.front() == L'<' && t.back() == L'>')))
        {
            t = t.substr(1, t.size() - 2);
        }
        if (t.empty())
        {
            return std::nullopt;
        }
        if (t.size() >= 5 && detail::FlLowerAscii(t.substr(0, 5)) == L"file:")
        {
            return FileUrlToWindowsPath(t);
        }
        std::wstring full;
        if (t.size() >= 3 && detail::FlDriveLetter(t[0]) && t[1] == L':' && (t[2] == L'\\' || t[2] == L'/'))
        {
            full = std::wstring{ t };
        }
        else
        {
            if (sessionDir.empty())
            {
                return std::nullopt;
            }
            full = std::wstring{ sessionDir } + L"\\" + std::wstring{ t };
        }
        if (full.size() < 3 || !detail::FlDriveLetter(full[0]) || full[1] != L':')
        {
            return std::nullopt;
        }
        std::vector<std::wstring> parts;
        std::wstring_view tail{ full.data() + 2, full.size() - 2 };
        while (!tail.empty())
        {
            const size_t sep = tail.find_first_of(L"/\\");
            const std::wstring_view seg = tail.substr(0, sep);
            if (seg == L"..")
            {
                if (!parts.empty())
                {
                    parts.pop_back();
                }
            }
            else if (!seg.empty() && seg != L".")
            {
                if (!detail::FlPlainSegment(seg))
                {
                    return std::nullopt;
                }
                parts.emplace_back(seg);
            }
            if (sep == std::wstring_view::npos)
            {
                break;
            }
            tail.remove_prefix(sep + 1);
        }
        if (parts.empty())
        {
            return std::nullopt;
        }
        std::wstring out;
        out.push_back(full[0]);
        out += L":";
        for (const auto& p : parts)
        {
            out += L"\\";
            out += p;
        }
        return out;
    }
}
