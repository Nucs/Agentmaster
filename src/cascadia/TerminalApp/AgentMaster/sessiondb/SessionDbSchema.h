// SPDX-FileCopyrightText: 2026 Eli Belash <elibelash@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Agentmaster — the session database's SCHEMA (SESSIONS_DB.md §5): every table, the read-only views
// the tools and the `sql` tool see, and the version bookkeeping.
//
// Versioning (§11): meta.schema_version holds the version that created the file. EnsureSchema
// creates a fresh file at kSchemaVersion, accepts a file at the same version, and refuses a NEWER one
// (an older service never writes a newer file — the caller then reports "unavailable" and leaves the
// file alone). A future migration from an older version goes in EnsureSchema, version by version.
//
// The views (§5.3) are the only surface the read-only `sql` tool may name; they hide the storage
// details (line ids, folded columns, flag bits) behind the names the tool's description documents.

#pragma once

#include <string>

namespace Agentmaster::SessionDb
{
    class Connection;

    // What EnsureSchema found.
    enum class SchemaState
    {
        Created, // a new, empty database: the caller runs the cold build
        Current, // the file is at this service's schema version
        TooNew, // the file was written by a newer service: do not touch it
    };

    // Create every table, view and macro that is missing, and stamp / check meta.schema_version.
    //   con — a connection to the database (the writer connection, before any other work).
    // Returns the state (see SchemaState).
    // Throws DbError when a statement fails (a disk error, a corrupt file).
    SchemaState EnsureSchema(Connection& con);

    // The meta table's value for `key` ("" when absent). Throws DbError.
    std::string GetMeta(Connection& con, const std::string& key);
    // Set meta[key] = value (insert or replace). Throws DbError.
    void SetMeta(Connection& con, const std::string& key, const std::string& value);
}
