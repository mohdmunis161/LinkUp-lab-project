
#include "nlohmann/json.hpp" // For JSON responses (header-only, add to project if not present)
#include "db_utils.h"        // MySQL DB config/types (credentials stored below)
#include "linkup_channels.h"
#include "linkup_chat.h"
#include "linkup_file_transfer.h"
#include "blocking_queue.h"
#include "ds_lab.h"
#include "toc.h"             // TOC features: DFA router, PDA, TM encryption, etc.
#include <string>
#include <iostream>
#include <iostream>
#include <string>
#include <thread>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <sstream>
#include <unistd.h>
#include <cstdio>
#include <ctime>
// Global content list root for sequence search and other features
content_node* g_content_list_root = nullptr;
#include <iomanip>
#include <fstream>
#include <openssl/sha.h>     // For SHA256 password hashing
#include "httplib.h" // REST API server
#include "gui_event_queue.h" // Outgoing event queue for GUI
#include <vector>
#include <mutex>
#include <condition_variable>
#include <unordered_map>
#include <random>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>

// ...existing includes...

// MySQL login configuration
// Centralized DB credentials for the app. Defaults can be overridden via environment variables:
//   LINKUP_DB_HOST, LINKUP_DB_PORT, LINKUP_DB_USER, LINKUP_DB_PASS, LINKUP_DB_NAME, LINKUP_DB_SOCKET
// Note: Avoid logging the password.
static DBConfig g_db_cfg = [](){
    DBConfig cfg;
    // Defaults per user request
    cfg.host = "127.0.0.1";
    cfg.port = 3306;
    cfg.user = "root";              // adjust if your MySQL user differs
    cfg.password = "muni321s123";   // provided password
    cfg.database = "linkup_db";     // provided database name
    cfg.unix_socket = "";           // empty -> use TCP
    // Environment overrides
    if (const char* v = std::getenv("LINKUP_DB_HOST"))   cfg.host = v;
    if (const char* v = std::getenv("LINKUP_DB_PORT"))   { int p = std::atoi(v); if (p > 0 && p < 65536) cfg.port = (unsigned)p; }
    if (const char* v = std::getenv("LINKUP_DB_USER"))   cfg.user = v;
    if (const char* v = std::getenv("LINKUP_DB_PASS"))   cfg.password = v;
    if (const char* v = std::getenv("LINKUP_DB_NAME"))   cfg.database = v;
    if (const char* v = std::getenv("LINKUP_DB_SOCKET")) cfg.unix_socket = v;
    return cfg;
}();
// Forward declaration removed; move implementation here

// Sequence search backend integration for frontend
std::string get_seq_search(const std::string& seq) {
    // 1. Get matching content IDs
    content_id_node* results = seq_search_fallback(g_content_list_root, seq);
    if (!results) return "[]";

    // 2. For each content_id, get type from DB
    DB db;
    std::string err;
    if (!db.connect(g_db_cfg, &err)) {
        std::cerr << "[SEQ_SEARCH] DB connect failed: " << err << std::endl;
        return "[]";
    }

    nlohmann::json output = nlohmann::json::array();
    for (content_id_node* p = results; p; p = p->next) {
        int content_id = p->content_id;
        std::string type;
        std::string type_query = "SELECT content_type FROM contents WHERE content_id=" + std::to_string(content_id);
        if (!db.query_scalar(type_query, type, &err)) {
            std::cerr << "[SEQ_SEARCH] Failed to get type for content_id=" << content_id << ": " << err << std::endl;
            continue;
        }
        // 3. For each (content_id, type), get content
        std::string content;
        std::ostringstream oss;
        oss << "SELECT db_get_history(" << content_id << ", '" << type << "')";
        if (!db.query_scalar(oss.str(), content, &err)) {
            std::cerr << "[SEQ_SEARCH] db_get_history failed for content_id=" << content_id << ": " << err << std::endl;
            continue;
        }
        // 4. Add to output array
        output.push_back({{"content_id", content_id}, {"type", type}, {"content", content}});
    }
    return output.dump();
}


// Global outgoing event queue for GUI delivery (declare before use)
GuiEventQueue gui_event_queue;


// Session management for multi-user authentication
std::atomic<uint64_t> g_current_user_id{1}; // Currently logged-in user (1 = default user)
static std::mutex g_sessions_mtx;
static std::unordered_map<std::string, uint64_t> g_sessions; // session_token -> user_id

// Broadcast log for GUI events so multiple SSE clients can receive all events
static std::mutex g_events_mtx;
static std::condition_variable g_events_cv;
static std::vector<std::string> g_events; // append-only event log

// TOC Feature Instances (Theory of Computation)
static SmartCommandRouter g_command_router;
static CommandSafetyChecker g_safety_checker;
static SecureSQLParser g_sql_parser;
static NestedCommandValidator g_nested_validator;
static MessageFilter g_message_filter;
static TMEncryption g_tm_encryption("linkup_secure_key_2025");

// Forward declarations
nlohmann::json get_history(uint64_t user_id = 1);

// ========================
// Authentication Helpers
// ========================

// Simple password hashing using SHA256 (basic implementation for demo)
// For production, use bcrypt or argon2
std::string hash_password(const std::string& password) {
    // Use OpenSSL SHA256 for proper password hashing
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256_CTX sha256;
    SHA256_Init(&sha256);
    SHA256_Update(&sha256, password.c_str(), password.length());
    SHA256_Final(hash, &sha256);
    
    std::ostringstream oss;
    for (int i = 0; i < SHA256_DIGEST_LENGTH; i++) {
        oss << std::hex << std::setw(2) << std::setfill('0') << (int)hash[i];
    }
    return oss.str();
}

// Generate random session token


// GUI event dispatcher

// Dispatcher: single consumer of gui_event_queue; prints and fans out to SSE log

void gui_event_dispatcher(std::atomic<bool>& running) {
    while (running.load()) {
        std::string event;
        if (!gui_event_queue.wait_pop(event, running)) break;
        // Debug print to terminal
        std::cout << "[GUI_EVENT] " << event << std::endl;

        // Parse event JSON and log to DB (ignore non-JSON events like "[FILE] ... ready!")
        if (event.empty() || event.front() != '{') {
            // Not JSON; just publish to SSE clients without DB logging
            {
                std::lock_guard<std::mutex> lk(g_events_mtx);
                g_events.push_back(std::move(event));
            }
            g_events_cv.notify_all();
            continue;
        }

        // Parse event JSON and log to DB
        try {
            auto j = nlohmann::json::parse(event);

            std::string type = j.value("type", "message");
            std::string direction;
            if (j.contains("direction")) {
                direction = j["direction"].get<std::string>();
            } else if (j.contains("sent")) {
                bool sent = j["sent"].is_boolean() ? j["sent"].get<bool>() : (j["sent"].is_number() ? (j["sent"].get<int>() != 0) : false);
                direction = sent ? "send" : "receive";
            } else {
                direction = "send";
            }
            std::string channel = j.value("channel", type == "file" ? "file" : "chat");
            
            // Extract user_id from event (1 if not present)
            uint64_t user_id = j.value("user_id", 1);
            // For received messages, use the currently logged-in user's id
            if (user_id == 1 && direction == "receive") {
                uint64_t global_user_id = g_current_user_id.load();
                if (global_user_id > 0) {
                    user_id = global_user_id;
                }
            }

            // Only allow valid enums for type/direction
            if (type != "message" && type != "file" && type != "system") type = "message";
            if (direction != "send" && direction != "receive") direction = "send";

            // Extract fields for messages and files
            std::string text_content;
            std::string file_name;
            uint64_t file_size = 0;

            if (type == "message" || type == "chat") {
                type = "message";
                text_content = j.value("text", "");
            } else if (type == "file") {
                // Extract file path from text field like "[FILE] Sending: path" or "[FILE] Received: path"
                std::string text = j.value("text", "");
                size_t colon_pos = text.find_last_of(':');
                if (colon_pos != std::string::npos && colon_pos + 1 < text.size()) {
                    file_name = text.substr(colon_pos + 1);
                    // Trim leading spaces
                    size_t start = file_name.find_first_not_of(" \t");
                    if (start != std::string::npos) file_name = file_name.substr(start);
                    
                    // Get file size using stat
                    struct stat st;
                    if (stat(file_name.c_str(), &st) == 0) {
                        file_size = static_cast<uint64_t>(st.st_size);
                    }
                }
            }

            // SQL escape helper for strings
            auto sql_escape = [](const std::string& s) -> std::string {
                std::string result;
                for (char c : s) {
                    if (c == '\'') result += "''";
                    else if (c == '\\') result += "\\\\";
                    else result += c;
                }
                return result;
            };

            DB db;
            std::string err;
            uint64_t contents_id = 0;
            
            if (db.connect(g_db_cfg, &err)) {
                std::ostringstream oss;
                oss << "SELECT log_contents(";
                // p_user_id: if still 0 (no logged-in user), store NULL to avoid FK errors
                if (user_id > 0) {
                    oss << user_id;
                } else {
                    oss << "NULL";
                }
                oss << ", '"
                    << type << "','"
                    << direction << "','"
                    << channel << "',";
                
                // p_text (NULL if file)
                if (type == "message") {
                    oss << "'" << sql_escape(text_content) << "',";
                } else {
                    oss << "NULL,";
                }
                
                // p_file_name (NULL if message)
                if (type == "file" && !file_name.empty()) {
                    oss << "'" << sql_escape(file_name) << "',";
                } else {
                    oss << "NULL,";
                }
                
                // p_file_size (NULL if message or unknown)
                if (type == "file" && file_size > 0) {
                    oss << file_size;
                } else {
                    oss << "NULL";
                }
                
                oss << ") AS contents_id";
                
                std::string result;
                if (db.query_scalar(oss.str(), result, &err)) {
                    try {
                        contents_id = std::stoull(result);
                        if (contents_id == 0) {
                            std::cerr << "[DB][log_contents] Insert failed (returned 0)" << std::endl;
                        }
                    } catch (...) {
                        std::cerr << "[DB][log_contents] Failed to parse contents_id: " << result << std::endl;
                    }
                } else {
                    std::cerr << "[DB][log_contents] Query failed: " << err << std::endl;
                }
            } else {
                std::cerr << "[DB][log_contents] Connect failed: " << err << std::endl;
            }
            
            // Inject contents_id into the event JSON
            if (contents_id > 0) {
                j["contents_id"] = contents_id;
                event = j.dump();
            }
        } catch (const std::exception& ex) {
            std::cerr << "[DB][log_contents] JSON parse error: " << ex.what() << std::endl;
        }

        // Append to event log and notify SSE clients
        {
            std::lock_guard<std::mutex> lk(g_events_mtx);
            g_events.push_back(std::move(event));
        }
        g_events_cv.notify_all();
    }
}

