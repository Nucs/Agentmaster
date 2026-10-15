// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — the session database's DuckDB layer (SESSIONS_DB.md §3.1, §10): a thin RAII wrapper
// over DuckDB's official C API (duckdb.h, pinned by tools/Get-DuckDb.ps1).
//
// Why a wrapper at all: every DuckDB C handle must be destroyed exactly once, every failing call
// reports through a different accessor (duckdb_result_error, duckdb_prepare_error,
// duckdb_appender_error, ...), and a forgotten destroy leaks a whole query result. The classes below
// turn each handle into a move-only owner and each failure into a DbError carrying DuckDB's own
// message, so the service code reads like ordinary C++ and a failure can never be silently ignored.
//
// Deliberately NOT used: the deprecated per-value accessors (duckdb_value_varchar & co., which
// DuckDB 2.0 may remove) — results are read through the data-chunk API (duckdb_fetch_chunk), which is
// also the fast path. Complex values (LIST / STRUCT / MAP) are not decoded here: the SQL turns them
// into JSON text (to_json) and the caller parses that.
//
// Threading: a Database may be shared by threads; a Connection, a Prepared, a Result and an Appender
// belong to ONE thread at a time (DuckDB's rule). Connection::Interrupt is the one call that is safe
// from another thread (it is what cancels a running query).
//
// Plain C++ + Win32 (no WinRT): compiled into agentmaster-sessiondb.exe only — the GUI never loads
// duckdb.dll (SESSIONS_DB.md §4.2).

#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "duckdb.h"

namespace Agentmaster::SessionDb
{
    // A DuckDB call failed. what() carries DuckDB's own error text, prefixed with the operation that
    // failed ("query", "prepare", "append", ...) so a log line names the step without a stack.
    class DbError : public std::runtime_error
    {
    public:
        using std::runtime_error::runtime_error;
    };

    // The DuckDB options the service opens its file with (SESSIONS_DB.md §10). Every value is the
    // string form duckdb_set_config takes. The defaults are the design's: 4 threads, 1 GB, external
    // access OFF (no SQL may read or write a file — Agentmaster reads the transcripts itself), no
    // extension auto-install / auto-load, no community extensions.
    struct DbOptions
    {
        std::string threads{ "4" }; // worker threads for one query
        std::string memoryLimit{ "1GB" }; // ⚠ DuckDB's default is 80% of RAM — never leave it unset
        std::string tempDirectory; // spill folder (UTF-8 path); empty = DuckDB's default next to the file
        bool externalAccess{ false }; // false = SQL cannot touch the filesystem (read_text, COPY, ATTACH ...)
        bool readOnly{ false }; // open the file read-only (a tool that only inspects)
    };

    class Connection;

    // An open database file. Move-only; closing happens in the destructor (after every Connection of
    // it is gone — the caller keeps that order: connections are members destroyed first).
    class Database
    {
    public:
        // Open (or create) `pathUtf8` with `options`.
        //   pathUtf8 — the file, UTF-8. ":memory:" or empty opens an in-memory database (tests).
        //   options  — see DbOptions.
        // Returns the open database.
        // Throws DbError when DuckDB refuses (a corrupt file, a newer storage version, the file is
        // locked by another process — DuckDB allows one writer process per file, §4.2), or a setting
        // name it does not know (a pin bump that renamed an option: the message names it).
        static Database Open(const std::string& pathUtf8, const DbOptions& options);

        Database() noexcept = default;
        Database(Database&& other) noexcept;
        Database& operator=(Database&& other) noexcept;
        Database(const Database&) = delete;
        Database& operator=(const Database&) = delete;
        // Closes the file (duckdb_close checkpoints the write-ahead log). Never throws.
        ~Database();

        // The raw handle (connections are made from it). Valid while this object lives.
        duckdb_database Handle() const noexcept
        {
            return _db;
        }

        // True when a file is open.
        explicit operator bool() const noexcept
        {
            return _db != nullptr;
        }

    private:
        duckdb_database _db{ nullptr };
    };

    // A query result, read row by row. Wraps a materialized duckdb_result and walks its data chunks.
    // Move-only. The string_views it hands out point into the CURRENT chunk and stay valid only until
    // Next() moves past that chunk — copy what must outlive the row.
    class Result
    {
    public:
        Result() noexcept = default;
        // Adopt a filled duckdb_result (ownership moves here; it is destroyed by this object).
        explicit Result(duckdb_result result) noexcept;
        Result(Result&& other) noexcept;
        Result& operator=(Result&& other) noexcept;
        Result(const Result&) = delete;
        Result& operator=(const Result&) = delete;
        ~Result();

        // Advance to the next row. Returns false past the last row (then no accessor may be called).
        // The first call positions on the first row. Never throws.
        bool Next() noexcept;

        // Number of result columns.
        size_t ColumnCount() const noexcept
        {
            return _types.size();
        }

        // A column's name (as the SQL spelled it or DuckDB derived it). Empty for a bad index.
        std::string ColumnName(size_t col) const;

