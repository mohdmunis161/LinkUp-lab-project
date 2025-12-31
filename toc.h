#ifndef TOC_H
#define TOC_H

#include <string>
#include <vector>
#include <map>
#include <set>
#include <stack>
#include <functional>
#include <regex>
#include <cstdint>  // For uint8_t

// ============================================================
// 1. SMART MESSAGE ROUTING ENGINE (DFA)
// ============================================================

enum class CommandType {
    CHAT,           // Regular chat message
    SHORTHAND_CREATE, // sh:key>value
    SHORTHAND_GET,    // >key or >key:vX
    SHORTHAND_DELETE, // <key
    SHELL,          // $command
    SQL,            // %query
    FILE,           // @filepath
    SEARCH,         // >search:sequence
    CONTENT_DELETE, // >del or >del#1,2,3
    NESTED,         // {nested{commands}}
    UNKNOWN
};

struct ParsedCommand {
    CommandType type;
    std::string action;     // "send", "create", "get", "delete", "execute", "query"
    std::string key;        // For shorthands
    std::string value;      // Content/value
    int priority;           // 0=highest (system), 5=lowest (chat)
    bool is_valid;
    std::string error_msg;
};

class SmartCommandRouter {
public:
    SmartCommandRouter();
    
    // Parse command using DFA
    ParsedCommand parse(const std::string& input);
    
    // Validate command syntax
    bool validate(const ParsedCommand& cmd);
    
    // Get command type as string
    static std::string type_to_string(CommandType type);
    
private:
    // DFA states
    enum State {
        START,
        SHORTHAND_CREATE_PREFIX,  // "sh"
        SHORTHAND_CREATE_COLON,   // "sh:"
        SHORTHAND_GET_PREFIX,     // ">"
        SHORTHAND_DELETE_PREFIX,  // "<"
        SHELL_PREFIX,             // "$"
        SQL_PREFIX,               // "%"
        FILE_PREFIX,              // "@"
        MESSAGE,                  // Default chat
        ERROR_STATE
    };
    
    State current_state_;
    
    // DFA transition function
    State transition(State current, char input);
    
    // Parse shorthand creation: sh:key>value
    ParsedCommand parse_shorthand_create(const std::string& input);
    
    // Parse shorthand get: >key or >key:vX or >search:seq or >del or >del#ids
    ParsedCommand parse_shorthand_get(const std::string& input);
    
    // Parse shorthand delete: <key
    ParsedCommand parse_shorthand_delete(const std::string& input);
    
    // Simple SQL validation
    bool validate_sql_syntax(const std::string& sql);
};

// ============================================================
// 2. MESSAGE FILTER (NFA with Regex)
// ============================================================

struct FilterRule {
    std::string name;
    std::string regex_pattern;
    std::string action;  // "block", "flag", "redirect", "allow"
    bool enabled;
    int priority;        // Lower number = higher priority
};

class MessageFilter {
public:
    MessageFilter();
    
    // Add a filter rule
    void add_rule(const std::string& name, const std::string& pattern, 
                  const std::string& action, int priority = 5);
    
    // Enable/disable a rule
    void enable_rule(const std::string& name, bool enable);
    
    // Remove a rule
    void remove_rule(const std::string& name);
    
    // Apply filters to a message
    std::string apply_filters(const std::string& message, std::string& action_taken);
    
    // Get all rules
    std::vector<FilterRule> get_rules() const;
    
    // Clear all rules
    void clear_rules();
    
private:
    std::vector<FilterRule> rules_;
    
    // Check if message matches pattern
    bool matches_pattern(const std::string& text, const std::string& pattern);
    
    // Log filtered message
    void log_filtered(const std::string& msg, const std::string& reason, const std::string& action);
};

// ============================================================
// 3. SECURE SQL PARSER (CFG)
// ============================================================

struct SafeQuery {
    std::string type;  // SELECT, INSERT, UPDATE, DELETE
    std::vector<std::string> columns;
    std::string table;
    std::map<std::string, std::string> where_conditions;
    bool is_safe;
    std::string error_msg;
    std::string original_query;
};

class SecureSQLParser {
public:
    SecureSQLParser();
    
