// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster M5 — MESSAGING.md §21.32.4: the Telegram FILE LINKS (the user's requests of 2026-10-20: Claude's links to files on
// this PC made openable from the phone, through cc.sysd.cc or the home network, a session key from "a custom modified sha256 on
// both sides", Cloudflare letting through only /station-<station>/<key>/<exact path>, and /download delivering the same way).
// This file pins:
//   1. the session key: the custom SHA-256 against an INDEPENDENT implementation's answers (a pure-Python SHA-256 with the same
//      initial words), the standard initial words reproducing SHA-256, case-insensitive inputs, the key's form, the secret's hex;
//   2. the paths: every file-URL form Claude writes, and every refusal (a climb, an escaped separator, a stream, a UNC share, a
//      broken escape or UTF-8, a device name, a trailing dot or space); the URL path's encoding both ways;
//   3. the request target: its parts, and each refusal kind; the link built from its parts;
//   4. the Markdown rewrite: links outside code only, the angle form, other links and code untouched, the counts;
//   5. the files never linked (secret names and folders, the excluded folders) and the settings' readers (station, base, mode);
//   6. the HTTP pieces: the request head, byte ranges, the attachment header;
//   7. the linked set: add, dedupe by key, find, prune by age and by cap, its JSON both ways and a damaged file;
//   8. the LAN choices, the peer filter, the base each mode uses; /download's argument; the command's reading;
//   9. the server END TO END on loopback: a real file served whole, by HEAD and by range, every refusal, a "secret" final path,
//      the totals, a stop and a second start;
//  10. the secret in Credential Manager: made, read again, replaced (a scratch target, deleted after).
// The service's half (the mirror's rewrite, /download's reply, the button's upload) needs Telegram: the live check is the dev bot.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#include "m5_tests.h"

#include <wincred.h>

#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#include "../MessagingFileLinks.h"
#include "../MessagingFileServer.h"
#include "../MessagingModels.h" // ParseRemoteCommand, kAgentmasterCommands (/download)

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "advapi32.lib")

using namespace Agentmaster;
using namespace Agentmaster::Msg;

namespace
{
    /// <summary>One HTTP exchange with a server on 127.0.0.1: sends the raw request, reads until the server closes (it closes
    /// after every answer), 5 s at most.</summary>
    /// <param name="port">The server's port.</param>
    /// <param name="request">The raw request, head and blank line included.</param>
    /// <returns>The whole raw response ("" when no connection could be made).</returns>
    std::string HttpExchange(uint16_t port, const std::string& request)
    {
        const SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET)
        {
            return {};
        }
        DWORD timeout = 5000;
        ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(port);
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::connect(s, reinterpret_cast<const sockaddr*>(&a), sizeof(a)) != 0)
        {
            ::closesocket(s);
            return {};
        }
        size_t at = 0;
        while (at < request.size())
        {
            const int n = ::send(s, request.data() + at, static_cast<int>(request.size() - at), 0);
            if (n <= 0)
            {
                break;
            }
            at += static_cast<size_t>(n);
        }
        std::string out;
        char buf[4096];
        for (;;)
        {
            const int n = ::recv(s, buf, sizeof(buf), 0);
            if (n <= 0)
            {
                break;
            }
            out.append(buf, static_cast<size_t>(n));
        }
        ::closesocket(s);
        return out;
    }

    /// <summary>A response's status code (0 when it has none).</summary>
    /// <param name="response">The raw response.</param>
    /// <returns>The status.</returns>
    int StatusOf(const std::string& response)
    {
        if (response.rfind("HTTP/1.1 ", 0) != 0 || response.size() < 12)
        {
            return 0;
        }
        return std::atoi(response.substr(9, 3).c_str());
    }

    /// <summary>A response's body: everything after the blank line.</summary>
    /// <param name="response">The raw response.</param>
    /// <returns>The body.</returns>
    std::string BodyOf(const std::string& response)
    {
        const size_t end = response.find("\r\n\r\n");
        return end == std::string::npos ? std::string{} : response.substr(end + 4);
    }

    /// <summary>A response header's value, by its exact name ("" when absent).</summary>
    /// <param name="response">The raw response.</param>
    /// <param name="name">The header's name.</param>
    /// <returns>The value.</returns>
    std::string HeaderOf(const std::string& response, const std::string& name)
    {
        const size_t end = response.find("\r\n\r\n");
        const std::string head = response.substr(0, end);
        const std::string key = "\r\n" + name + ": ";
        const size_t at = head.find(key);
        if (at == std::string::npos)
        {
            return {};
        }
        const size_t from = at + key.size();
        const size_t to = head.find("\r\n", from);
        return head.substr(from, to == std::string::npos ? std::string::npos : to - from);
    }

    /// <summary>The 32 test secret bytes the independent answers were computed for: byte i = (i * 7 + 1) mod 256.</summary>
    /// <returns>The secret.</returns>
    std::array<uint8_t, 32> TestSecret()
    {
        std::array<uint8_t, 32> secret{};
        for (size_t i = 0; i < secret.size(); ++i)
        {
            secret[i] = static_cast<uint8_t>((i * 7 + 1) & 0xFF);
        }
        return secret;
    }

    /// <summary>Writes a file's bytes (the server test's fixtures).</summary>
    /// <param name="path">The file.</param>
    /// <param name="bytes">The content.</param>
    void WriteFixture(const std::filesystem::path& path, const std::string& bytes)
    {
        std::ofstream f{ path, std::ios::binary | std::ios::trunc };
        f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
}

