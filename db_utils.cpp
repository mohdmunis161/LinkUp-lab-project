// Minimal MySQL C API wrapper implementation
#include "db_utils.h"

#include <mysql/mysql.h>
#include <sstream>
#include <fstream>
#include <cstring>

DB::DB() : conn_(nullptr) {}

DB::~DB() {
    if (conn_) {
        mysql_close(conn_);
        conn_ = nullptr;
    }
}

DB::DB(DB&& other) noexcept : conn_(other.conn_) {
    other.conn_ = nullptr;
}

DB& DB::operator=(DB&& other) noexcept {
    if (this != &other) {
        if (conn_) mysql_close(conn_);
        conn_ = other.conn_;
        other.conn_ = nullptr;
    }
    return *this;
}

bool DB::connect(const DBConfig& cfg, std::string* err) {
    if (conn_) { mysql_close(conn_); conn_ = nullptr; }
    conn_ = mysql_init(nullptr);
    if (!conn_) { if (err) *err = "mysql_init failed"; return false; }
    // Optional: set timeouts
    unsigned int timeout = 5;
    mysql_options(conn_, MYSQL_OPT_CONNECT_TIMEOUT, &timeout);
    if (!mysql_real_connect(conn_,
                            cfg.host.c_str(),
                            cfg.user.c_str(),
                            cfg.password.c_str(),
                            cfg.database.empty() ? nullptr : cfg.database.c_str(),
                            cfg.port,
                            cfg.unix_socket.empty() ? nullptr : cfg.unix_socket.c_str(),
                            cfg.client_flags)) {
        if (err) *err = mysql_error(conn_);
        mysql_close(conn_);
        conn_ = nullptr;
        return false;
    }
    return true;
}

bool DB::query_scalar(const std::string& sql, std::string& out, std::string* err) {
    if (!conn_) { if (err) *err = "not connected"; return false; }
    if (mysql_query(conn_, sql.c_str()) != 0) {
        if (err) *err = mysql_error(conn_);
        return false;
    }
    MYSQL_RES* res = mysql_store_result(conn_);
    if (!res) {
        // Could be OK for queries without result sets; emulate empty
        out.clear();
        return true;
    }
    MYSQL_ROW row = mysql_fetch_row(res);
    if (row && row[0]) out = row[0]; else out.clear();
    mysql_free_result(res);
    return true;
}

bool query_to_file(DB& db, const std::string& sql, const std::string& filepath, std::string* err) {
    MYSQL* conn = db.handle();
    if (!conn) { if (err) *err = "not connected"; return false; }
    if (mysql_query(conn, sql.c_str()) != 0) { if (err) *err = mysql_error(conn); return false; }

    std::ofstream ofs(filepath, std::ios::out | std::ios::trunc);
    if (!ofs) { if (err) *err = "failed to open output file"; return false; }

    unsigned int field_count = mysql_field_count(conn);
    if (field_count == 0) {
        // No result set; DML or similar
        my_ulonglong affected = mysql_affected_rows(conn);
        ofs << "Affected rows: " << affected << "\n";
        ofs.close();
        return true;
    }

    MYSQL_RES* res = mysql_store_result(conn);
    if (!res) { if (err) *err = mysql_error(conn); return false; }

    unsigned int cols = mysql_num_fields(res);
    MYSQL_FIELD* fields = mysql_fetch_fields(res);
    // Header
    for (unsigned int i = 0; i < cols; ++i) {
        if (i) ofs << '\t';
        ofs << (fields[i].name ? fields[i].name : "");
    }
    ofs << '\n';

    // Rows
    MYSQL_ROW row;
    while ((row = mysql_fetch_row(res))) {
        unsigned long* lengths = mysql_fetch_lengths(res);
        for (unsigned int i = 0; i < cols; ++i) {
            if (i) ofs << '\t';
            if (row[i]) {
                ofs.write(row[i], lengths ? lengths[i] : (unsigned long)std::strlen(row[i]));
            }
        }
        ofs << '\n';
    }
    mysql_free_result(res);
    ofs.close();
    return true;
}