        // A column's DuckDB type id (DUCKDB_TYPE_*). DUCKDB_TYPE_INVALID for a bad index.
        duckdb_type ColumnType(size_t col) const noexcept
        {
            return col < _types.size() ? _types[col] : DUCKDB_TYPE_INVALID;
        }

        // Rows changed by an INSERT / UPDATE / DELETE (0 for a SELECT).
        int64_t RowsChanged() noexcept;

        // --- accessors for the CURRENT row (after a Next() that returned true) ---

        // True when the value is SQL NULL (or the column index is out of range).
        bool IsNull(size_t col) const noexcept;
        // The value as int64: any integer type, BOOLEAN (0/1), TIMESTAMP / TIME (microseconds),
        // DATE (days), DOUBLE / FLOAT (truncated). 0 for NULL or a type that is not numeric.
        int64_t Int64(size_t col) const noexcept;
        // The value as double: any numeric type. 0 for NULL / non-numeric.
        double Double(size_t col) const noexcept;
        // The value as bool: BOOLEAN, or an integer != 0. false for NULL.
        bool Bool(size_t col) const noexcept;
        // A VARCHAR / BLOB / ENUM-free string column's bytes (UTF-8 for VARCHAR). Empty for NULL or a
        // non-string type. ⚠ Valid only until Next() leaves the current chunk.
        std::string_view Str(size_t col) const noexcept;
        // Str() copied into an owned string.
        std::string String(size_t col) const
        {
            return std::string{ Str(col) };
        }

    private:
        // Load the next data chunk and cache its vectors. False when the result has no more chunks.
        bool _loadChunk() noexcept;
        // Release the current chunk (if any).
        void _dropChunk() noexcept;
        // Release everything (chunk + result).
        void _release() noexcept;

        duckdb_result _result{};
        bool _owns{ false };
        std::vector<duckdb_type> _types; // per column
        duckdb_data_chunk _chunk{ nullptr };
        size_t _chunkSize{ 0 };
        size_t _row{ 0 }; // index in the current chunk
        bool _started{ false };
        std::vector<void*> _data; // per column: the vector's data pointer for the current chunk
        std::vector<uint64_t*> _validity; // per column: validity mask (null = all valid)
    };

    // A prepared statement: bind parameters (1-based, DuckDB's convention), execute, repeat.
    // Move-only.
    class Prepared
    {
    public:
        Prepared() noexcept = default;
        Prepared(Prepared&& other) noexcept;
        Prepared& operator=(Prepared&& other) noexcept;
        Prepared(const Prepared&) = delete;
        Prepared& operator=(const Prepared&) = delete;
        ~Prepared();

        // Bind parameter `idx` (1-based). Each throws DbError when DuckDB rejects the binding (an
        // index past the statement's parameters, a value the parameter's type cannot take).
        Prepared& Bind(size_t idx, int64_t value);
        Prepared& Bind(size_t idx, int32_t value);
        Prepared& Bind(size_t idx, uint32_t value);
        Prepared& Bind(size_t idx, double value);
        Prepared& BindBool(size_t idx, bool value);
        // A UTF-8 string (VARCHAR). Pass valid UTF-8: DuckDB rejects invalid sequences.
        Prepared& Bind(size_t idx, std::string_view utf8);
        // Binary data (BLOB).
        Prepared& BindBlob(size_t idx, const void* data, size_t bytes);
        Prepared& BindNull(size_t idx);
        // A LIST of BIGINT (an `IN (SELECT unnest(?))` set without building SQL text).
        Prepared& BindInt64List(size_t idx, const std::vector<int64_t>& values);
        // A LIST of VARCHAR (UTF-8 strings).
        Prepared& BindStringList(size_t idx, const std::vector<std::string>& values);

        // Run with the current bindings. Throws DbError with DuckDB's message on failure (an
        // interrupted query reports "INTERRUPT Error: Interrupted!").
        Result Execute();
        // Forget every binding (the next Execute must bind again). Never throws.
        void ClearBindings() noexcept;
        // The statement type (DUCKDB_STATEMENT_TYPE_SELECT, ...) — the read-only SQL tool's gate (§10).
        duckdb_statement_type Type() const noexcept;
        // How many parameters the statement takes.
        size_t ParameterCount() const noexcept;

    private:
        friend class Connection;
        explicit Prepared(duckdb_prepared_statement st) noexcept : _st(st)
        {
        }
        // Throw DbError("bind <idx>: <message>") when `state` is an error.
        void _check(duckdb_state state, size_t idx, const char* what) const;

        duckdb_prepared_statement _st{ nullptr };
    };

    // An appender: fast row insertion into one table (DuckDB's bulk path). Values are appended column
    // by column, EndRow() closes a row. Data is visible to the connection's transaction after Flush()
    // or Close(); the caller flushes before COMMIT. Move-only; the destructor closes (flushes) it.
    class Appender
    {
    public:
        Appender() noexcept = default;
        Appender(Appender&& other) noexcept;
        Appender& operator=(Appender&& other) noexcept;
        Appender(const Appender&) = delete;
        Appender& operator=(const Appender&) = delete;
        // Destroys the appender (DuckDB flushes what is pending). A failure there is logged by the
        // caller's flow only if Close() was called explicitly — the destructor never throws.
        ~Appender();