    // Parse and validate SQL query
    SafeQuery parse_and_validate(const std::string& sql);
    
    // Set allowed tables
    void set_allowed_tables(const std::set<std::string>& tables);
    
    // Add allowed table
    void add_allowed_table(const std::string& table);
    
private:
    std::set<std::string> allowed_tables_;
    std::vector<std::string> dangerous_patterns_;
    
    // Tokenize SQL
    std::vector<std::string> tokenize(const std::string& sql);
    
    // Check for SQL injection patterns
    bool validate_no_injection(const std::string& sql);
    
    // Validate table access
    bool validate_allowed_tables(const std::string& table);
    
    // Parse SELECT query
    SafeQuery parse_select(const std::vector<std::string>& tokens);
    
    // Parse INSERT query
    SafeQuery parse_insert(const std::vector<std::string>& tokens);
    
    // Parse UPDATE query
    SafeQuery parse_update(const std::vector<std::string>& tokens);
    
    // Parse DELETE query
    SafeQuery parse_delete(const std::vector<std::string>& tokens);
};

// ============================================================
// 4. NESTED COMMAND VALIDATOR (PDA)
// ============================================================

struct NestedCommand {
    std::vector<std::string> commands;  // Innermost to outermost
    bool is_valid;
    int nesting_level;
    std::string error_msg;
};

class NestedCommandValidator {
public:
    NestedCommandValidator();
    
    // Parse nested commands with bracket validation
    NestedCommand parse(const std::string& input);
    
    // Execute nested commands from innermost to outermost
    std::string execute_nested(const std::string& input, 
                               std::function<std::string(std::string)> executor);
    
    // Validate balanced brackets
    bool validate_balanced(const std::string& input);
    
private:
    std::stack<char> pda_stack_;
    
    // PDA transition
    bool push_symbol(char bracket);
    bool pop_symbol(char bracket);
};

// ============================================================
// 5. COMMAND SAFETY CHECKER (Halting Problem)
// ============================================================

enum class SafetyLevel {
    SAFE,
    RISKY,
    DANGEROUS,
    INFINITE_LOOP
};

struct SafetyReport {
    SafetyLevel level;
    std::string reason;
    int estimated_timeout_ms;
    bool allow_execution;
    std::vector<std::string> warnings;
};

class CommandSafetyChecker {
public:
    CommandSafetyChecker();
    
    // Analyze command safety
    SafetyReport analyze(const std::string& cmd);
    
    // Check for infinite loop patterns
    bool contains_infinite_loop(const std::string& cmd);
    
    // Check for dangerous commands
    bool is_dangerous_command(const std::string& cmd);
    
    // Check for long-running commands
    bool is_long_running(const std::string& cmd);
    
    // Add custom dangerous pattern
    void add_dangerous_pattern(const std::string& pattern);
    
    // Add custom infinite loop pattern
    void add_infinite_loop_pattern(const std::string& pattern);
    
private:
    std::vector<std::string> infinite_patterns_;
    std::vector<std::string> dangerous_patterns_;
    std::vector<std::string> long_running_patterns_;
};

// ============================================================
// 6. TURING MACHINE FILE ENCRYPTION
// ============================================================

class TMEncryption {
public:
    TMEncryption(const std::string& key);
    
    // Encrypt file data
    std::vector<uint8_t> encrypt_file(const std::vector<uint8_t>& file_data);
    
    // Decrypt file data (symmetric)
    std::vector<uint8_t> decrypt_file(const std::vector<uint8_t>& encrypted_data);
    
    // Encrypt string
    std::string encrypt_string(const std::string& plaintext);
    
    // Decrypt string
    std::string decrypt_string(const std::string& ciphertext);
    
private:
    std::string encryption_key_;
    std::vector<uint8_t> tape_;
    int head_position_;
    int current_state_;
    
    // Build encryption Turing Machine
    void build_encryption_tm();
    
    // Execute TM transition
    void execute_transition();
    
    // XOR-based stream cipher (TM operation)
    uint8_t tm_cipher_operation(uint8_t byte, uint8_t key_byte);
};

#endif // TOC_H