// REST API server thread definition (now after all types are known)
void rest_api_thread(LinkUpChat* chat, FileTransfer* file_transfer, std::atomic<bool>& running, int rest_port) {
    httplib::Server svr;

    // Helper: execute shell command and write output to file, return file path
    auto generate_cmd_output_file = [](const std::string& cmd) -> std::string {
        auto t = std::time(nullptr);
        std::tm tm; localtime_r(&t, &tm);
        std::ostringstream oss; oss << "$_" << std::put_time(&tm, "%Y%m%d_%H%M%S") << "_output.txt";
        std::string filename = oss.str();
        std::string full_cmd = cmd + " 2>&1";
        FILE* pipe = popen(full_cmd.c_str(), "r");
        if (!pipe) return "";
        std::ofstream ofs(filename);
        char buffer[4096];
        while (fgets(buffer, sizeof(buffer), pipe)) ofs << buffer;
        pclose(pipe);
        ofs.close();
        return filename;
    };

    // Helper: execute SQL query and write result to file, return file path
    auto generate_sql_output_file = [](const std::string& sql) -> std::string {
        auto t = std::time(nullptr);
        std::tm tm; localtime_r(&t, &tm);
        std::ostringstream oss; oss << "%_" << std::put_time(&tm, "%Y%m%d_%H%M%S");
        std::string base = oss.str();
        std::string filename = base + "_output.txt";
        DB db;
        std::string err;
        if (!db.connect(g_db_cfg, &err)) {
            std::cerr << "[DB] connect failed: " << err << std::endl;
            // Write error file with details
            std::string errfile = base + "_error.txt";
            std::ofstream ofs(errfile);
            ofs << "DB connect failed: " << err << "\n";
            ofs << "Query: " << sql << "\n";
            ofs.close();
            return errfile;
        }
        if (!query_to_file(db, sql, filename, &err)) {
            std::cerr << "[DB] query failed: " << err << std::endl;
            // Write error file with details
            std::string errfile = base + "_error.txt";
            std::ofstream ofs(errfile);
            ofs << "SQL execution failed: " << err << "\n";
            ofs << "Query: " << sql << "\n";
            ofs.close();
            return errfile;
        }
        return filename;
    };

    // Helper: create shorthand by calling log_shorthand function
    auto create_shorthand = [](uint64_t user_id, const std::string& key, const std::string& value) -> int {
        DB db;
        std::string err;
        if (!db.connect(g_db_cfg, &err)) {
            std::cerr << "[SHORTHAND] DB connect failed: " << err << std::endl;
            return 0;
        }
        
        // SQL escape helper
        auto sql_escape = [](const std::string& s) -> std::string {
            std::string result;
            for (char c : s) {
                if (c == '\'') result += "''";
                else if (c == '\\') result += "\\\\";
                else result += c;
            }
            return result;
        };
        
        // First check if the key is reserved
        std::ostringstream check_oss;
        check_oss << "SELECT is_reserved('" << sql_escape(key) << "')";
        std::string reserved_result;
        if (!db.query_scalar(check_oss.str(), reserved_result, &err)) {
            std::cerr << "[SHORTHAND] is_reserved check failed: " << err << std::endl;
            return 0;
        }
        
        int is_reserved_val = std::atoi(reserved_result.c_str());
        if (is_reserved_val == 1) {
            std::cout << "[SHORTHAND] Key '" << key << "' is reserved, cannot create" << std::endl;
            return 2; // Return 2 for reserved
        }
        
        // Not reserved, proceed with creation
        std::ostringstream oss;
        oss << "SELECT log_shorthand(" << user_id << ", '" << sql_escape(key) << "', '" << sql_escape(value) << "')";
        std::string result;
        if (!db.query_scalar(oss.str(), result, &err)) {
            std::cerr << "[SHORTHAND] log_shorthand failed: " << err << std::endl;
            return 0;
        }
        
        // Parse result as int (0 = failure, 1 = success)
        int ret = std::atoi(result.c_str());
        std::cout << "[SHORTHAND] Created: user_id=" << user_id << ", key='" << key << "', result=" << ret << std::endl;
        return ret;
    };

    // Helper: get shorthand value by key and version
    auto get_shorthand = [](uint64_t user_id, const std::string& text) -> std::string {
        // Parse >key or >key:vX or >search:seq
        if (text.empty() || text[0] != '>') return "0";

        std::string key;
        int version = 0;
        std::string seq;

        size_t colon_pos = text.find(':');
        if (colon_pos != std::string::npos) {
            key = text.substr(1, colon_pos - 1);
            std::string v_part = text.substr(colon_pos + 1);
            if (key == "search") {
                seq = v_part;
            } else if (v_part.size() > 1 && v_part[0] == 'v') {
                version = std::atoi(v_part.substr(1).c_str());
            }
        } else {
            key = text.substr(1);
            version = 0;
        }

        if (key == "search" && !seq.empty()) {
            // Call get_seq_search and return its result
            return get_seq_search(seq);
        }

        DB db;
        std::string err;
        if (!db.connect(g_db_cfg, &err)) {
            std::cerr << "[SHORTHAND] DB connect failed: " << err << std::endl;
            return "0";
        }

        // SQL escape helper
        auto sql_escape = [](const std::string& s) -> std::string {
            std::string result;
            for (char c : s) {
                if (c == '\'') result += "''";
                else if (c == '\\') result += "\\\\";
                else result += c;
            }
            return result;
        };

        std::ostringstream oss;
        oss << "SELECT db_get_shorthand(" << user_id << ", '" << sql_escape(key) << "', " << version << ")";
        std::string result;
        if (!db.query_scalar(oss.str(), result, &err)) {
            std::cerr << "[SHORTHAND] db_get_shorthand failed: " << err << std::endl;
            return "0";
        }

        std::cout << "[SHORTHAND] Retrieved: user_id=" << user_id << ", key='" << key << "', v=" << version << ", value='" << result << "'" << std::endl;

        // If this was the reserved 'del' shorthand, db_get_shorthand/res_del will return '1' or '0'
        if ((key == "del" || key.rfind("del#", 0) == 0) && (result == "1" || result == "0")) {
            // Short-circuit: return the result directly to caller (frontend)
            return result;
        }

        return result;
    };

    // Helper: handle 'del' reserved shorthand
    auto res_del = [](uint64_t user_id, const std::string& key) -> std::string {
        DB db;
        std::string err;
        if (!db.connect(g_db_cfg, &err)) {
            std::cerr << "[RES_DEL] DB connect failed: " << err << std::endl;
            return "0";
        }
        
        // Check if key is exactly 'del' (delete all for this user)
        if (key == "del") {
            std::string result;
            std::ostringstream oss;
            oss << "SELECT db_del_content(" << user_id << ", 0)";
            if (!db.query_scalar(oss.str(), result, &err)) {
                std::cerr << "[RES_DEL] Delete all failed: " << err << std::endl;
                return "0";
            }
            return result;
        }
        
        // Check if key starts with 'del#' (delete specific IDs)
        if (key.length() > 4 && key.substr(0, 4) == "del#") {
            std::string content_ids = key.substr(4);
            if (content_ids.empty()) {
                return "0";
            }
            
            // Parse comma-separated content IDs
            std::stringstream ss(content_ids);
            std::string id_str;
            while (std::getline(ss, id_str, ',')) {
                // Trim whitespace
                id_str.erase(0, id_str.find_first_not_of(" \t"));
                id_str.erase(id_str.find_last_not_of(" \t") + 1);
                
                if (id_str.empty()) continue;
                
                // Convert to integer and delete
                try {
                    uint64_t content_id = std::stoull(id_str);
                    std::string result;
                    std::ostringstream oss;
                    oss << "SELECT db_del_content(" << user_id << ", " << content_id << ")";
                    if (!db.query_scalar(oss.str(), result, &err)) {
                        std::cerr << "[RES_DEL] Delete content " << content_id << " failed: " << err << std::endl;
                        return "0";
                    }
                    if (result != "1") {
                        std::cerr << "[RES_DEL] Delete content " << content_id << " returned: " << result << std::endl;
                        return "0";
                    }
                } catch (const std::exception& e) {
                    std::cerr << "[RES_DEL] Invalid content ID: " << id_str << std::endl;
                    return "0";
                }
            }
            return "1";
        }
        
        // Key doesn't match del pattern
        return "0";
    };

    // Helper: delete shorthand by key; returns 1 on success, 0 otherwise
    auto del_shorthand = [](uint64_t user_id, const std::string& key) -> int {
        DB db;
        std::string err;
        if (!db.connect(g_db_cfg, &err)) {
            std::cerr << "[SHORTHAND][DEL] DB connect failed: " << err << std::endl;
            return 0;
        }
        // SQL escape helper
        auto sql_escape = [](const std::string& s) -> std::string {
            std::string result;
            for (char c : s) {
                if (c == '\'') result += "''";
                else if (c == '\\') result += "\\\\";
                else result += c;
            }
            return result;
        };
        std::ostringstream oss;
        oss << "SELECT db_del_shorthand(" << user_id << ", '" << sql_escape(key) << "')";
        std::string result;
        if (!db.query_scalar(oss.str(), result, &err)) {
            std::cerr << "[SHORTHAND][DEL] db_del_shorthand failed: " << err << std::endl;
            return 0;
        }
        int ret = std::atoi(result.c_str());
        std::cout << "[SHORTHAND][DEL] Deleted: user_id=" << user_id << ", key='" << key << "' result=" << ret << std::endl;
        return ret;
    };

    // ========================
    // Authentication Endpoints
    // ========================

    // CORS preflight for auth endpoints
    svr.Options("/signup", [](const httplib::Request& req, httplib::Response& res){
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("Access-Control-Allow-Methods", "POST, OPTIONS");
        auto req_headers = req.get_header_value("Access-Control-Request-Headers");
        res.set_header("Access-Control-Allow-Headers", req_headers.empty() ? "content-type" : req_headers.c_str());
        res.status = 204;
    });

    svr.Options("/login", [](const httplib::Request& req, httplib::Response& res){
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("Access-Control-Allow-Methods", "POST, OPTIONS");
        auto req_headers = req.get_header_value("Access-Control-Request-Headers");
        res.set_header("Access-Control-Allow-Headers", req_headers.empty() ? "content-type" : req_headers.c_str());
        res.status = 204;
    });

    svr.Options("/logout", [](const httplib::Request& req, httplib::Response& res){
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("Access-Control-Allow-Methods", "POST, OPTIONS");
        auto req_headers = req.get_header_value("Access-Control-Request-Headers");
        res.set_header("Access-Control-Allow-Headers", req_headers.empty() ? "content-type, authorization" : req_headers.c_str());
        res.status = 204;
    });

    // POST /signup - Register new user
    svr.Post("/signup", [](const httplib::Request& req, httplib::Response& res){
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("Access-Control-Allow-Methods", "POST, OPTIONS");
        try {
            auto j = nlohmann::json::parse(req.body);
            if (!j.contains("username") || !j.contains("password")) {
                res.status = 400;
                res.set_content(R"({"error":"Missing username or password"})", "application/json");
                return;
            }
            std::string username = j["username"].get<std::string>();
            std::string password = j["password"].get<std::string>();
            std::string email = j.value("email", "");
            std::string display_name = j.value("display_name", "");
            if (username.empty() || password.empty()) {
                res.status = 400;
                res.set_content(R"({"error":"Username and password cannot be empty"})", "application/json");
                return;
            }
            std::string password_hash = hash_password(password);
            auto sql_escape = [](const std::string& s) -> std::string {
                std::string result;
                for (char c : s) {
                    if (c == '\'') result += "''";
                    else if (c == '\\') result += "\\\\";
                    else result += c;
                }
                return result;
            };
            DB db;
            std::string err;
            if (!db.connect(g_db_cfg, &err)) {
                res.status = 500;
                res.set_content(R"({"error":"Database connection failed"})", "application/json");
                return;
            }
            std::ostringstream oss;
            oss << "SELECT create_user('"
                << sql_escape(username) << "', '"
                << sql_escape(password_hash) << "', "
                << (email.empty() ? "NULL" : ("'" + sql_escape(email) + "'")) << ", "
                << (display_name.empty() ? "NULL" : ("'" + sql_escape(display_name) + "'"))
                << ")";
            std::string result;
            if (!db.query_scalar(oss.str(), result, &err)) {
                res.status = 500;
                res.set_content(R"({"error":"User creation failed"})", "application/json");
                return;
            }
            uint64_t user_id = std::stoull(result);
            if (user_id == 0) {
                res.status = 409;
                res.set_content(R"({"error":"Username already exists"})", "application/json");
                return;
            }
            g_current_user_id.store(user_id);
            std::cout << "[AUTH] User created: username=" << username << ", user_id=" << user_id << std::endl;
            nlohmann::json response;
            response["status"] = "ok";
            response["user_id"] = user_id;
            response["message"] = "User created successfully";
            res.set_content(response.dump(), "application/json");
        } catch (const std::exception& ex) {
            res.status = 400;
            res.set_content(R"({"error":"Invalid request"})", "application/json");
        }
    });

    // POST /login - Authenticate user
    svr.Post("/login", [](const httplib::Request& req, httplib::Response& res){
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("Access-Control-Allow-Methods", "POST, OPTIONS");
        try {
            auto j = nlohmann::json::parse(req.body);
            if (!j.contains("username") || !j.contains("password")) {
                res.status = 400;
                res.set_content(R"({"error":"Missing username or password"})", "application/json");
                return;
            }
            std::string username = j["username"].get<std::string>();
            std::string password = j["password"].get<std::string>();
            std::string password_hash = hash_password(password);
            auto sql_escape = [](const std::string& s) -> std::string {
                std::string result;
                for (char c : s) {
                    if (c == '\'') result += "''";
                    else if (c == '\\') result += "\\\\";
                    else result += c;
                }
                return result;
            };
            DB db;
            std::string err;
            if (!db.connect(g_db_cfg, &err)) {
                res.status = 500;
                res.set_content(R"({"error":"Database connection failed"})", "application/json");
                return;
            }
            std::ostringstream oss;
            oss << "SELECT verify_user('" << sql_escape(username) << "', '" << sql_escape(password_hash) << "')";
            std::string result;
            if (!db.query_scalar(oss.str(), result, &err)) {
                res.status = 500;
                res.set_content(R"({"error":"Authentication failed"})", "application/json");
                return;
            }
            uint64_t user_id = std::stoull(result);
            if (user_id == 0) {
                res.status = 401;
                res.set_content(R"({"error":"Invalid username or password"})", "application/json");
                return;
            }
            g_current_user_id.store(user_id);
            std::cout << "[AUTH] User logged in: username=" << username << ", user_id=" << user_id << std::endl;
            nlohmann::json response;
            response["status"] = "ok";
            response["user_id"] = user_id;
            res.set_content(response.dump(), "application/json");
        } catch (const std::exception& ex) {
            res.status = 400;
            res.set_content(R"({"error":"Invalid request"})", "application/json");
        }
    });

    // POST /logout - End session
    svr.Post("/logout", [](const httplib::Request& req, httplib::Response& res){
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("Access-Control-Allow-Methods", "POST, OPTIONS");
        try {
            // Reset global user_id to 1
            g_current_user_id.store(1);
            std::cout << "[AUTH] User logged out, user_id reset to 1" << std::endl;
            nlohmann::json response;
            response["status"] = "ok";
            response["message"] = "Logged out successfully";
            res.set_content(response.dump(), "application/json");
        } catch (const std::exception& ex) {
            res.status = 400;
            res.set_content(R"({"error":"Invalid request"})", "application/json");
        }
    });

    // ========================
    // Chat and File Endpoints
    // ========================

    // CORS preflight for /chat
    svr.Options("/chat", [](const httplib::Request& req, httplib::Response& res){
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
        auto req_headers = req.get_header_value("Access-Control-Request-Headers");
        res.set_header("Access-Control-Allow-Headers", req_headers.empty() ? "content-type, authorization" : req_headers.c_str());
        res.status = 204;
    });

    // POST /chat
    svr.Post("/chat", [chat, file_transfer, generate_cmd_output_file, generate_sql_output_file, create_shorthand, get_shorthand, res_del, del_shorthand](const httplib::Request& req, httplib::Response& res){
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("Access-Control-Allow-Methods", "POST, OPTIONS");
        
        try {
            auto j = nlohmann::json::parse(req.body);
            if (!j.contains("message")) { 
                res.status = 400;
                res.set_content(R"({"error":"Missing 'message' field"})", "application/json");
                return;
            }
            
            uint64_t user_id = g_current_user_id.load();
            if (user_id == 0) {
                user_id = 1; // Default to user 1 as fallback
            }
            std::string msg = j["message"].get<std::string>();
            
            // ============================================================
            // TOC FEATURE 1: SMART COMMAND ROUTER (DFA-based parsing)
            // Now includes PDA logic for nested command detection
            // ============================================================
            ParsedCommand parsed_cmd = g_command_router.parse(msg);
            
            std::cout << "[TOC][ROUTER] Input: \"" << msg << "\" -> Type: " 
                      << SmartCommandRouter::type_to_string(parsed_cmd.type) 
                      << ", Priority: " << parsed_cmd.priority << std::endl;
            
            if (!parsed_cmd.is_valid) {
                res.status = 400;
                nlohmann::json error_response;
                error_response["error"] = "Invalid command";
                error_response["details"] = parsed_cmd.error_msg;
                res.set_content(error_response.dump(), "application/json");
                return;
            }
            
            // ============================================================
            // TOC FEATURE 4: NESTED COMMAND VALIDATOR (PDA)
            // Integrated with DFA - process nested commands
            // ============================================================
            if (parsed_cmd.type == CommandType::NESTED) {
                std::cout << "[TOC][PDA] Nested structure detected, processing..." << std::endl;
                
                // Use PDA to parse the nested structure
                NestedCommand nested_result = g_nested_validator.parse(msg);
                
                std::cout << "[TOC][PDA] Valid: " << (nested_result.is_valid ? "YES" : "NO") 
                          << ", Nesting Level: " << nested_result.nesting_level 
                          << ", Commands: " << nested_result.commands.size() << std::endl;
                
                if (!nested_result.is_valid) {
                    res.status = 400;
                    nlohmann::json error_response;
                    error_response["error"] = "Invalid nested structure";
                    error_response["reason"] = nested_result.error_msg;
                    res.set_content(error_response.dump(), "application/json");
                    return;
                }
                
                // Execute commands from innermost to outermost
                // Commands are already sorted by depth (innermost first)
                std::cout << "[TOC][PDA] Executing " << nested_result.commands.size() 
                          << " commands in INNERMOST→OUTERMOST order" << std::endl;
                
                std::vector<std::string> output_files;
                std::vector<std::string> chat_messages;
                bool has_special_commands = false;
                
                for (size_t i = 0; i < nested_result.commands.size(); i++) {
                    std::string cmd = nested_result.commands[i];
                    cmd.erase(0, cmd.find_first_not_of(" \t\n\r\f\v"));
                    cmd.erase(cmd.find_last_not_of(" \t\n\r\f\v") + 1);
                    
                    if (cmd.empty()) continue;
                    
                    std::cout << "[TOC][PDA] [" << (i+1) << "/" << nested_result.commands.size() 
                              << "] Executing: \"" << cmd << "\"" << std::endl;
                    
                    // Parse each nested command
                    ParsedCommand nested_parsed = g_command_router.parse(cmd);
                    std::cout << "[TOC][PDA] Nested type: " << SmartCommandRouter::type_to_string(nested_parsed.type) << std::endl;
                    
                    if (nested_parsed.type == CommandType::SHORTHAND_GET) {
                        std::string value = get_shorthand(user_id, cmd);
                        if (value != "0") {
                            std::cout << "[TOC][PDA] Shorthand expanded: \"" << cmd << "\" -> \"" << value << "\"" << std::endl;
                            chat_messages.push_back(value);
                        }
                    }
                    else if (nested_parsed.type == CommandType::SHELL) {
                        // Check safety first
                        SafetyReport safety = g_safety_checker.analyze(nested_parsed.value);
                        std::cout << "[TOC][PDA] Safety check for shell: " << (safety.allow_execution ? "SAFE" : "BLOCKED") << std::endl;
                        
                        if (!safety.allow_execution) {
                            res.status = 403;
                            nlohmann::json error_response;
                            error_response["error"] = "Nested command blocked for safety";
                            error_response["reason"] = safety.reason;
                            error_response["command"] = cmd;
                            res.set_content(error_response.dump(), "application/json");
                            return;
                        }
                        
                        // Execute the shell command and get output file
                        std::string out_path = generate_cmd_output_file(nested_parsed.value);
                        if (!out_path.empty()) {
                            std::cout << "[TOC][PDA] Shell command executed, output file: " << out_path << std::endl;
                            output_files.push_back(out_path);
                            has_special_commands = true;
                        }
                    }
                    else if (nested_parsed.type == CommandType::SQL) {
                        // Check SQL safety
                        SafeQuery safe_query = g_sql_parser.parse_and_validate(nested_parsed.value);
                        std::cout << "[TOC][PDA] SQL query safe: " << (safe_query.is_safe ? "YES" : "NO") << std::endl;
                        
                        if (!safe_query.is_safe) {
                            res.status = 403;
                            nlohmann::json error_response;
                            error_response["error"] = "Nested SQL query blocked";
                            error_response["reason"] = safe_query.error_msg;
                            error_response["command"] = cmd;
                            res.set_content(error_response.dump(), "application/json");
                            return;
                        }
                        
                        // Execute the SQL query and get output file
                        std::string out_path = generate_sql_output_file(nested_parsed.value);
                        if (!out_path.empty()) {
                            std::cout << "[TOC][PDA] SQL query executed, output file: " << out_path << std::endl;
                            output_files.push_back(out_path);
                            has_special_commands = true;
                        }
                    }
                    else if (nested_parsed.type == CommandType::CHAT) {
                        // Filter chat messages in nested structure
                        std::string action_taken;
                        std::string filtered_msg = g_message_filter.apply_filters(cmd, action_taken);
                        
                        if (action_taken.find("blocked") != std::string::npos) {
                            res.status = 403;
                            nlohmann::json error_response;
                            error_response["error"] = "Nested message blocked by filter";
                            error_response["reason"] = action_taken;
                            error_response["command"] = cmd;
                            res.set_content(error_response.dump(), "application/json");
                            return;
                        }
                        
                        // Store chat message to send later
                        chat_messages.push_back(filtered_msg);
                        std::cout << "[TOC][PDA] Chat message queued: \"" << filtered_msg << "\"" << std::endl;
                    }
                }
                
                // Send all output files from shell/sql commands
                if (has_special_commands && !output_files.empty()) {
                    std::cout << "[TOC][PDA] Sending " << output_files.size() << " output files from nested commands" << std::endl;
                    
                    for (const auto& out_file : output_files) {
                        file_transfer->send_file(out_file);
                        std::cout << "[TOC][PDA] Sent file: " << out_file << std::endl;
                    }
                }
                
                // Send all chat messages
                if (!chat_messages.empty()) {
                    std::cout << "[TOC][PDA] Sending " << chat_messages.size() << " chat messages from nested commands" << std::endl;
                    
                    for (const auto& chat_msg : chat_messages) {
                        std::cout << "[TOC][PDA] Sending chat message: \"" << chat_msg << "\"" << std::endl;
                        chat->send_message(chat_msg);
                        
                        // Manually push GUI event with user_id for DB logging
                        nlohmann::json event_json;
                        event_json["type"] = "chat";
                        event_json["text"] = chat_msg;
                        event_json["sent"] = true;
                        event_json["user_id"] = user_id;
                        event_json["channel"] = "chat";
                        gui_event_queue.push(event_json.dump());
                        
                        std::cout << "[GUI_EVENT] " << event_json.dump() << std::endl;
                    }
                }
                
                // Return success response
                res.set_header("Access-Control-Allow-Origin", "*");
                res.set_header("Access-Control-Allow-Methods", "POST, OPTIONS");
                nlohmann::json response;
                response["status"] = "ok";
                if (!output_files.empty()) {
                    response["files"] = output_files;
                }
                if (!chat_messages.empty()) {
                    response["chat_messages"] = chat_messages;
                }
                response["nested_commands"] = nested_result.commands.size();
                res.set_content(response.dump(), "application/json");
                return;
            }
            
            // Route based on parsed command type
            if (parsed_cmd.type == CommandType::SHORTHAND_CREATE) {
                int result = create_shorthand(user_id, parsed_cmd.key, parsed_cmd.value);
                if (result == 1) {
                    res.set_content(R"({"status":"ok","action":"shorthand_created","message":"Shorthand successfully created"})", "application/json");
                } else if (result == 2) {
                    res.set_content(R"({"status":"error","action":"shorthand_reserved","message":"Reserved shorthand - cannot be used"})", "application/json");
                } else {
                    res.set_content(R"({"status":"error","action":"shorthand_failed","message":"Shorthand creation failed"})", "application/json");
                }
                return;
            }
            else if (parsed_cmd.type == CommandType::SHORTHAND_DELETE) {
                int result = del_shorthand(user_id, parsed_cmd.key);
                if (result == 1) {
                    res.set_content(R"({"status":"ok","action":"shorthand_deleted","message":"Shorthand deletion successful"})", "application/json");
                } else {
                    res.set_content(R"({"status":"error","action":"shorthand_delete_failed","message":"Shorthand deletion failed"})", "application/json");
                }
                return;
            }
            else if (parsed_cmd.type == CommandType::CONTENT_DELETE) {
                std::string result = res_del(user_id, parsed_cmd.key);
                if (result == "1") {
                    res.set_content(R"({"status":"success","action":"content_deleted","message":"Content deletion successful"})", "application/json");
                } else {
                    res.set_content(R"({"status":"error","action":"content_delete_failed","message":"Content deletion failed"})", "application/json");
                }
                return;
            }
            else if (parsed_cmd.type == CommandType::SEARCH) {
                std::string search_result = get_seq_search(parsed_cmd.value);
                res.set_content(nlohmann::json({{"status","ok"}, {"value", search_result}}).dump(), "application/json");
                return;
            }
            else if (parsed_cmd.type == CommandType::SHORTHAND_GET) {
                std::string value = get_shorthand(user_id, msg);
                if (value == "0") {
                    res.set_content(R"({"status":"error","action":"shorthand_not_found","message":"Shorthand not found"})", "application/json");
                    return;
                }
                // Replace msg with the retrieved value and process recursively
                msg = value;
                std::cout << "[SHORTHAND] Expanded to: " << msg << std::endl;
                // Re-parse the expanded command
                parsed_cmd = g_command_router.parse(msg);
                std::cout << "[TOC][ROUTER] Re-parsed: \"" << msg << "\" -> Type: " 
                          << SmartCommandRouter::type_to_string(parsed_cmd.type) << std::endl;
            }
            
            // ============================================================
            // TOC FEATURE 6: COMMAND SAFETY CHECKER (Halting Problem)
            // ============================================================
            if (parsed_cmd.type == CommandType::SHELL) {
                SafetyReport safety = g_safety_checker.analyze(parsed_cmd.value);
                
                std::cout << "[TOC][SAFETY] Command: \"" << parsed_cmd.value << "\" -> Level: ";
                switch (safety.level) {
                    case SafetyLevel::SAFE: std::cout << "SAFE"; break;
                    case SafetyLevel::RISKY: std::cout << "RISKY"; break;
                    case SafetyLevel::DANGEROUS: std::cout << "DANGEROUS"; break;
                    case SafetyLevel::INFINITE_LOOP: std::cout << "INFINITE_LOOP"; break;
                }
                std::cout << ", Reason: " << safety.reason << std::endl;
                
                if (!safety.allow_execution) {
                    res.status = 403;
                    nlohmann::json error_response;
                    error_response["error"] = "Command blocked for safety";
                    error_response["reason"] = safety.reason;
                    error_response["level"] = static_cast<int>(safety.level);
                    res.set_content(error_response.dump(), "application/json");
                    return;
                }
                
                // Execute with timeout based on safety report
                std::string out_path = generate_cmd_output_file(parsed_cmd.value);
                if (!out_path.empty()) {
                    file_transfer->send_file(out_path);
                    res.set_content(std::string("{\"status\":\"ok\",\"file\":\"") + out_path + "\"}", "application/json");
                } else {
                    res.status = 500;
                    res.set_content(R"({"error":"Failed to execute command"})", "application/json");
                }
                return;
            }
            else if (parsed_cmd.type == CommandType::SQL) {
                // ============================================================
                // TOC FEATURE 3: SECURE SQL PARSER (CFG)
                // ============================================================
                SafeQuery safe_query = g_sql_parser.parse_and_validate(parsed_cmd.value);
                
                std::cout << "[TOC][SQL_PARSER] Query: \"" << parsed_cmd.value << "\" -> Safe: " 
                          << (safe_query.is_safe ? "YES" : "NO") << std::endl;
                
                if (!safe_query.is_safe) {
                    res.status = 403;
                    nlohmann::json error_response;
                    error_response["error"] = "SQL query blocked";
                    error_response["reason"] = safe_query.error_msg;
                    res.set_content(error_response.dump(), "application/json");
                    return;
                }
                
                std::cout << "[REST][SQL] Executing: " << parsed_cmd.value << std::endl;
                std::string out_path = generate_sql_output_file(parsed_cmd.value);
                if (out_path.empty()) {
                    // Fallback: write the error context to a file and send it so UI doesn't alert-fail
                    auto t = std::time(nullptr);
                    std::tm tm; localtime_r(&t, &tm);
                    std::ostringstream oss; oss << "%_" << std::put_time(&tm, "%Y%m%d_%H%M%S") << "_error.txt";
                    out_path = oss.str();
                    std::ofstream ofs(out_path);
                    ofs << "SQL execution failed.\n\n";
                    ofs << "Query:\n" << parsed_cmd.value << "\n\n";
                    ofs << "See server logs for detailed error." << std::endl;
                    ofs.close();
                    std::cerr << "[REST][SQL] Failed to execute query; sent error file instead: " << out_path << std::endl;
                }
                // Send the file (result or error) and return 200 always to avoid UI alert
                file_transfer->send_file(out_path);
                // FileTransfer will emit GUI event after successful send
                res.set_header("Access-Control-Allow-Origin", "*"); 
                res.set_header("Access-Control-Allow-Methods", "POST, OPTIONS"); 
                res.set_content(std::string("{\"status\":\"ok\",\"file\":\"") + out_path + "\"}", "application/json");
                return;
            }
            else if (parsed_cmd.type == CommandType::FILE) {
                file_transfer->send_file(parsed_cmd.value);
                res.set_header("Access-Control-Allow-Origin", "*");
                res.set_header("Access-Control-Allow-Methods", "POST, OPTIONS");
                res.set_content(std::string("{\"status\":\"ok\",\"file\":\"") + parsed_cmd.value + "\"}", "application/json");
                return;
            }
            else if (parsed_cmd.type == CommandType::CHAT) {
                // ============================================================
                // TOC FEATURE 2: MESSAGE FILTER (NFA + Regex) - NOW ACTIVE
                // ============================================================
                std::string action_taken;
                std::string filtered_msg = g_message_filter.apply_filters(parsed_cmd.value, action_taken);
                
                std::cout << "[TOC][FILTER] Message: \"" << parsed_cmd.value << "\" -> Action: " << action_taken << std::endl;
                
                // Check if message was blocked
                if (action_taken.find("blocked") != std::string::npos) {
                    res.status = 403;
                    nlohmann::json error_response;
                    error_response["error"] = "Message blocked by filter";
                    error_response["reason"] = action_taken;
                    res.set_content(error_response.dump(), "application/json");
                    return;
                }
                
                // Use filtered message (may have warning flags added)
                std::string final_msg = (action_taken == "none") ? parsed_cmd.value : filtered_msg;
                
                // Send regular chat message
                chat->send_message(final_msg);
                
                // Manually push GUI event with user_id for DB logging
                nlohmann::json event_json;
                event_json["type"] = "chat";
                event_json["text"] = final_msg;
                event_json["sent"] = true;
                event_json["user_id"] = user_id;
                event_json["channel"] = "chat";
                gui_event_queue.push(event_json.dump());
                
                res.set_content(R"({"status":"ok"})", "application/json");
                return;
            }
        } catch (...) {
            res.status = 400;
            res.set_content(R"({"error":"Invalid JSON"})", "application/json");
        }
    });

    // CORS preflight for /file
    svr.Options("/file", [](const httplib::Request& req, httplib::Response& res){
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
        auto req_headers = req.get_header_value("Access-Control-Request-Headers");
        res.set_header("Access-Control-Allow-Headers", req_headers.empty() ? "content-type" : req_headers.c_str());
        res.status = 204;
    });

    // POST /file
    svr.Post("/file", [file_transfer](const httplib::Request& req, httplib::Response& res){
        try {
            auto j = nlohmann::json::parse(req.body);
            if (!j.contains("path")) { res.status = 400; res.set_header("Access-Control-Allow-Origin", "*"); res.set_header("Access-Control-Allow-Methods", "POST, OPTIONS"); res.set_content(R"({"error":"Missing 'path' field"})", "application/json"); return; }
            std::string path = j["path"].get<std::string>();
            file_transfer->send_file(path);
            res.set_header("Access-Control-Allow-Origin", "*"); res.set_header("Access-Control-Allow-Methods", "POST, OPTIONS"); res.set_content(R"({"status":"ok"})", "application/json");
        } catch (...) { res.status = 400; res.set_header("Access-Control-Allow-Origin", "*"); res.set_header("Access-Control-Allow-Methods", "POST, OPTIONS"); res.set_content(R"({"error":"Invalid JSON"})", "application/json"); }
    });

    // GET /content - fetch user's content sorted by timestamp
    svr.Get("/content", [](const httplib::Request& req, httplib::Response& res){
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("Access-Control-Allow-Methods", "GET, OPTIONS");
        
        std::cout << "[CONTENT] Content endpoint called" << std::endl;
        
        uint64_t user_id = g_current_user_id.load();
        if (user_id == 0) {
            user_id = 1; // Default to user 1
        }
        
        if (user_id == 0) {
            res.status = 401;
            res.set_content(R"({"error":"Unauthorized - please log in"})", "application/json");
            return;
        }
        
        // Use the new get_history() function with specific user_id
        nlohmann::json history_data = get_history(user_id);
        
        nlohmann::json response;
        response["status"] = "success";
        response["content"] = history_data;
        
        std::cout << "[CONTENT] Returning " << history_data.size() << " items" << std::endl;
        res.set_content(response.dump(), "application/json");
    });

    // CORS preflight for /content
    svr.Options("/content", [](const httplib::Request& req, httplib::Response& res){
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("Access-Control-Allow-Methods", "GET, OPTIONS");
        auto req_headers = req.get_header_value("Access-Control-Request-Headers");
        res.set_header("Access-Control-Allow-Headers", req_headers.empty() ? "content-type" : req_headers.c_str());
        res.status = 204;
    });

    // GET /download/:filename
    svr.Get(R"(/download/(.+))", [](const httplib::Request& req, httplib::Response& res){
        res.set_header("Access-Control-Allow-Origin", "*");
        // URL decode
        auto url_decode = [](const std::string& in){ std::string out; out.reserve(in.size()); for (size_t i=0;i<in.size();++i){ if(in[i]=='%' && i+2<in.size()){ char c=(char)strtol(in.substr(i+1,2).c_str(), nullptr, 16); out.push_back(c); i+=2; } else if (in[i]=='+'){ out.push_back(' ');} else out.push_back(in[i]); } return out; };
        std::string filename = url_decode(req.matches[1]);
        // Debug: print attempted download path and CWD
        char cwd_buf[4096] = {0};
        if (::getcwd(cwd_buf, sizeof(cwd_buf)-1)) {
            std::cout << "[DOWNLOAD] CWD=" << cwd_buf << ", requested='" << filename << "'" << std::endl;
        } else {
            std::cout << "[DOWNLOAD] requested='" << filename << "' (cwd unknown)" << std::endl;
        }
        
        // Try original filename first, then with recv_ prefix if not found
        std::ifstream file(filename, std::ios::binary);
        if (!file && filename.find("recv_") != 0) {
            // Try with recv_ prefix (file was received from peer)
            std::string recv_filename = "recv_" + filename;
            std::cout << "[DOWNLOAD] File not found, trying with recv_ prefix: " << recv_filename << std::endl;
            file.open(recv_filename, std::ios::binary);
            if (file) {
                filename = recv_filename; // Update filename for Content-Disposition header
            }
        }
        if (!file) { res.status = 404; res.set_content("File not found", "text/plain"); return; }
        std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        file.close();
        std::string content_type = "application/octet-stream";
        if (filename.find(".txt")!=std::string::npos) content_type = "text/plain";
        else if (filename.find(".json")!=std::string::npos) content_type = "application/json";
        else if (filename.find(".html")!=std::string::npos) content_type = "text/html";
        else if (filename.find(".jpg")!=std::string::npos || filename.find(".jpeg")!=std::string::npos) content_type = "image/jpeg";
        else if (filename.find(".png")!=std::string::npos) content_type = "image/png";
        else if (filename.find(".mp4")!=std::string::npos) content_type = "video/mp4";
        else if (filename.find(".mkv")!=std::string::npos) content_type = "video/x-matroska";
        res.set_header("Content-Disposition", "inline; filename=\"" + filename.substr(filename.find_last_of("/\\") + 1) + "\"");
        res.set_content(content, content_type);
    });

    // CORS preflight for /open
    svr.Options("/open", [](const httplib::Request& req, httplib::Response& res){
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("Access-Control-Allow-Methods", "POST, OPTIONS");
        auto req_headers = req.get_header_value("Access-Control-Request-Headers");
        res.set_header("Access-Control-Allow-Headers", req_headers.empty() ? "content-type" : req_headers.c_str());
        res.status = 204;
    });

    // POST /open { path: "<filename>" }
    svr.Post("/open", [](const httplib::Request& req, httplib::Response& res){
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("Access-Control-Allow-Methods", "POST, OPTIONS");
        try {
            auto j = nlohmann::json::parse(req.body);
            if (!j.contains("path")) {
                res.status = 400;
                res.set_content(R"({"error":"Missing 'path' field"})", "application/json");
                return;
            }
            std::string path = j["path"].get<std::string>();
            // Attempt to open original path first; if not present and not already recv_, try recv_ prefix
            auto file_exists = [](const std::string& p){ std::ifstream f(p, std::ios::binary); return (bool)f; };
            std::string to_open = path;
            if (!file_exists(to_open) && to_open.find("recv_") != 0) {
                std::string alt = std::string("recv_") + to_open;
                if (file_exists(alt)) to_open = alt;
            }
            if (!file_exists(to_open)) {
                res.status = 404;
                res.set_content(R"({"error":"File not found"})", "application/json");
                return;
            }
            // Fork and execlp xdg-open to open with default application, no shell involved
            pid_t pid = fork();
            if (pid == 0) {
                // Child: detach stdio to /dev/null
                int devnull = open("/dev/null", O_RDWR);
                if (devnull >= 0) {
                    dup2(devnull, STDOUT_FILENO);
                    dup2(devnull, STDERR_FILENO);
                }
                // Execute xdg-open
                execlp("xdg-open", "xdg-open", to_open.c_str(), (char*)nullptr);
                _exit(127);
            } else if (pid < 0) {
                res.status = 500;
                res.set_content(R"({"error":"Failed to fork process"})", "application/json");
                return;
            }
            // Parent: don't wait; return immediately
            res.set_content(std::string("{") + "\"status\":\"ok\",\"opened\":\"" + to_open + "\"}", "application/json");
            std::cout << "[OPEN] Opening file: " << to_open << std::endl;
        } catch (...) {
            res.status = 400;
            res.set_content(R"({"error":"Invalid JSON"})", "application/json");
        }
    });

    // GET /events - SSE
    svr.Get("/events", [&running](const httplib::Request& /*req*/, httplib::Response& res){
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("Content-Type", "text/event-stream");
        res.set_header("Cache-Control", "no-cache");
        res.set_header("Connection", "keep-alive");
        size_t start_idx = 0; { std::lock_guard<std::mutex> lk(g_events_mtx); start_idx = g_events.size(); }
        res.set_chunked_content_provider(
            "text/event-stream",
            [&running, idx = start_idx](size_t, httplib::DataSink &sink) mutable {
                std::unique_lock<std::mutex> lk(g_events_mtx);
                if (!g_events_cv.wait_for(lk, std::chrono::seconds(30), [&]{ return g_events.size() > idx || !running.load(); })) {
                    lk.unlock(); const char* hb = ": ping\n\n"; sink.write(hb, 8); return running.load();
                }
                if (!running.load()) return false;
                while (idx < g_events.size()) { std::string ev = g_events[idx++]; lk.unlock(); std::string sse = "data: " + ev + "\n\n"; sink.write(sse.data(), sse.size()); lk.lock(); }
                return running.load();
            },
            [](bool){}
        );
    });

    std::cout << "[REST API] Listening on port " << rest_port << "..." << std::endl;
    svr.listen("0.0.0.0", rest_port);
}

