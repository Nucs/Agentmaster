// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — MessagingOutboxJournal: the FILES of the durable Telegram outbox (doc/agentmaster/MESSAGING_OUTBOX.md §4 –
// §6, MESSAGING.md §21.50). One folder, `<profile>\messaging\telegram\outbox\`:
//   outbox.jsonl - the journal: sealed lines (MessagingOutbox.h), appended by ONE WriteFile each, rewritten compact (only
//                  the live items) at every start and whenever it passes kOutboxCheckpointBytes;
//   dead.jsonl   - the items Telegram refused in every form, one line each, never read back by the service;
//   .lock        - held with NO sharing and FILE_FLAG_DELETE_ON_CLOSE while a process writes the journal: a second
//                  process cannot open it, and a killed process leaves none behind (Windows closes its handles).
// Durability (D-O3): a line is in the OS cache when Append returns (a process kill loses nothing), and on the disk within
// ~250 ms: a flusher thread flushes through a handle of its OWN, because a FlushFileBuffers on the appending handle stalls
// the next WriteFile on it (Windows serializes I/O per file object — measured: 1.3 ms median per append through the same
// handle, 39 µs through a second one; tools/telegram-outbox-bench/flush_contention.py).
//
// Plain C++ + Win32 (no WinRT, no PCH): the m5 harness tests it on a scratch folder (tests_messaging_outbox.cpp).

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

namespace Agentmaster::Msg
{
    /// <summary>
    /// The outbox journal's files (see the file comment). Thread-safe: Append may be called from any thread; Rewrite, Open and
    /// Close by their owner (the service), never concurrently with each other. Its own locks are LEAVES: no call made while one
    /// is held calls back into the service, logs, or takes another lock of the caller's (the service may hold its `_mtx` while
    /// it appends — MESSAGING_OUTBOX.md §5.1's order: the service's lock, then the journal's, never the reverse).
    /// </summary>
    class OutboxJournal
    {
    public:
        /// <summary>A closed journal: nothing is opened until Open.</summary>
        OutboxJournal() = default;
        /// <summary>Closes it (Close): the flusher is stopped, the last lines flushed, the lock file released and deleted.</summary>
        ~OutboxJournal();
        OutboxJournal(const OutboxJournal&) = delete;
        OutboxJournal& operator=(const OutboxJournal&) = delete;

        /// <summary>
        /// Takes the folder: creates it, takes `.lock` (no sharing, delete-on-close) and reads the whole journal. Nothing is
        /// open for appends yet: the caller folds the bytes and calls Rewrite with what is still live — the compacted journal
        /// replaces the file, its torn tail included.
        /// </summary>
        /// <param name="folder">The outbox folder (`…\messaging\telegram\outbox`).</param>
        /// <param name="bytes">Receives the journal's bytes ("" when there is none yet).</param>
        /// <param name="error">Receives why it failed, in words (another process holds the lock, the folder cannot be made, the
        /// file cannot be read).</param>
        /// <returns>True when the lock is held and the journal was read; false otherwise (nothing is held then).</returns>
        /// <exception cref="std::bad_alloc">If the paths or the bytes cannot be allocated (the lock is released first).</exception>
        bool Open(const std::wstring& folder, std::string& bytes, std::wstring& error);

        /// <summary>
        /// Replaces the journal with `content` (complete sealed lines: the header, then the live items): written to
        /// `outbox.jsonl.tmp`, flushed, then renamed over `outbox.jsonl` (MOVEFILE_WRITE_THROUGH) — a crash leaves either the
        /// old journal or the new one, never a mix. Then the append and flush handles are (re)opened on it and the flusher is
        /// started if it is not running. Holds every lock of the journal for its length, so the caller must make sure no
        /// append of an item it did not put into `content` can be waiting (the service runs it on its send thread with its own
        /// `_mtx` held: every other append happens under that lock or on that thread).
        /// </summary>
        /// <param name="content">The new journal's bytes.</param>
        /// <param name="error">Receives why it failed, in words.</param>
        /// <returns>True when the new journal is in place and open for appends. False when it could not be written or renamed —
        /// the old file is then opened for appends again when it can be (IsOpen says), so durability goes on uncompacted.</returns>
        /// <exception cref="std::bad_alloc">If a path cannot be allocated.</exception>
        bool Rewrite(std::string_view content, std::wstring& error);

