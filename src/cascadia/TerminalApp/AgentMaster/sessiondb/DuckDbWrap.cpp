// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — the RAII wrapper over DuckDB's C API (see DuckDbWrap.h for the contract).

#include "DuckDbWrap.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace Agentmaster::SessionDb
{
    namespace
    {
        // DuckDB hands out C strings it allocated (duckdb_open_ext's error); copy, then free with its
        // own allocator — a CRT free() on DuckDB's heap would corrupt it (the DLL may use another CRT).
        std::string TakeDuckDbString(char* s)
        {
            std::string out = s ? s : "";
            if (s)
            {
                duckdb_free(s);
            }
            return out;
        }

        // A message for a failed call whose accessor returned nothing (DuckDB always sets one; this is
        // the belt for an out-of-memory path that could not).
        std::string MessageOr(const char* msg, const char* fallback)
        {
            return (msg && *msg) ? std::string{ msg } : std::string{ fallback };
        }
    }

    // ===== Database ===============================================================================

    Database Database::Open(const std::string& pathUtf8, const DbOptions& options)
    {
        duckdb_config cfg{ nullptr };
        if (duckdb_create_config(&cfg) == DuckDBError)
        {
            throw DbError("open: duckdb_create_config failed (out of memory)");
        }
        // Every option goes through this lambda so a rejected one (a renamed setting after a pin
        // bump) names itself in the error, and the config is released on every path.
        const auto set = [&cfg](const char* name, const std::string& value) {
            if (duckdb_set_config(cfg, name, value.c_str()) == DuckDBError)
            {
                duckdb_destroy_config(&cfg);
                throw DbError(std::string("open: DuckDB rejected the option ") + name + "=" + value);
            }
        };
        set("threads", options.threads);
        set("memory_limit", options.memoryLimit);
        if (!options.tempDirectory.empty())
        {
            set("temp_directory", options.tempDirectory);
        }
        // SESSIONS_DB.md §10: no SQL may read or write a file (the read-only `sql` tool included), and
        // nothing is ever installed or loaded behind our back. JSON / ICU / Parquet are statically
        // linked into the official libduckdb, so they need no loading.
        set("enable_external_access", options.externalAccess ? "true" : "false");
        set("autoinstall_known_extensions", "false");
        set("autoload_known_extensions", "false");
        set("allow_community_extensions", "false");
        // A NEW file is written in the newest storage format this library knows. ⚠ DuckDB's default is an
        // old format kept for backward compatibility, and in it a column's `USING COMPRESSION zstd` is
        // silently stored Uncompressed (measured on 1.5: 460 MB vs 75 MB for the same 200 K strings). The
        // option only decides how a new file is created; an existing file keeps the version it has.
        set("storage_compatibility_version", "latest");
        if (options.readOnly)
        {
            set("access_mode", "READ_ONLY");
        }

        Database out;
        char* err = nullptr;
        const char* path = pathUtf8.empty() ? nullptr : pathUtf8.c_str(); // nullptr == in-memory
        const duckdb_state st = duckdb_open_ext(path, &out._db, cfg, &err);
        duckdb_destroy_config(&cfg);
        if (st == DuckDBError)
        {
            out._db = nullptr;
            throw DbError("open " + (pathUtf8.empty() ? std::string{ ":memory:" } : pathUtf8) + ": " + TakeDuckDbString(err));
        }
        if (err)
        {
            duckdb_free(err); // a success never sets it, but never leak one
        }
        return out;
    }

    Database::Database(Database&& other) noexcept :
        _db(std::exchange(other._db, nullptr))
    {
    }

    Database& Database::operator=(Database&& other) noexcept
    {
        if (this != &other)
        {
            if (_db)
            {
                duckdb_close(&_db);
            }
            _db = std::exchange(other._db, nullptr);
        }
        return *this;
    }

    Database::~Database()
    {
        if (_db)
        {
            duckdb_close(&_db); // checkpoints the WAL; DuckDB reports nothing back
        }
    }

    // ===== Result =================================================================================

    Result::Result(duckdb_result result) noexcept :
        _result(result), _owns(true)
    {
        // Cache the column types once: every accessor dispatches on them.
        const idx_t n = duckdb_column_count(&_result);
        _types.reserve(static_cast<size_t>(n));
        for (idx_t c = 0; c < n; ++c)
        {
            _types.push_back(duckdb_column_type(&_result, c));
        }
        _data.assign(_types.size(), nullptr);
        _validity.assign(_types.size(), nullptr);
    }

    Result::Result(Result&& other) noexcept :
        _result(other._result),
        _owns(std::exchange(other._owns, false)),
        _types(std::move(other._types)),
        _chunk(std::exchange(other._chunk, nullptr)),
        _chunkSize(std::exchange(other._chunkSize, 0)),
        _row(std::exchange(other._row, 0)),
        _started(std::exchange(other._started, false)),
        _data(std::move(other._data)),
        _validity(std::move(other._validity))
    {
        other._result = {};
    }

    Result& Result::operator=(Result&& other) noexcept
    {
        if (this != &other)
        {
            _release();
            _result = other._result;
            other._result = {};
            _owns = std::exchange(other._owns, false);
            _types = std::move(other._types);
            _chunk = std::exchange(other._chunk, nullptr);
            _chunkSize = std::exchange(other._chunkSize, 0);
            _row = std::exchange(other._row, 0);
            _started = std::exchange(other._started, false);
            _data = std::move(other._data);
            _validity = std::move(other._validity);
        }
        return *this;
    }

    Result::~Result()
    {
        _release();
    }

    void Result::_dropChunk() noexcept
    {
        if (_chunk)
        {
            duckdb_destroy_data_chunk(&_chunk);
            _chunk = nullptr;
        }
        _chunkSize = 0;
        _row = 0;
    }

    void Result::_release() noexcept
    {
        _dropChunk();
        if (_owns)
        {
            duckdb_destroy_result(&_result);
            _owns = false;
        }
    }

    bool Result::_loadChunk() noexcept
    {
        _dropChunk();
        if (!_owns)
        {
            return false;
        }
        // An empty chunk can appear mid-stream (a filter that dropped a whole vector): skip it.
        for (;;)
        {
            _chunk = duckdb_fetch_chunk(_result);
            if (!_chunk)
            {
                return false;
            }
            _chunkSize = static_cast<size_t>(duckdb_data_chunk_get_size(_chunk));
            if (_chunkSize != 0)
            {
                break;
            }
            duckdb_destroy_data_chunk(&_chunk);
            _chunk = nullptr;
        }
        for (size_t c = 0; c < _types.size(); ++c)
        {
            duckdb_vector v = duckdb_data_chunk_get_vector(_chunk, c);
            _data[c] = duckdb_vector_get_data(v);
            _validity[c] = duckdb_vector_get_validity(v); // nullptr == every row valid
        }
        _row = 0;
        return true;
    }

    bool Result::Next() noexcept
    {
        if (!_started)
        {
            _started = true;
            return _loadChunk();
        }
        if (!_chunk)
        {
            return false;
        }
        if (++_row < _chunkSize)
        {
            return true;
        }
        return _loadChunk();
    }

    std::string Result::ColumnName(size_t col) const
    {
        if (!_owns || col >= _types.size())
        {
            return {};
        }
        // duckdb_column_name takes a non-const pointer but does not modify the result.
        const char* n = duckdb_column_name(const_cast<duckdb_result*>(&_result), col);
        return n ? std::string{ n } : std::string{};
    }

    int64_t Result::RowsChanged() noexcept
    {
        return _owns ? static_cast<int64_t>(duckdb_rows_changed(&_result)) : 0;
    }

    bool Result::IsNull(size_t col) const noexcept
    {
        if (!_chunk || col >= _types.size())
        {
            return true;
        }
        const uint64_t* mask = _validity[col];
        return mask != nullptr && !duckdb_validity_row_is_valid(const_cast<uint64_t*>(mask), _row);
    }

    int64_t Result::Int64(size_t col) const noexcept
    {
        if (IsNull(col))
        {
            return 0;
        }
        const void* d = _data[col];
        switch (_types[col])
        {
        case DUCKDB_TYPE_BOOLEAN:
            return static_cast<const bool*>(d)[_row] ? 1 : 0;
        case DUCKDB_TYPE_TINYINT:
            return static_cast<const int8_t*>(d)[_row];
        case DUCKDB_TYPE_SMALLINT:
            return static_cast<const int16_t*>(d)[_row];
        case DUCKDB_TYPE_INTEGER:
            return static_cast<const int32_t*>(d)[_row];
        case DUCKDB_TYPE_BIGINT:
            return static_cast<const int64_t*>(d)[_row];
        case DUCKDB_TYPE_UTINYINT:
            return static_cast<const uint8_t*>(d)[_row];
        case DUCKDB_TYPE_USMALLINT:
            return static_cast<const uint16_t*>(d)[_row];
        case DUCKDB_TYPE_UINTEGER:
            return static_cast<const uint32_t*>(d)[_row];
        case DUCKDB_TYPE_UBIGINT:
            return static_cast<int64_t>(static_cast<const uint64_t*>(d)[_row]);
        case DUCKDB_TYPE_FLOAT:
            return static_cast<int64_t>(static_cast<const float*>(d)[_row]);
        case DUCKDB_TYPE_DOUBLE:
            return static_cast<int64_t>(static_cast<const double*>(d)[_row]);
        case DUCKDB_TYPE_TIMESTAMP:
        case DUCKDB_TYPE_TIMESTAMP_TZ:
            return static_cast<const duckdb_timestamp*>(d)[_row].micros;
        case DUCKDB_TYPE_TIME:
            return static_cast<const duckdb_time*>(d)[_row].micros;
        case DUCKDB_TYPE_DATE:
            return static_cast<const duckdb_date*>(d)[_row].days;
        case DUCKDB_TYPE_HUGEINT:
            // Only small values are expected (a sum() of BIGINTs widens to HUGEINT): keep the low word.
            return static_cast<int64_t>(static_cast<const duckdb_hugeint*>(d)[_row].lower);
        default:
            return 0;
        }
    }

    double Result::Double(size_t col) const noexcept
    {
        if (IsNull(col))
        {
            return 0;
        }
        switch (_types[col])
        {
        case DUCKDB_TYPE_FLOAT:
            return static_cast<const float*>(_data[col])[_row];
        case DUCKDB_TYPE_DOUBLE:
            return static_cast<const double*>(_data[col])[_row];
        case DUCKDB_TYPE_HUGEINT:
            return duckdb_hugeint_to_double(static_cast<const duckdb_hugeint*>(_data[col])[_row]);
        default:
            return static_cast<double>(Int64(col));
        }
    }

    bool Result::Bool(size_t col) const noexcept
    {
        return Int64(col) != 0;
    }

    std::string_view Result::Str(size_t col) const noexcept
    {
        if (IsNull(col))
        {
            return {};
        }
        const duckdb_type t = _types[col];
        if (t != DUCKDB_TYPE_VARCHAR && t != DUCKDB_TYPE_BLOB)
        {
            return {};
        }
        auto* s = static_cast<duckdb_string_t*>(_data[col]) + _row;
        const uint32_t len = duckdb_string_t_length(*s);
        const char* p = duckdb_string_t_data(s);
        return { p, len };
    }

    // ===== Prepared ===============================================================================

    Prepared::Prepared(Prepared&& other) noexcept :
        _st(std::exchange(other._st, nullptr))
    {
    }

    Prepared& Prepared::operator=(Prepared&& other) noexcept
    {
        if (this != &other)
        {
            if (_st)
            {
                duckdb_destroy_prepare(&_st);
            }
            _st = std::exchange(other._st, nullptr);
        }
        return *this;
    }

    Prepared::~Prepared()
    {
        if (_st)
        {
            duckdb_destroy_prepare(&_st);
        }
    }

    void Prepared::_check(duckdb_state state, size_t idx, const char* what) const
    {
        if (state == DuckDBError)
        {
            throw DbError("bind " + std::to_string(idx) + " (" + what + "): " +
                          MessageOr(_st ? duckdb_prepare_error(_st) : nullptr, "rejected"));
        }
    }

    Prepared& Prepared::Bind(size_t idx, int64_t value)
    {
        _check(duckdb_bind_int64(_st, idx, value), idx, "int64");
        return *this;
    }

    Prepared& Prepared::Bind(size_t idx, int32_t value)
    {
        _check(duckdb_bind_int32(_st, idx, value), idx, "int32");
        return *this;
    }

    Prepared& Prepared::Bind(size_t idx, uint32_t value)
    {
        _check(duckdb_bind_uint32(_st, idx, value), idx, "uint32");
        return *this;
    }

    Prepared& Prepared::Bind(size_t idx, double value)
    {
        _check(duckdb_bind_double(_st, idx, value), idx, "double");
        return *this;
    }

    Prepared& Prepared::BindBool(size_t idx, bool value)
    {
        _check(duckdb_bind_boolean(_st, idx, value), idx, "bool");
        return *this;
    }

    Prepared& Prepared::Bind(size_t idx, std::string_view utf8)
    {
        _check(duckdb_bind_varchar_length(_st, idx, utf8.data(), utf8.size()), idx, "varchar");
        return *this;
    }

    Prepared& Prepared::BindBlob(size_t idx, const void* data, size_t bytes)
    {
        _check(duckdb_bind_blob(_st, idx, data, bytes), idx, "blob");
        return *this;
    }

    Prepared& Prepared::BindNull(size_t idx)
    {
        _check(duckdb_bind_null(_st, idx), idx, "null");
        return *this;
    }

    namespace
    {
        // Build a LIST value of `type` from owned element values (the elements are destroyed here
        // whatever happens; the returned list is the caller's to destroy).
        duckdb_value MakeList(duckdb_type type, std::vector<duckdb_value>& elements)
        {
            duckdb_logical_type lt = duckdb_create_logical_type(type);
            // An empty list needs a non-null array pointer on some builds: point at a dummy.
            duckdb_value dummy = nullptr;
            duckdb_value list = duckdb_create_list_value(lt, elements.empty() ? &dummy : elements.data(), elements.size());
            for (auto& e : elements)
            {
                duckdb_destroy_value(&e);
            }
            elements.clear();
            duckdb_destroy_logical_type(&lt);
            return list;
        }
    }

    Prepared& Prepared::BindInt64List(size_t idx, const std::vector<int64_t>& values)
    {
        std::vector<duckdb_value> elements;
        elements.reserve(values.size());
        for (const int64_t v : values)
        {
            elements.push_back(duckdb_create_int64(v));
        }
        duckdb_value list = MakeList(DUCKDB_TYPE_BIGINT, elements);
        if (!list)
        {
            throw DbError("bind " + std::to_string(idx) + " (int64 list): could not build the list");
        }
        const duckdb_state st = duckdb_bind_value(_st, idx, list);
        duckdb_destroy_value(&list);
        _check(st, idx, "int64 list");
        return *this;
    }

    Prepared& Prepared::BindStringList(size_t idx, const std::vector<std::string>& values)
    {
        std::vector<duckdb_value> elements;
        elements.reserve(values.size());
        for (const auto& v : values)
        {
            elements.push_back(duckdb_create_varchar_length(v.data(), v.size()));
        }
        duckdb_value list = MakeList(DUCKDB_TYPE_VARCHAR, elements);
        if (!list)
        {
            throw DbError("bind " + std::to_string(idx) + " (varchar list): could not build the list");
        }
        const duckdb_state st = duckdb_bind_value(_st, idx, list);
        duckdb_destroy_value(&list);
        _check(st, idx, "varchar list");
        return *this;
    }

    Result Prepared::Execute()
    {
        duckdb_result res{};
        if (duckdb_execute_prepared(_st, &res) == DuckDBError)
        {
            std::string msg = MessageOr(duckdb_result_error(&res), "execution failed");
            duckdb_destroy_result(&res);
            throw DbError("execute: " + msg);
        }
        return Result{ res };
    }

    void Prepared::ClearBindings() noexcept
    {
        if (_st)
        {
            duckdb_clear_bindings(_st);
        }
    }

    duckdb_statement_type Prepared::Type() const noexcept
    {
        return _st ? duckdb_prepared_statement_type(_st) : DUCKDB_STATEMENT_TYPE_INVALID;
    }

    size_t Prepared::ParameterCount() const noexcept
    {
        return _st ? static_cast<size_t>(duckdb_nparams(_st)) : 0;
    }

    // ===== Appender ===============================================================================

    Appender::Appender(Appender&& other) noexcept :
        _app(std::exchange(other._app, nullptr))
    {
    }

    Appender& Appender::operator=(Appender&& other) noexcept
    {
        if (this != &other)
        {
            if (_app)
            {
                duckdb_appender_destroy(&_app);
            }
            _app = std::exchange(other._app, nullptr);
        }
        return *this;
    }

    Appender::~Appender()
    {
        if (_app)
        {
            // Destroy flushes; a failure here has no reporter (the caller that cares calls Close()).
            duckdb_appender_destroy(&_app);
        }
    }

    void Appender::_check(duckdb_state state, const char* what)
    {
        if (state == DuckDBError)
        {
            throw DbError(std::string("append (") + what + "): " + MessageOr(_app ? duckdb_appender_error(_app) : nullptr, "rejected"));
        }
    }

    Appender& Appender::Int64(int64_t v)
    {
        _check(duckdb_append_int64(_app, v), "int64");
        return *this;
    }

    Appender& Appender::Int32(int32_t v)
    {
        _check(duckdb_append_int32(_app, v), "int32");
        return *this;
    }

    Appender& Appender::UInt32(uint32_t v)
    {
        _check(duckdb_append_uint32(_app, v), "uint32");
        return *this;
    }

    Appender& Appender::UInt16(uint16_t v)
    {
        _check(duckdb_append_uint16(_app, v), "uint16");
        return *this;
    }

    Appender& Appender::UInt8(uint8_t v)
    {
        _check(duckdb_append_uint8(_app, v), "uint8");
        return *this;
    }

    Appender& Appender::Int16(int16_t v)
    {
        _check(duckdb_append_int16(_app, v), "int16");
        return *this;
    }

    Appender& Appender::Double(double v)
    {
        _check(duckdb_append_double(_app, v), "double");
        return *this;
    }

    Appender& Appender::Bool(bool v)
    {
        _check(duckdb_append_bool(_app, v), "bool");
        return *this;
    }

    Appender& Appender::Str(std::string_view utf8)
    {
        _check(duckdb_append_varchar_length(_app, utf8.data(), utf8.size()), "varchar");
        return *this;
    }

    Appender& Appender::StrOrNull(std::string_view utf8)
    {
        return utf8.empty() ? Null() : Str(utf8);
    }

    Appender& Appender::Blob(const void* data, size_t bytes)
    {
        _check(duckdb_append_blob(_app, data, bytes), "blob");
        return *this;
    }

    Appender& Appender::Null()
    {
        _check(duckdb_append_null(_app), "null");
        return *this;
    }

    void Appender::EndRow()
    {
        _check(duckdb_appender_end_row(_app), "end row");
    }

    void Appender::Flush()
    {
        _check(duckdb_appender_flush(_app), "flush");
    }

    void Appender::Close()
    {
        if (!_app)
        {
            return;
        }
        const duckdb_state st = duckdb_appender_close(_app);
        std::string msg = st == DuckDBError ? MessageOr(duckdb_appender_error(_app), "close failed") : std::string{};
        duckdb_appender_destroy(&_app);
        _app = nullptr;
        if (st == DuckDBError)
        {
            throw DbError("append (close): " + msg);
        }
    }

    // ===== Connection =============================================================================

    Connection::Connection(const Database& db)
    {
        if (!db || duckdb_connect(db.Handle(), &_con) == DuckDBError)
        {
            _con = nullptr;
            throw DbError("connect: DuckDB refused a connection (database closed, or out of memory)");
        }
    }

    Connection::Connection(Connection&& other) noexcept :
        _con(std::exchange(other._con, nullptr))
    {
    }

    Connection& Connection::operator=(Connection&& other) noexcept
    {
        if (this != &other)
        {
            if (_con)
            {
                duckdb_disconnect(&_con);
            }
            _con = std::exchange(other._con, nullptr);
        }
        return *this;
    }

    Connection::~Connection()
    {
        if (_con)
        {
            duckdb_disconnect(&_con);
        }
    }

    void Connection::Execute(std::string_view sql)
    {
        // duckdb_query needs a NUL-terminated string; a view may not be one.
        const std::string text{ sql };
        duckdb_result res{};
        const duckdb_state st = duckdb_query(_con, text.c_str(), &res);
        if (st == DuckDBError)
        {
            std::string msg = MessageOr(duckdb_result_error(&res), "query failed");
            duckdb_destroy_result(&res);
            throw DbError("query: " + msg);
        }
        duckdb_destroy_result(&res);
    }

    Result Connection::Query(std::string_view sql)
    {
        const std::string text{ sql };
        duckdb_result res{};
        if (duckdb_query(_con, text.c_str(), &res) == DuckDBError)
        {
            std::string msg = MessageOr(duckdb_result_error(&res), "query failed");
            duckdb_destroy_result(&res);
            throw DbError("query: " + msg);
        }
        return Result{ res };
    }

    Prepared Connection::Prepare(std::string_view sql)
    {
        const std::string text{ sql };
        duckdb_prepared_statement st{ nullptr };
        if (duckdb_prepare(_con, text.c_str(), &st) == DuckDBError)
        {
            std::string msg = MessageOr(st ? duckdb_prepare_error(st) : nullptr, "prepare failed");
            if (st)
            {
                duckdb_destroy_prepare(&st);
            }
            throw DbError("prepare: " + msg);
        }
        return Prepared{ st };
    }

    Appender Connection::MakeAppender(std::string_view table)
    {
        const std::string name{ table };
        duckdb_appender app{ nullptr };
        if (duckdb_appender_create(_con, nullptr, name.c_str(), &app) == DuckDBError)
        {
            std::string msg = MessageOr(app ? duckdb_appender_error(app) : nullptr, "appender refused");
            if (app)
            {
                duckdb_appender_destroy(&app);
            }
            throw DbError("appender " + name + ": " + msg);
        }
        return Appender{ app };
    }

    void Connection::Interrupt() noexcept
    {
        if (_con)
        {
            duckdb_interrupt(_con);
        }
    }

    int64_t Connection::ScalarInt64(std::string_view sql, int64_t fallback)
    {
        Result r = Query(sql);
        if (!r.Next() || r.ColumnCount() == 0 || r.IsNull(0))
        {
            return fallback;
        }
        return r.Int64(0);
    }

    std::string Connection::ScalarString(std::string_view sql)
    {
        Result r = Query(sql);
        if (!r.Next() || r.ColumnCount() == 0)
        {
            return {};
        }
        return r.String(0);
    }

    // ===== Transaction ============================================================================

    Transaction::Transaction(Connection& con) :
        _con(con)
    {
        _con.Execute("BEGIN TRANSACTION");
    }

    Transaction::~Transaction()
    {
        if (!_done)
        {
            try
            {
                _con.Execute("ROLLBACK");
            }
            catch (...)
            {
                // ⚠ DELIBERATELY QUIET (no LogSwallowedException here: this file is the DuckDB layer and
                // links no logger). A failed ROLLBACK leaves the connection's transaction aborted, which
                // DuckDB discards at the next BEGIN; the ORIGINAL failure that unwound through here is
                // what the caller logs.
            }
        }
    }

    void Transaction::Commit()
    {
        _con.Execute("COMMIT");
        _done = true;
    }

    // ===== helpers ================================================================================

    std::string ToUtf8(std::wstring_view wide)
    {
        if (wide.empty())
        {
            return {};
        }
        // Flags 0: a lone surrogate is REPLACED with U+FFFD (WC_ERR_INVALID_CHARS would fail instead),
        // which keeps the output valid UTF-8 for DuckDB.
        const int n = ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
        if (n <= 0)
        {
            return {};
        }
        std::string out(static_cast<size_t>(n), '\0');
        ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), n, nullptr, nullptr);
        return out;
    }

    std::wstring FromUtf8(std::string_view utf8)
    {
        if (utf8.empty())
        {
            return {};
        }
        const int n = ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
        if (n <= 0)
        {
            return {};
        }
        std::wstring out(static_cast<size_t>(n), L'\0');
        ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), n);
        return out;
    }

    bool IsValidUtf8(std::string_view bytes) noexcept
    {
        const auto* s = reinterpret_cast<const unsigned char*>(bytes.data());
        const size_t n = bytes.size();
        size_t i = 0;
        while (i < n)
        {
            const unsigned char c = s[i];
            if (c < 0x80)
            {
                ++i;
                continue;
            }
            size_t len = 0;
            uint32_t cp = 0;
            if ((c & 0xE0) == 0xC0)
            {
                len = 2;
                cp = c & 0x1F;
            }
            else if ((c & 0xF0) == 0xE0)
            {
                len = 3;
                cp = c & 0x0F;
            }
            else if ((c & 0xF8) == 0xF0)
            {
                len = 4;
                cp = c & 0x07;
            }
            else
            {
                return false; // a continuation byte where a lead was expected, or 0xF8..0xFF
            }
            if (i + len > n)
            {
                return false; // truncated sequence
            }
            for (size_t k = 1; k < len; ++k)
            {
                if ((s[i + k] & 0xC0) != 0x80)
                {
                    return false;
                }
                cp = (cp << 6) | (s[i + k] & 0x3F);
            }
            // Overlongs, surrogates and anything past U+10FFFF are what a strict decoder refuses.
            if ((len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) || (len == 4 && cp < 0x10000) ||
                (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF)
            {
                return false;
            }
            i += len;
        }
        return true;
    }

    std::string SqlQuote(std::string_view text)
    {
        std::string out;
        out.reserve(text.size() + 2);
        out.push_back('\'');
        for (const char c : text)
        {
            if (c == '\'')
            {
                out.push_back('\''); // SQL doubles a quote inside a literal
            }
            out.push_back(c);
        }
        out.push_back('\'');
        return out;
    }
}