// Thread to read commands from stdin and trigger actions
void stdin_command_thread(LinkUpChat* chat, FileTransfer* file_transfer, std::atomic<bool>& running) {
    std::cout << "\n=================================================\n";
    std::cout << "Command Interface:\n";
    std::cout << "  !<message>   - Send chat message\n";
    std::cout << "  @<filepath>  - Send file\n";
    std::cout << "  %<sql>       - Run MySQL query, save to file, and send\n";
    std::cout << "  quit         - Exit program\n";
    std::cout << "=================================================\n\n";
    std::string line;
    while (running.load() && std::getline(std::cin, line)) {
        if (line.empty()) continue;
        if (line == "quit" || line == "exit") { std::cout << "[STDIN] Exiting...\n"; running.store(false); break; }
        if (line.rfind("!", 0) == 0) { std::string msg = line.substr(1); std::cout << "[STDIN] Sending message: " << msg << std::endl; chat->send_message(msg); }
        else if (line.rfind("%", 0) == 0) {
            std::string sql = line.substr(1);
            // Execute SQL and send file
            auto t = std::time(nullptr);
            std::tm tm; localtime_r(&t, &tm);
            std::ostringstream oss; oss << "%_" << std::put_time(&tm, "%Y%m%d_%H%M%S") << "_output.txt";
            std::string filename = oss.str();
            DB db; std::string err;
            if (!db.connect(g_db_cfg, &err)) { std::cerr << "[STDIN][DB] connect failed: " << err << "\n"; continue; }
            if (!query_to_file(db, sql, filename, &err)) { std::cerr << "[STDIN][DB] query failed: " << err << "\n"; continue; }
            std::cout << "[STDIN][DB] Result saved to: " << filename << "\n";
            file_transfer->send_file(filename);
        }
        else if (line.rfind("@", 0) == 0) { std::string path = line.substr(1); std::cout << "[STDIN] Sending file: " << path << std::endl; file_transfer->send_file(path); }
        else { std::cout << "[STDIN] Unknown command. Use !<msg> for chat or @<path> for file\n"; }
    }
}