void TestMessagingFiles()
{
    std::wprintf(L"[MESSAGING 21.32.4 file links]\n");

    // ===== 1. the session key ===========================================================================================
    CHECK(Sha256Hex("abc", 3) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "file links: plain SHA-256 of abc is the FIPS vector");
    CHECK(Sha256DigestHex(Sha256WithInitialState("abc", 3, kSha256InitialState)) == Sha256Hex("abc", 3), "file links: the standard initial words reproduce SHA-256");
    {
        const auto iv = FileLinkInitialStateFromSecret(TestSecret());
        CHECK(iv[0] == 0x01080F16u && iv[7] == 0xC5CCD3DAu, "file links: the initial words are the secret's big-endian words");
        // The answers of an INDEPENDENT implementation (a pure-Python SHA-256 with the same initial words, checked against
        // hashlib with the standard words): one block, the other station, and a two-block input.
        const std::string h1 = FileLinkSessionHash(iv, L"homepc", L"0391efeb-1111-2222-3333-444455556666");
        CHECK(h1 == "f76d8d64a2de5dece4dfa60b4746b1fbb7756dbf1d4d60e8b1c13078e6f88635", "file links: the key matches the independent implementation (homepc)");
        CHECK(FileLinkSessionHash(iv, L"homepc-dev", L"0391efeb-1111-2222-3333-444455556666") == "ac395b5e4ad27d16dcb27ad35bdee6f8687b2f841933b2425edd8e8633f7304b",
              "file links: the key matches the independent implementation (homepc-dev): the station is part of the input");
        CHECK(FileLinkSessionHash(iv, L"homepc", std::wstring(100, L'x')) == "daa5e2ae71dbf2b46931918944715f5624f6c541965743fe3e6ac7d3286b8371",
              "file links: the key matches the independent implementation over two blocks");
        CHECK(FileLinkSessionHash(iv, L"HOMEPC", L"0391EFEB-1111-2222-3333-444455556666") == h1, "file links: the inputs are lowercased (one key per session, any case)");
        CHECK(h1 != Sha256Hex(std::string_view{ "homepc:0391efeb-1111-2222-3333-444455556666" }), "file links: the key is not the plain SHA-256 of the same input");
        CHECK(IsFileLinkHash(h1), "file links: a key has the key's form");
        std::string upper = h1;
        upper[0] = 'F';
        CHECK(!IsFileLinkHash(upper) && !IsFileLinkHash(h1.substr(1)) && !IsFileLinkHash(h1 + "0") && !IsFileLinkHash(std::string(64, 'g')),
              "file links: an uppercase digit, a short or long key and a non-hex key are not keys");
        const std::wstring hex = FormatFileLinkSecretHex(TestSecret());
        const auto back = ParseFileLinkSecretHex(hex);
        CHECK(hex.size() == 64 && back && *back == TestSecret(), "file links: the secret's hex round trip");
        CHECK(!ParseFileLinkSecretHex(hex.substr(2)) && !ParseFileLinkSecretHex(hex + L"00") && !ParseFileLinkSecretHex(std::wstring(64, L'z')),
              "file links: a damaged stored secret is refused (a new one is made)");
    }

    // ===== 2. the paths =================================================================================================
    {
        const auto p = FileUrlToWindowsPath(L"file:///K:/source/x%20y/report.md#L27");
        CHECK(p && *p == L"K:\\source\\x y\\report.md", "file links: file:/// with an escape and a fragment");
        CHECK(FileUrlToWindowsPath(L"file://localhost/K:/a/b.md").value_or(L"") == L"K:\\a\\b.md", "file links: file://localhost/");
        CHECK(FileUrlToWindowsPath(L"file:/K:/a/b.md").value_or(L"") == L"K:\\a\\b.md", "file links: file:/K:");
        CHECK(FileUrlToWindowsPath(L"file://K:/a/b.md").value_or(L"") == L"K:\\a\\b.md", "file links: file://K: (a drive where the host goes)");
        CHECK(FileUrlToWindowsPath(L"FILE:///K:/a/b.md?x=1").value_or(L"") == L"K:\\a\\b.md", "file links: any case, a query dropped");
        CHECK(FileUrlToWindowsPath(L"file:///K:/a/r\x00E9port.md").value_or(L"") == L"K:\\a\\r\x00E9port.md", "file links: an unescaped non-ASCII name");
        CHECK(!FileUrlToWindowsPath(L"file:///K:/a/../b.md"), "file links: a climb is refused");
        CHECK(!FileUrlToWindowsPath(L"file:///K:/a/./b.md"), "file links: a . segment is refused");
        CHECK(!FileUrlToWindowsPath(L"file:///K:/a%2Fb.md") && !FileUrlToWindowsPath(L"file:///K:/a%5Cb.md"), "file links: an escaped separator is refused");
        CHECK(!FileUrlToWindowsPath(L"file:///K:/a%zzb.md") && !FileUrlToWindowsPath(L"file:///K:/a%C3.md"), "file links: a broken escape or broken UTF-8 is refused");
        CHECK(!FileUrlToWindowsPath(L"file://server/share/x.md"), "file links: a UNC share is refused");
        CHECK(!FileUrlToWindowsPath(L"file:///K:/a/b.md:secret") && !FileUrlToWindowsPath(L"file:///K:/a/b%3A%24DATA"), "file links: a stream name is refused");
        CHECK(!FileUrlToWindowsPath(L"file:///K:/x/COM1") && !FileUrlToWindowsPath(L"file:///K:/x/nul.txt") && !FileUrlToWindowsPath(L"file:///K:/x/Con") &&
                  !FileUrlToWindowsPath(L"file:///K:/x/lpt9.log") && !FileUrlToWindowsPath(L"file:///K:/x/CONOUT$"),
              "file links: a device name is refused (COM1, nul.txt, Con, lpt9.log, CONOUT$)");
        CHECK(FileUrlToWindowsPath(L"file:///K:/x/console.txt").value_or(L"") == L"K:\\x\\console.txt" &&
                  FileUrlToWindowsPath(L"file:///K:/x/com10.md").value_or(L"") == L"K:\\x\\com10.md",
              "file links: a name that only starts like a device is fine (console.txt, com10.md)");
        CHECK(!FileUrlToWindowsPath(L"file:///K:/x/key.pem.") && !FileUrlToWindowsPath(L"file:///K:/x/key.pem%20"),
              "file links: a name ending in a dot or a space is refused (Win32 would open key.pem)");
        CHECK(!FileUrlToWindowsPath(L"https://x.y/a.md") && !FileUrlToWindowsPath(L"file:///relative/a.md"), "file links: not a drive file URL");

        const auto e = EncodeFileLinkPath(L"k:\\source\\x y\\r\x00E9port (1).md");
        CHECK(e && *e == "K/source/x%20y/r%C3%A9port%20%281%29.md", "file links: the URL path is encoded (drive uppercased, every unsafe byte escaped)");
        CHECK(DecodeFileLinkPath(*e).value_or(L"") == L"K:\\source\\x y\\r\x00E9port (1).md", "file links: the URL path decodes back");
        CHECK(!DecodeFileLinkPath("K/a/%2E%2E/b") && !DecodeFileLinkPath("K/a/b%3A%24DATA") && !DecodeFileLinkPath("K/a/%C0%AF") && !DecodeFileLinkPath("KK/a") &&
                  !DecodeFileLinkPath("K//a") && !DecodeFileLinkPath("K/a/COM1"),
              "file links: an encoded climb, a stream, overlong UTF-8, a two-letter drive, an empty segment and a device are refused");
        CHECK(!EncodeFileLinkPath(L"\\\\server\\share\\a.md") && !EncodeFileLinkPath(L"K:") && !EncodeFileLinkPath(L"K:\\a\\..\\b"), "file links: only a drive file path is encoded");
        CHECK(FileLinkPathKey(L"K:/Source/A.md") == FileLinkPathKey(L"k:\\source\\a.MD"), "file links: one key for any case and either slash");
        CHECK(FileLinkFileName(L"K:\\a\\b c.md") == L"b c.md", "file links: the file's name");
    }

    // ===== 3. the request target ========================================================================================
    {
        const auto iv = FileLinkInitialStateFromSecret(TestSecret());
        const std::string key = FileLinkSessionHash(iv, L"homepc", L"sid");
        const std::string encoded = EncodeFileLinkPath(L"K:\\source\\x y\\a.md").value_or("");
        FileLinkTargetError why{};
        const auto t = ParseFileLinkTarget("/station-homepc/" + key + "/" + encoded, &why);
        CHECK(t && t->station == L"homepc" && t->hash == key && t->path == L"K:\\source\\x y\\a.md" && t->encodedPath == encoded && why == FileLinkTargetError::None,
              "file links: a target's parts");
        CHECK(!ParseFileLinkTarget("/station-homepc/" + key + "/" + encoded + "?x", &why) && why == FileLinkTargetError::HasQuery, "file links: a query is refused");
        CHECK(!ParseFileLinkTarget("/station-Homepc/" + key + "/K/x", &why) && why == FileLinkTargetError::BadStation, "file links: an uppercase station is refused");
        CHECK(!ParseFileLinkTarget("/station-homepc/" + key.substr(1) + "/K/x", &why) && why == FileLinkTargetError::BadHash, "file links: a short key is refused");
        CHECK(!ParseFileLinkTarget("/stations-homepc/" + key + "/K/x", &why) && why == FileLinkTargetError::NotOurs, "file links: another shape is not ours");
        CHECK(!ParseFileLinkTarget("/station-homepc/" + key + "/K/../x", &why) && why == FileLinkTargetError::BadPath, "file links: a bad path is refused");
        CHECK(!ParseFileLinkTarget("/station-homepc/" + key + "/" + std::string(9000, 'a'), &why) && why == FileLinkTargetError::TooLong, "file links: a too long target is refused");
        CHECK(BuildFileLinkUrl(L"https://cc.sysd.cc/", L"homepc", key, encoded) == L"https://cc.sysd.cc/station-homepc/" + std::wstring(key.begin(), key.end()) + L"/K/source/x%20y/a.md",
              "file links: the link's form (the base's trailing slash dropped)");
    }

    // ===== 4. the Markdown rewrite ======================================================================================
    {
        std::vector<std::wstring> asked;
        const auto r = RewriteFileLinksInMarkdown(
            L"See [MESSAGING.md:4596](file:///K:/source/doc/MESSAGING.md \"t\") and `[x](file:///K:/a.md)` and\n```\n[y](file:///K:/b.md)\n```\n"
            L"and [z](https://x.y) [w](<file:///K:/c%20d.md>) [gone](file:///K:/gone.md).",
            [&asked](const std::wstring& path) -> std::optional<std::wstring> {
                asked.push_back(path);
                if (path == L"K:\\gone.md")
                {
                    return std::nullopt;
                }
                return L"https://cc.sysd.cc/L" + std::to_wstring(asked.size());
            });
        CHECK(r.rewritten == 2 && r.kept == 1, "file links: two links rewritten, one kept (code skipped)");
        CHECK(asked.size() == 3 && asked[0] == L"K:\\source\\doc\\MESSAGING.md" && asked[1] == L"K:\\c d.md" && asked[2] == L"K:\\gone.md",
              "file links: the resolver is asked for the links outside code, in order");
        CHECK(r.text.find(L"[MESSAGING.md:4596](https://cc.sysd.cc/L1 \"t\")") != std::wstring::npos, "file links: a link's title is kept");
        CHECK(r.text.find(L"[w](https://cc.sysd.cc/L2)") != std::wstring::npos, "file links: the angle form becomes a plain link");
        CHECK(r.text.find(L"`[x](file:///K:/a.md)`") != std::wstring::npos && r.text.find(L"[y](file:///K:/b.md)") != std::wstring::npos,
              "file links: a code span and a fenced block are left byte for byte");
        CHECK(r.text.find(L"[z](https://x.y)") != std::wstring::npos && r.text.find(L"[gone](file:///K:/gone.md)") != std::wstring::npos,
              "file links: other links and a declined link are left as they were");
        const auto none = RewriteFileLinksInMarkdown(L"no links here", [](const std::wstring&) -> std::optional<std::wstring> { return L"x"; });
        CHECK(none.text == L"no links here" && none.rewritten == 0 && none.kept == 0, "file links: a text without links is unchanged");
    }

    // ===== 5. files never linked; the settings' readers =================================================================
    {
        CHECK(LooksLikeSecretFile(L"C:\\Users\\ELI\\.ssh\\config") && LooksLikeSecretFile(L"C:\\Users\\ELI\\.claude\\.credentials.json") &&
                  LooksLikeSecretFile(L"C:\\Users\\ELI\\.config\\sysd\\provision.token") && LooksLikeSecretFile(L"K:\\x\\.env.local") &&
                  LooksLikeSecretFile(L"K:\\x\\server.KEY") && LooksLikeSecretFile(L"K:\\x\\id_ed25519.pub") && LooksLikeSecretFile(L"C:\\Users\\ELI\\.claude.json") &&
                  LooksLikeSecretFile(L"K:\\x\\kubeconfig"),
              "file links: secret names and folders are never linked");
        CHECK(!LooksLikeSecretFile(L"K:\\source\\Agentmaster\\doc\\agentmaster\\MESSAGING.md") && !LooksLikeSecretFile(L"K:\\x\\keys.md") && !LooksLikeSecretFile(L"K:\\x\\environment.md"),
              "file links: ordinary files are linkable");
        const auto ex = ParseFileLinkExcludedFolders(L"K:\\source\\proxmox\\\r\n# a note\n\n  C:/Users/ELI/private  \n\"D:\\quoted\"\nX");
        CHECK(ex.size() == 3 && ex[0] == L"k:\\source\\proxmox\\" && ex[1] == L"c:\\users\\eli\\private\\" && ex[2] == L"d:\\quoted\\",
              "file links: the excluded folders' list (notes, blanks, quotes and a too short line skipped)");
        CHECK(IsUnderExcludedFolder(L"K:\\Source\\Proxmox\\CLAUDE.md", ex) && !IsUnderExcludedFolder(L"K:\\source\\proxmox2\\x.md", ex) &&
                  !IsUnderExcludedFolder(L"K:\\source\\proxmox", ex),
              "file links: under an excluded folder (any case), not a sibling, not the folder itself");
        CHECK(NormalizeFileLinkStation(L"  HomePC ") == L"homepc" && NormalizeFileLinkStation(L"home pc").empty() && NormalizeFileLinkStation(L"-x").empty() &&
                  NormalizeFileLinkStation(std::wstring(33, L'a')).empty() && IsValidFileLinkStation(L"homepc-dev") && !IsValidFileLinkStation(L"HomePC"),
              "file links: the station is normalized, an invalid one refused");
        CHECK(NormalizeFileLinkBase(L" HTTPS://cc.sysd.cc/ ") == L"https://cc.sysd.cc" && NormalizeFileLinkBase(L"http://192.168.50.17:32594") == L"http://192.168.50.17:32594" &&
                  NormalizeFileLinkBase(L"http://[::1]:32594/") == L"http://[::1]:32594",
              "file links: a base is trimmed, its scheme lowercased, its trailing slash dropped");
        CHECK(NormalizeFileLinkBase(L"javascript:alert(1)").empty() && NormalizeFileLinkBase(L"https://x.y/a b").empty() && NormalizeFileLinkBase(L"https://x.y/\"a").empty() &&
                  NormalizeFileLinkBase(L"https://").empty() && NormalizeFileLinkBase(L"https://:443").empty() && NormalizeFileLinkBase(L"ftp://x.y").empty(),
              "file links: another scheme, a space, a quote or no host is refused");
        CHECK(ParseFileLinkMode(L"LAN") == FileLinkMode::Lan && ParseFileLinkMode(L" hosted ") == FileLinkMode::Hosted && ParseFileLinkMode(L"") == FileLinkMode::Off &&
                  ParseFileLinkMode(L"bogus") == FileLinkMode::Off && FileLinkModeWord(FileLinkMode::Hosted) == L"hosted",
              "file links: the mode's word both ways (anything unknown is Off)");
        CHECK(DefaultFileLinkPort(false) == 32594 && DefaultFileLinkPort(true) == 32595 && DefaultFileLinkStation(false) == L"homepc" && DefaultFileLinkStation(true) == L"homepc-dev",
              "file links: the release and dev installs' defaults");
    }

    // ===== 6. the HTTP pieces ===========================================================================================
    {
        const auto head = ParseHttpRequestHead("GET /station-homepc/x HTTP/1.1\r\nHost: cc.sysd.cc\r\nRange: bytes=0-\r\nCf-Connecting-Ip: 1.2.3.4");
        CHECK(head && head->method == "GET" && head->target == "/station-homepc/x" && head->Header("range") && *head->Header("RANGE") == "bytes=0-" &&
                  head->Header("Cf-Connecting-Ip") && *head->Header("cf-connecting-ip") == "1.2.3.4",
              "file links: a request head (headers by any case)");
        CHECK(!ParseHttpRequestHead("GET  /x HTTP/1.1") && !ParseHttpRequestHead("GET /x") && !ParseHttpRequestHead(""), "file links: a malformed request line is refused");
        FileByteRange br{};
        std::string rv = "bytes=10-";
        CHECK(ParseFileByteRange(&rv, 100, br) == FileRangeVerdict::Partial && br.first == 10 && br.last == 99, "file links: range a-");
        rv = "bytes=-10";
        CHECK(ParseFileByteRange(&rv, 100, br) == FileRangeVerdict::Partial && br.first == 90 && br.last == 99, "file links: range -n (the last n)");
        rv = "bytes=5-500";
        CHECK(ParseFileByteRange(&rv, 100, br) == FileRangeVerdict::Partial && br.first == 5 && br.last == 99, "file links: a range past the end is cut");
        rv = "bytes=100-";
        CHECK(ParseFileByteRange(&rv, 100, br) == FileRangeVerdict::Unsatisfiable, "file links: a range that starts past the end is unsatisfiable");
        rv = "bytes=0-1,5-6";
        CHECK(ParseFileByteRange(&rv, 100, br) == FileRangeVerdict::Whole, "file links: several ranges get the whole file");
        CHECK(ParseFileByteRange(nullptr, 100, br) == FileRangeVerdict::Whole, "file links: no Range header = the whole file");
        CHECK(ContentDispositionAttachment(L"r\x00E9port \"x\".md") == "attachment; filename=\"r_port _x_.md\"; filename*=UTF-8''r%C3%A9port%20%22x%22.md",
              "file links: the attachment header (an ASCII stand-in and the exact UTF-8 name)");
        CHECK(GuessFileMimeType(L"a.md").find("text/") == 0 && GuessFileMimeType(L"a.PNG") == "image/png" && GuessFileMimeType(L"a.unknown") == "application/octet-stream",
              "file links: the content type by extension");
    }

    // ===== 7. the linked set ============================================================================================
    {
        FileLinkStore st;
        CHECK(st.Add(L"sid1", L"K:\\a.md", 1000) && !st.Add(L"sid1", L"k:/A.MD", 2000), "file links: a file is new once per session (any case, either slash)");
        st.Add(L"sid2", L"K:\\a.md", 1500);
        const FileLinkEntry* e = st.Find(L"sid1", L"K:/a.md");
        CHECK(st.FileCount() == 2 && e && e->linkedMs == 2000 && e->path == L"k:/A.MD", "file links: a mention renews the link (the newest spelling kept)");
        CHECK(!st.Find(L"sid1", L"K:\\b.md") && !st.Find(L"sid3", L"K:\\a.md"), "file links: another file or session finds nothing");
        const auto back = FileLinkStoreFromJson(*json::Parse(json::Dump(FileLinkStoreToJson(st))));
        CHECK(back.FileCount() == 2 && back.Find(L"sid2", L"K:\\a.md") && back.Find(L"sid2", L"K:\\a.md")->linkedMs == 1500, "file links: the set's JSON round trip");
        CHECK(FileLinkStoreFromJson(*json::Parse(L"{\"v\":2,\"sessions\":{}}")).FileCount() == 0 &&
                  FileLinkStoreFromJson(*json::Parse(L"{\"v\":1,\"sessions\":{\"s\":[[\"K:\\\\a.md\",5],[1,2],[\"\",3],\"x\"]}}")).FileCount() == 1,
              "file links: an unknown version reads empty, damaged entries are skipped");
        FileLinkStore old;
        old.Add(L"s", L"K:\\old.md", 0);
        old.Add(L"s", L"K:\\new.md", kFileLinkLifetimeMs);
        CHECK(old.Prune(kFileLinkLifetimeMs + 10, kFileLinkLifetimeMs, 4000) == 1 && old.FileCount() == 1 && old.Find(L"s", L"K:\\new.md"), "file links: an expired link is dropped");
        FileLinkStore many;
        for (int i = 0; i < 10; ++i)
        {
            many.Add(L"s", L"K:\\f" + std::to_wstring(i) + L".md", 100 + i);
        }
        CHECK(many.Prune(200, kFileLinkLifetimeMs, 4) == 6 && many.FileCount() == 4 && many.Find(L"s", L"K:\\f9.md") && !many.Find(L"s", L"K:\\f5.md"),
              "file links: past the cap a session keeps its newest links");
        FileLinkStore empty;
        empty.Add(L"s", L"K:\\x.md", 0);
        empty.Prune(kFileLinkLifetimeMs + 1, kFileLinkLifetimeMs, 4000);
        CHECK(empty.sessions.empty(), "file links: a session left without links is dropped");
    }

    // ===== 8. LAN, peers, bases, /download, the command ==================================================================
    {
        CHECK(IsAllowedFileLinkPeer(0x7F000001u) && IsAllowedFileLinkPeer(0x0A000001u) && IsAllowedFileLinkPeer(0xAC100001u) && IsAllowedFileLinkPeer(0xAC1FFFFFu) &&
                  IsAllowedFileLinkPeer(0xC0A83211u),
              "file links: loopback and private peers are served");
        CHECK(!IsAllowedFileLinkPeer(0x08080808u) && !IsAllowedFileLinkPeer(0xAC200001u) && !IsAllowedFileLinkPeer(0xC0A90001u) && !IsAllowedFileLinkPeer(0x64400001u),
              "file links: public peers (and 172.32/16, 192.169/16, CGNAT) are not");
        std::vector<FileLinkLanAddress> addrs{ { L"vEthernet (WSL)", L"172.20.0.1", true }, { L"Wi-Fi", L"192.168.50.17", false }, { L"Wi-Fi", L"192.168.50.17", false } };
        const auto choices = ComposeFileLinkLanChoices(addrs, L"HOMEPC", 32594);
        CHECK(choices.size() == 3 && choices[0].url == L"http://192.168.50.17:32594" && choices[1].url == L"http://172.20.0.1:32594" && choices[2].url == L"http://homepc:32594",
              "file links: the LAN choices (physical first, virtual next, the PC's name last, duplicates dropped)");
        CHECK(choices[1].label.find(L"(virtual)") != std::wstring::npos, "file links: a virtual adapter is labeled");
        CHECK(ChooseFileLinkBase(FileLinkMode::Lan, L"", choices, L"") == L"http://192.168.50.17:32594" &&
                  ChooseFileLinkBase(FileLinkMode::Lan, L"http://homepc:32594", choices, L"") == L"http://homepc:32594" &&
                  ChooseFileLinkBase(FileLinkMode::Hosted, L"x", choices, L"") == L"https://cc.sysd.cc" &&
                  ChooseFileLinkBase(FileLinkMode::Hosted, L"", choices, L"https://files.example") == L"https://files.example" &&
                  ChooseFileLinkBase(FileLinkMode::Off, L"x", choices, L"y").empty() && ChooseFileLinkBase(FileLinkMode::Lan, L"", {}, L"").empty(),
              "file links: the base each mode uses");
        CHECK(ResolveDownloadArgument(L"  `src/../doc/x.md`  ", L"K:\\source\\Agentmaster").value_or(L"") == L"K:\\source\\Agentmaster\\doc\\x.md",
              "file links: /download takes a path relative to the session's folder");
        CHECK(ResolveDownloadArgument(L"\"K:/a b/c.md\"", L"").value_or(L"") == L"K:\\a b\\c.md" &&
                  ResolveDownloadArgument(L"file:///K:/a%20b/c.md", L"").value_or(L"") == L"K:\\a b\\c.md",
              "file links: /download takes a quoted absolute path and a file URL");
        CHECK(!ResolveDownloadArgument(L"", L"K:\\x") && !ResolveDownloadArgument(L"rel.md", L"") && !ResolveDownloadArgument(L"\\\\server\\share\\a", L"") &&
                  !ResolveDownloadArgument(L"x/COM1", L"K:\\a"),
              "file links: /download refuses nothing, a relative path with no folder, a share and a device");
        CHECK(ResolveDownloadArgument(L"../../../../x.md", L"K:\\a").value_or(L"") == L"K:\\x.md", "file links: a climb stops at the drive");
        const RemoteCommand cmd = ParseRemoteCommand(L"/download src/x.md");
        CHECK(cmd.kind == RemoteKind::AgentmasterCommand && cmd.name == L"download" && cmd.args == L"src/x.md", "file links: /download is Agentmaster's command (never sent to Claude Code)");
    }

    // ===== 9. the server end to end on loopback =========================================================================
    {
        WSADATA wsa{};
        const bool wsaUp = ::WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
        CHECK(wsaUp, "file links: Winsock starts for the test client");
        std::error_code ec;
        const std::filesystem::path dir = std::filesystem::temp_directory_path(ec) / L"agentmaster-m5-filelinks";
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir, ec);
        std::string content = "hello world\n";
        for (int i = 0; i < 3000; ++i)
        {
            content += static_cast<char>('a' + i % 26);
        }
        const std::wstring aPath = (dir / L"a b.md").wstring();
        const std::wstring keyPath = (dir / L"server.pem").wstring();
        WriteFixture(aPath, content);
        WriteFixture(keyPath, "-----BEGIN PRIVATE KEY-----\n");
        const auto iv = FileLinkInitialStateFromSecret(TestSecret());
        const std::string key = FileLinkSessionHash(iv, L"homepc", L"sid-1");
        const std::string aUrl = "/station-homepc/" + key + "/" + EncodeFileLinkPath(aPath).value_or("");
        const std::string keyUrl = "/station-homepc/" + key + "/" + EncodeFileLinkPath(keyPath).value_or("");
        std::mutex logMtx;
        std::vector<std::wstring> logs;
        {
            // The authorizer: this session's key, and the two linked files (the "secret" one too: the server's own final-path
            // check must refuse it even when the service said yes).
            FileLinkServer server(
                [&](const FileLinkTarget& t) {
                    FileServeDecision d;
                    if (t.hash == key && (FileLinkPathKey(t.path) == FileLinkPathKey(aPath) || FileLinkPathKey(t.path) == FileLinkPathKey(keyPath)))
                    {
                        d.status = 200;
                        d.path = t.path;
                        d.sessionId = L"sid-1";
                    }
                    else
                    {
                        d.reason = L"not linked";
                    }
                    return d;
                },
                [&](const std::wstring& line) {
                    std::lock_guard lk{ logMtx };
                    logs.push_back(line);
                });
            std::wstring error;
            const bool started = server.Start(0, error, true);
            CHECK(started && server.Running() && server.Port() != 0, "file links: the server starts on a free loopback port");
            const uint16_t port = server.Port();
            if (started)
            {
                const std::string health = HttpExchange(port, "GET /healthz HTTP/1.1\r\nHost: x\r\n\r\n");
                CHECK(StatusOf(health) == 200 && BodyOf(health) == "ok\n", "file links: /healthz answers ok");
                const std::string whole = HttpExchange(port, "GET " + aUrl + " HTTP/1.1\r\nHost: x\r\n\r\n");
                CHECK(StatusOf(whole) == 200 && BodyOf(whole) == content && HeaderOf(whole, "Content-Length") == std::to_string(content.size()),
                      "file links: a linked file is served whole");
                CHECK(HeaderOf(whole, "Content-Disposition").rfind("attachment; filename=\"a b.md\"", 0) == 0 && HeaderOf(whole, "X-Content-Type-Options") == "nosniff" &&
                          HeaderOf(whole, "Cache-Control") == "no-store" && HeaderOf(whole, "Accept-Ranges") == "bytes",
                      "file links: as an attachment, never sniffed, never cached");
                const std::string head = HttpExchange(port, "HEAD " + aUrl + " HTTP/1.1\r\nHost: x\r\n\r\n");
                CHECK(StatusOf(head) == 200 && BodyOf(head).empty() && HeaderOf(head, "Content-Length") == std::to_string(content.size()), "file links: HEAD gets the headers only");
                const std::string part = HttpExchange(port, "GET " + aUrl + " HTTP/1.1\r\nHost: x\r\nRange: bytes=6-10\r\n\r\n");
                CHECK(StatusOf(part) == 206 && BodyOf(part) == "world" && HeaderOf(part, "Content-Range") == "bytes 6-10/" + std::to_string(content.size()),
                      "file links: a range is served as 206");
                const std::string past = HttpExchange(port, "GET " + aUrl + " HTTP/1.1\r\nHost: x\r\nRange: bytes=999999-\r\n\r\n");
                CHECK(StatusOf(past) == 416, "file links: a range past the end is 416");
                std::string otherKey = key;
                otherKey[0] = otherKey[0] == 'a' ? 'b' : 'a';
                CHECK(StatusOf(HttpExchange(port, "GET /station-homepc/" + otherKey + "/" + EncodeFileLinkPath(aPath).value_or("") + " HTTP/1.1\r\n\r\n")) == 404,
                      "file links: another key is 404");
                CHECK(StatusOf(HttpExchange(port, "GET " + aUrl + "x HTTP/1.1\r\n\r\n")) == 404, "file links: a file the session did not link is 404");
                CHECK(StatusOf(HttpExchange(port, "GET " + aUrl + "?a=1 HTTP/1.1\r\n\r\n")) == 404 && StatusOf(HttpExchange(port, "GET / HTTP/1.1\r\n\r\n")) == 404,
                      "file links: a query and another path are 404");
                const std::string post = HttpExchange(port, "POST " + aUrl + " HTTP/1.1\r\nContent-Length: 0\r\n\r\n");
                CHECK(StatusOf(post) == 405 && HeaderOf(post, "Allow") == "GET, HEAD", "file links: POST is 405");
                CHECK(StatusOf(HttpExchange(port, "GARBAGE\r\n\r\n")) == 400, "file links: a malformed request is 400");
                CHECK(StatusOf(HttpExchange(port, "GET " + keyUrl + " HTTP/1.1\r\n\r\n")) == 404, "file links: a key file is refused by the server itself, even when authorized");
                const FileLinkServer::Totals t = server.Counts();
                CHECK(t.served == 3 && t.refused >= 7 && t.bytes == content.size() + 5, "file links: the totals (3 served, the bytes of the GET and the range)");
            }
            server.Stop();
            CHECK(!server.Running() && server.Port() == 0, "file links: the server stops");
            std::wstring error2;
            const bool again = server.Start(0, error2, true);
            CHECK(again && StatusOf(HttpExchange(server.Port(), "GET /healthz HTTP/1.1\r\n\r\n")) == 200 && server.Counts().requests == 1,
                  "file links: a stopped server starts again with fresh totals");
            server.Stop();
            std::wstring error3;
            CHECK(server.Start(0, error3, true), "file links: (a third start for the next check)");
            FileLinkServer second([](const FileLinkTarget&) { return FileServeDecision{}; }, nullptr);
            std::wstring busyError;
            CHECK(!second.Start(server.Port(), busyError, true) && busyError.find(L"in use") != std::wstring::npos, "file links: a port in use is refused with the reason");
        } // the destructor stops the server
        {
            std::lock_guard lk{ logMtx };
            bool sawServed = false;
            bool sawSecret = false;
            for (const auto& line : logs)
            {
                sawServed = sawServed || (line.find(L"[file-link] 200 GET homepc") == 0 && line.find(L"\"a b.md\"") != std::wstring::npos);
                sawSecret = sawSecret || line.find(L"looks like a secret") != std::wstring::npos;
            }
            CHECK(sawServed && sawSecret, "file links: the log names what was served and why a file was refused");
        }
        std::filesystem::remove_all(dir, ec);
        if (wsaUp)
        {
            ::WSACleanup();
        }
    }

    // ===== 10. the secret in Credential Manager =========================================================================
    {
        // A scratch target, deleted at the end (a crash leaves one harmless "m5" credential behind).
        const std::wstring target = L"Agentmaster:m5-test:filelinks:" + std::to_wstring(::GetCurrentProcessId()) + L"-" + std::to_wstring(::GetTickCount64());
        std::array<uint8_t, 32> first{};
        bool created = false;
        std::wstring error;
        CHECK(LoadOrCreateFileLinkSecret(target, first, created, error) && created, "file links: a missing secret is made and stored");
        std::array<uint8_t, 32> again{};
        bool createdAgain = true;
        CHECK(LoadOrCreateFileLinkSecret(target, again, createdAgain, error) && !createdAgain && again == first, "file links: the stored secret is read back");
        std::array<uint8_t, 32> replaced{};
        CHECK(ReplaceFileLinkSecret(target, replaced, error) && replaced != first, "file links: a replaced secret differs");
        std::array<uint8_t, 32> third{};
        bool createdThird = true;
        CHECK(LoadOrCreateFileLinkSecret(target, third, createdThird, error) && !createdThird && third == replaced, "file links: the replacement is what is stored");
        ::CredDeleteW(target.c_str(), CRED_TYPE_GENERIC, 0);
    }

    // The command list knows /download (so a session's topic never sends it to Claude Code).
    bool listed = false;
    for (const auto c : kAgentmasterCommands)
    {
        listed = listed || c == L"download";
    }
    CHECK(listed, "file links: /download is in kAgentmasterCommands");
}