        /// <summary>
        /// Appends complete line(s) in ONE WriteFile on the FILE_APPEND_DATA handle: the bytes land whole at the end of the file,
        /// never interleaved with another append (§5.2). Returns once they are in the OS cache; the flusher puts them on the disk
        /// within ~250 ms.
        /// </summary>
        /// <param name="bytes">Complete sealed lines, each ending in '\n'.</param>
        /// <returns>True when every byte was written; false when the journal is not open or the write failed (a full disk —
        /// counted in AppendFailures; the caller's item still goes, only not durably: MESSAGING_OUTBOX.md O4).</returns>
        bool Append(std::string_view bytes) noexcept;

        /// <summary>Appends one line to dead.jsonl (opened, written, flushed and closed each time: dead letters are rare).</summary>
        /// <param name="line">A complete sealed line.</param>
        /// <returns>True when it was written.</returns>
        bool AppendDead(std::string_view line) noexcept;

        /// <summary>Flushes what was appended now (the service's Stop and the app's last-window close), outside the flusher's
        /// rhythm. Waits for a flush in progress first.</summary>
        void FlushNow() noexcept;

        /// <summary>Stops the flusher, flushes, closes the journal and releases the lock (the lock file deletes itself). Safe to
        /// call twice, and on a journal that never opened.</summary>
        void Close() noexcept;

        /// <summary>Whether appends are possible (Rewrite opened the journal and Close has not run).</summary>
        /// <returns>True while open for appends.</returns>
        bool IsOpen() const noexcept;

        /// <summary>The journal's size as this process wrote it (the content of the last Rewrite plus every append since): the
        /// checkpoint's trigger.</summary>
        /// <returns>Bytes.</returns>
        uint64_t Bytes() const noexcept;

        /// <summary>Appends that failed since Open (a full disk, a lost handle): the census shows them.</summary>
        /// <returns>The count.</returns>
        uint64_t AppendFailures() const noexcept;

        /// <summary>The folder Open took ("" before).</summary>
        /// <returns>The path.</returns>
        const std::wstring& Folder() const noexcept;

    private:
        /// <summary>Opens the append and flush handles on the journal path (under `_mtx`). Returns false, with `error` set, when
        /// either cannot be opened (both are closed then).</summary>
        /// <param name="error">Receives why, in words.</param>
        /// <returns>True when both are open.</returns>
        bool _openHandlesLocked(std::wstring& error);
        /// <summary>Closes the append and flush handles (under `_mtx`).</summary>
        void _closeHandlesLocked() noexcept;
        /// <summary>One flush through the flush handle, if anything was appended since the last one. Takes `_flushMtx`.</summary>
        void _flushOnce() noexcept;
        /// <summary>The flusher thread: waits for an append, lets ~250 ms of appends gather, flushes; ends at Close.</summary>
        void _flusherLoop() noexcept;
        /// <summary>Starts the flusher thread if it is not running.</summary>
        void _startFlusher();

        /// <summary>The folder.</summary>
        std::wstring _folder;
        /// <summary>`<folder>\outbox.jsonl`.</summary>
        std::wstring _path;
        /// <summary>The `.lock` file's handle (HANDLE as void*; nullptr = not held).</summary>
        void* _lock{ nullptr };
        /// <summary>Guards the append and flush handles and `_bytes`' consistency with them. A LEAF: held only around a
        /// WriteFile, a handle copy or a handle swap.</summary>
        mutable std::mutex _mtx;
        /// <summary>The FILE_APPEND_DATA handle every append writes through (HANDLE as void*).</summary>
        void* _append{ nullptr };
        /// <summary>The handle the flusher flushes through — a second file object, so a flush never stalls an append.</summary>
        void* _flush{ nullptr };
        /// <summary>Held by the flusher for the length of a flush and by Rewrite and Close for theirs: a handle is never closed
        /// while a flush runs on it. Taken BEFORE `_mtx`, never after.</summary>
        std::mutex _flushMtx;
        /// <summary>The bytes written (the last Rewrite's content plus every append since).</summary>
        std::atomic<uint64_t> _bytes{ 0 };
        /// <summary>Appends that failed since Open.</summary>
        std::atomic<uint64_t> _appendFailures{ 0 };
        /// <summary>Guards `_dirty` and `_stopFlusher`, and the flusher's waits on `_cv`.</summary>
        std::mutex _cvMtx;
        /// <summary>Wakes the flusher: the first append after a flush, or the stop.</summary>
        std::condition_variable _cv;
        /// <summary>Something was appended since the last flush (under `_cvMtx`).</summary>
        bool _dirty{ false };
        /// <summary>The flusher must end (under `_cvMtx`).</summary>
        bool _stopFlusher{ false };
        /// <summary>The flusher thread (joinable while it runs).</summary>
        std::thread _flusher;
    };
}