        // Append one value to the current row. Each throws DbError on DuckDB's refusal (wrong type for
        // the column, a column past the table's end).
        Appender& Int64(int64_t v);
        Appender& Int32(int32_t v);
        Appender& UInt32(uint32_t v);
        Appender& UInt16(uint16_t v);
        Appender& UInt8(uint8_t v);
        Appender& Int16(int16_t v);
        Appender& Double(double v);
        Appender& Bool(bool v);
        // UTF-8 text (VARCHAR). ⚠ Must be valid UTF-8 (DuckDB checks).
        Appender& Str(std::string_view utf8);
        // UTF-8 text, or NULL when empty (the "only when it says something" columns).
        Appender& StrOrNull(std::string_view utf8);
        Appender& Blob(const void* data, size_t bytes);
        Appender& Null();
        // Close the current row. Throws DbError when the row is incomplete.
        void EndRow();
        // Push the buffered rows into the table (inside the connection's transaction). Throws DbError.
        void Flush();
        // Flush and release. Throws DbError when the flush fails; the appender is released either way.
        void Close();

    private:
        friend class Connection;
        explicit Appender(duckdb_appender app) noexcept : _app(app)
        {
        }
        // Throw DbError("append: <appender error>") when `state` is an error.
        void _check(duckdb_state state, const char* what);

        duckdb_appender _app{ nullptr };
    };

    // One connection to a Database. Owns its queries; move-only.
    class Connection
    {
    public:
        Connection() noexcept = default;
        // Connect to `db`. Throws DbError when DuckDB refuses (out of memory).
        explicit Connection(const Database& db);
        Connection(Connection&& other) noexcept;
        Connection& operator=(Connection&& other) noexcept;
        Connection(const Connection&) = delete;
        Connection& operator=(const Connection&) = delete;
        ~Connection();

        // Run SQL that returns nothing the caller needs (DDL, INSERT ... SELECT, SET, BEGIN ...).
        // Several statements separated by ';' are allowed. Throws DbError with DuckDB's message.
        void Execute(std::string_view sql);
        // Run a query and keep its result. Throws DbError.
        Result Query(std::string_view sql);
        // Compile a statement once for repeated execution. Throws DbError (a syntax or binder error).
        Prepared Prepare(std::string_view sql);
        // Make an appender for `table` in the main schema. Throws DbError (no such table).
        Appender MakeAppender(std::string_view table);
        // Cancel the query running on this connection right now (from ANOTHER thread). The running
        // call then fails with an INTERRUPT error. Safe on an idle connection. Never throws.
        void Interrupt() noexcept;

        // The single-query helpers below run one statement and read row 0 / column 0.
        // A scalar int64 (NULL or no row -> `fallback`). Throws DbError.
        int64_t ScalarInt64(std::string_view sql, int64_t fallback = 0);
        // A scalar string (NULL or no row -> empty). Throws DbError.
        std::string ScalarString(std::string_view sql);

        // The raw handle (for the read-only SQL tool's statement extraction). Valid while this lives.
        duckdb_connection Handle() const noexcept
        {
            return _con;
        }

    private:
        duckdb_connection _con{ nullptr };
    };

    // A transaction on one connection: BEGIN in the constructor, ROLLBACK in the destructor unless
    // Commit() ran. So an exception thrown between the two undoes every statement of the batch — the
    // invariant the ingest relies on (the cursor and the rows commit together or not at all, §7.6.3).
    class Transaction
    {
    public:
        // BEGIN TRANSACTION on `con`. Throws DbError (a transaction is already open on it).
        explicit Transaction(Connection& con);
        Transaction(const Transaction&) = delete;
        Transaction& operator=(const Transaction&) = delete;
        // Roll back when not committed. Never throws (a failed rollback is swallowed: the connection
        // is in an aborted state either way and DuckDB discards it at the next BEGIN).
        ~Transaction();
        // COMMIT. Throws DbError (a write conflict, a constraint violation found at commit, a full disk);
        // after a throw the destructor's rollback still runs.
        void Commit();

    private:
        Connection& _con;
        bool _done{ false };
    };

    // ===== small helpers =========================================================================

    // UTF-16 -> UTF-8. Invalid UTF-16 (a lone surrogate from a JSON "\uD800" escape) becomes U+FFFD,
    // so the result is always valid UTF-8 (DuckDB rejects anything else). Never throws beyond
    // std::bad_alloc.
    std::string ToUtf8(std::wstring_view wide);
    // UTF-8 -> UTF-16. Invalid bytes become U+FFFD. Never throws beyond std::bad_alloc.
    std::wstring FromUtf8(std::string_view utf8);
    // True when `bytes` is well-formed UTF-8 (no overlongs, no surrogates, nothing past U+10FFFF) —
    // what DuckDB demands of a VARCHAR. A line that fails is stored as a BLOB instead (§5.5). Pure.
    bool IsValidUtf8(std::string_view bytes) noexcept;
    // `text` as a single-quoted SQL string literal ('' doubles a quote). For the few statements that
    // cannot take a parameter (SET, PRAGMA). Pure.
    std::string SqlQuote(std::string_view text);
}