// Get history data for current user
nlohmann::json get_history(uint64_t user_id) {
    // If no user_id provided, use global variable as fallback, then default to 1
    if (user_id == 0) {
        user_id = g_current_user_id.load();
        if (user_id == 0) {
            user_id = 1; // Default to user 1
        }
    }
    
    std::cout << "[HISTORY] Getting history for user_id: " << user_id << std::endl;
    
    DB db;
    std::string err;
    if (!db.connect(g_db_cfg, &err)) {
        std::cerr << "[HISTORY] DB connect failed: " << err << std::endl;
        return nlohmann::json::array();
    }
    
    // Use MySQL to build a complete JSON array with all history data
    std::ostringstream query_stream;
    query_stream << "SELECT JSON_ARRAYAGG("
                 << "JSON_MERGE_PATCH("
                 << "  db_get_history(c.content_id, c.content_type),"
                 << "  JSON_OBJECT("
                 << "    'direction', c.direction,"
                 << "    'channel', c.channel,"
                 << "    'username', COALESCE(u.username, 'Unknown'),"
                 << "    'content_created_at', c.created_at"
                 << "  )"
                 << ")) as history_json "
                 << "FROM contents c "
                 << "LEFT JOIN users u ON c.user_id = u.user_id "
                 << "WHERE c.user_id = " << user_id << " "
                 << "ORDER BY c.created_at ASC";
    
    std::string query = query_stream.str();
    std::cout << "[HISTORY] Query: " << query << std::endl;
    
    std::string json_result;
    if (!db.query_scalar(query, json_result, &err)) {
        std::cerr << "[HISTORY] Query failed: " << err << std::endl;
        return nlohmann::json::array();
    }
    
    std::cout << "[HISTORY] Raw result: " << json_result << std::endl;
    
    if (json_result.empty() || json_result == "null") {
        std::cout << "[HISTORY] No data found" << std::endl;
        return nlohmann::json::array();
    }
    
    try {
        nlohmann::json history_data = nlohmann::json::parse(json_result);
        std::cout << "[HISTORY] Returning " << history_data.size() << " items" << std::endl;
        // Log every message sent in history
        for (const auto& item : history_data) {
            std::cout << "[DEBUG][HISTORY] Sending history item: " << item.dump() << std::endl;
        }
        return history_data;
    } catch (const std::exception& e) {
        std::cerr << "[HISTORY] JSON parse error: " << e.what() << std::endl;
        std::cerr << "[HISTORY] Raw result: " << json_result << std::endl;
        return nlohmann::json::array();
    }
}

