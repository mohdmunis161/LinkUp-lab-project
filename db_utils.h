// Simple MySQL DB utilities
#pragma once

#include <string>

// Forward-declare MYSQL struct from MySQL C API (opaque pointer)
struct MYSQL;

struct DBConfig {
    std::string host = "127.0.0.1";
    unsigned int port = 3306;
    std::string user = "root"; // Assumption; adjust if different
    std::string password;        // Provided by user
    std::string database;        // e.g., linkup_db
    std::string unix_socket;     // leave empty to use TCP
    unsigned long client_flags = 0;
};

// RAII wrapper around MYSQL*
class DB {
public:
    DB();
    ~DB();

    // Non-copyable, movable
    DB(const DB&) = delete;
    DB& operator=(const DB&) = delete;
    DB(DB&&) noexcept;
    DB& operator=(DB&&) noexcept;

    // Connect using config; returns true on success
    bool connect(const DBConfig& cfg, std::string* err = nullptr);

    // Execute a query that returns a single first column of first row (e.g., SELECT 1)
    bool query_scalar(const std::string& sql, std::string& out, std::string* err = nullptr);

    // Raw handle accessor
    MYSQL* handle() const { return conn_; }

private:
    MYSQL* conn_ = nullptr;
};

// Execute an arbitrary SQL statement on an established connection and dump results to a file.
// - If the statement returns a result set, headers are written on the first line (tab-separated),
//   followed by rows (tab-separated). NULLs are empty.
// - If no result set (e.g., INSERT/UPDATE), writes a single line: "Affected rows: N".
// Returns true on success; on failure, 'err' (if provided) is set.
bool query_to_file(DB& db, const std::string& sql, const std::string& filepath, std::string* err = nullptr);