content_node* create_content_list() {
    DB db;
    std::string err;
    std::cout << "[DEBUG] Starting create_content_list()" << std::endl;
    if (!db.connect(g_db_cfg, &err)) {
        std::cerr << "[create_content_list] DB connect failed: " << err << std::endl;
        return nullptr;
    }
    std::string result;
    if (!db.query_scalar("SELECT db_get_content()", result, &err)) {
        std::cerr << "[create_content_list] db_get_content() query failed: " << err << std::endl;
        return nullptr;
    }
    std::cout << "[DEBUG] db_get_content() result: " << result << std::endl;
    std::vector<std::pair<int, std::string>> content_info;
    if (!result.empty()) {
        std::istringstream iss(result);
        std::string pair;
        while (std::getline(iss, pair, ',')) {
            size_t delim = pair.find('|');
            if (delim != std::string::npos) {
                int id = std::stoi(pair.substr(0, delim));
                std::string type = pair.substr(delim + 1);
                content_info.emplace_back(id, type);
                std::cout << "[DEBUG] Parsed content_id=" << id << ", type=" << type << std::endl;
            }
        }
    }
    std::cout << "[DEBUG] content_info size: " << content_info.size() << std::endl;
    content_node* root = nullptr;
    content_node* tail = nullptr;
    for (const auto& [id, type] : content_info) {
        std::ostringstream oss;
        oss << "SELECT db_get_history(" << id << ", '" << type << "')";
        std::string json_str;
        if (!db.query_scalar(oss.str(), json_str, &err)) {
            std::cerr << "[create_content_list] db_get_history failed for id=" << id << ": " << err << std::endl;
            continue;
        }
        std::cout << "[DEBUG] db_get_history result for id=" << id << ": " << json_str << std::endl;
        try {
            auto j = nlohmann::json::parse(json_str);
            std::string content;
            if (type == "message" || type == "system") {
                content = j.value("text_content", "");
            } else if (type == "file") {
                content = j.value("file_name", "");
            } else {
                content = "";
            }
            std::cout << "[DEBUG] Creating node: content_id=" << id << ", content='" << content << "'" << std::endl;
            content_node* node = new content_node(id, content);
            if (!root) root = node;
            else tail->next = node;
            tail = node;
        } catch (const std::exception& ex) {
            std::cerr << "[create_content_list] JSON parse error for id=" << id << ": " << ex.what() << std::endl;
        }
    }
    if (!root) {
        std::cout << "[DEBUG] No content nodes created (root is nullptr)" << std::endl;
    } else {
        std::cout << "[DEBUG] Linked list created successfully" << std::endl;
    }
    return root;
}


int main(int argc, char** argv) {
    // Parse command-line arguments
    int my_main_port = 7000;
    int peer_main_port = 7000;
    std::string peer_ip = "127.0.0.1";
    std::string role = "m1"; // default
    int rest_api_port = 8080; // default REST API port
    
    if (argc >= 6) {
        my_main_port = std::stoi(argv[1]);
        peer_main_port = std::stoi(argv[2]);
        peer_ip = argv[3];
        role = argv[4];
        rest_api_port = std::stoi(argv[5]);
    } else {
        std::cout << "Usage: ./linkup_main <my_port> <peer_port> <peer_ip> <role:m1|m2> <rest_api_port>\n";
        std::cout << "Example (terminal1): ./linkup_main 7000 8000 127.0.0.1 m1 8080\n";
        std::cout << "Example (terminal2): ./linkup_main 8000 7000 127.0.0.1 m2 8081\n";
        return 1;
    }
    // Select database based on role so each side writes to its own DB
    // m1 -> linkup_db, others (e.g., m2) -> linkup_dbm2
    if (role == "m1") {
        g_db_cfg.database = "linkup_db";
    } else {
        g_db_cfg.database = "linkup_dbm2";
    }
    std::cout << "[DB] Using database '" << g_db_cfg.database << "' for role=" << role << std::endl;
// Use the same database for both roles by default; if you need per-role DBs,
// set LINKUP_DB_NAME env before launching.
    // Test DB connection and basic operations
    std::cout << "[DB_TEST] Testing MySQL connection and operations...\n";
    {
        DB test_db;
        std::string err;
        if (!test_db.connect(g_db_cfg, &err)) {
            std::cerr << "[DB_TEST] ✗ Connect failed: " << err << std::endl;
        } else {
            std::cout << "[DB_TEST] ✓ Connected to database: " << g_db_cfg.database << std::endl;
            
            // Create test table
            std::string create_sql = "CREATE TABLE IF NOT EXISTS linkup_test (id INT AUTO_INCREMENT PRIMARY KEY, name VARCHAR(100), value INT, created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP)";
            std::string result;
            if (!test_db.query_scalar(create_sql, result, &err)) {
                std::cerr << "[DB_TEST] ✗ Create table failed: " << err << std::endl;
            } else {
                std::cout << "[DB_TEST] ✓ Table 'linkup_test' created/verified" << std::endl;
                
                // Insert test data
                std::string insert_sql = "INSERT INTO linkup_test (name, value) VALUES ('test_item_1', 42), ('test_item_2', 100), ('test_item_3', 255)";
                if (!test_db.query_scalar(insert_sql, result, &err)) {
                    std::cerr << "[DB_TEST] ✗ Insert failed: " << err << std::endl;
                } else {
                    std::cout << "[DB_TEST] ✓ Inserted 3 test rows" << std::endl;
                    
                    // Query and display data
                    std::string temp_file = "/tmp/linkup_test_query.txt";
                    std::string select_sql = "SELECT id, name, value, created_at FROM linkup_test ORDER BY id DESC LIMIT 5";
                    if (!query_to_file(test_db, select_sql, temp_file, &err)) {
                        std::cerr << "[DB_TEST] ✗ Select failed: " << err << std::endl;
                    } else {
                        std::cout << "[DB_TEST] ✓ Query successful. Results:" << std::endl;
                        std::cout << "----------------------------------------" << std::endl;
                        std::ifstream result_file(temp_file);
                        std::string line;
                        while (std::getline(result_file, line)) {
                            std::cout << "  " << line << std::endl;
                        }
                        result_file.close();
                        std::cout << "----------------------------------------" << std::endl;
                        std::remove(temp_file.c_str());
                    }
                }
            }
        }
    }
    std::cout << "[DB_TEST] Test complete.\n\n";
    
    std::cout << "[linkup_main] Establishing all channels...\n";
    
    // Call the channel establishment function
    ChannelSet channels = establish_channels(my_main_port, peer_main_port, peer_ip, role);
    
    std::cout << "[linkup_main] All channels ready!\n";
    std::cout << "[linkup_main] Chat send FD: " << channels.chat_send_fd << "\n";
    std::cout << "[linkup_main] Chat recv FD: " << channels.chat_recv_fd << "\n";
    std::cout << "[linkup_main] File send FDs: ";
    for (int i = 0; i < 5; ++i) std::cout << channels.file_send_fds[i] << " ";
    std::cout << "\n";
    std::cout << "[linkup_main] File recv FDs: ";
    for (int i = 0; i < 5; ++i) std::cout << channels.file_recv_fds[i] << " ";
    std::cout << "\n";
    std::cout << "[linkup_main] Audio send FD: " << channels.audio_send_fd << "\n";
    std::cout << "[linkup_main] Audio recv FD: " << channels.audio_recv_fd << "\n";
    
    // Setup modules with GUI event queue
    std::string user_id = "user_" + role;
    std::atomic<bool> running{true};

    // Initialize and start chat module with established channels
    std::cout << "[linkup_main] Starting chat module...\n";
    LinkUpChat chat(user_id, channels.chat_send_fd, channels.chat_recv_fd, &gui_event_queue);
    chat.start();

    // Initialize and start file transfer module
    std::vector<int> file_send_fds(channels.file_send_fds, channels.file_send_fds + 5);
    std::vector<int> file_recv_fds(channels.file_recv_fds, channels.file_recv_fds + 5);
    FileTransfer file_transfer(file_send_fds, file_recv_fds, &gui_event_queue);
    file_transfer.start();

    // GUI thread removed; SSE will be the sole consumer of gui_event_queue and will also print events

    // Start GUI event dispatcher (sole consumer of gui_event_queue)
    std::thread gui_dispatcher_thread(gui_event_dispatcher, std::ref(running));

    // Start REST API server thread
    std::thread rest_thread(rest_api_thread, &chat, &file_transfer, std::ref(running), rest_api_port);

    // Command input disabled except for search sequence
    // std::thread stdin_thread(stdin_command_thread, &chat, &file_transfer, std::ref(running));

    std::cout << "[linkup_main] Chat, file transfer, and REST API ready!\n";
    std::cout << "[linkup_main] REST API endpoints:\n  POST /chat {\"message\":\"...\"}\n  POST /file {\"path\":\"...\"}\n  (port " << rest_api_port << ")\n";
    std::cout << "[linkup_main] Enter commands (type 'quit' to exit):\n";

    // Initialize content linked list
    g_content_list_root = create_content_list();
    test_content(g_content_list_root);

    // Take search sequence from terminal
    std::string seq;
    std::cout << "Enter search sequence: ";
    std::cin >> seq;

    // Run sequence search and print matching content IDs
    content_id_node* results = seq_search_fallback(g_content_list_root, seq);
    std::cout << "Matching content IDs for sequence '" << seq << "':\n";
    display_results(results);

        // Prepare sequence search data structures
        char_node* search_chars_arr[128] = {nullptr};
        create_seq_search_multilist(g_content_list_root, search_chars_arr);


    // Command input disabled except for search sequence
    // stdin_thread.join();

    // Cleanup
    std::cout << "[linkup_main] Shutting down...\n";
    running.store(false);
    gui_event_queue.notify_all();
    // No GUI thread to join
    if (rest_thread.joinable()) rest_thread.join();
    if (gui_dispatcher_thread.joinable()) gui_dispatcher_thread.join();
    chat.stop();
    file_transfer.stop();

    std::cout << "[linkup_main] Goodbye!\n";
    return 0;
}