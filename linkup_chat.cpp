#include "linkup_chat.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <queue>
#include <random>
#include <sstream>
#include <string>
#include <thread>

// Access to global current user ID from linkup_main.cpp
extern std::atomic<uint64_t> g_current_user_id;

namespace net {
static inline bool sendAll(int fd, const void* data, size_t len) {
    const char* buf = static_cast<const char*>(data);
    size_t total = 0;
    while (total < len) {
        ssize_t n = ::send(fd, buf + total, len - total, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) return false;
        total += static_cast<size_t>(n);
    }
    return true;
}
static inline bool recvAll(int fd, void* data, size_t len) {
    char* buf = static_cast<char*>(data);
    size_t total = 0;
    while (total < len) {
        ssize_t n = ::recv(fd, buf + total, len - total, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) return false;
        total += static_cast<size_t>(n);
    }
    return true;
}
static inline bool sendFrame(int fd, const std::string& payload) {
    uint32_t nlen = htonl(static_cast<uint32_t>(payload.size()));
    if (!sendAll(fd, &nlen, sizeof(nlen))) return false;
    if (payload.empty()) return true;
    return sendAll(fd, payload.data(), payload.size());
}
static inline bool recvFrame(int fd, std::string& out) {
    uint32_t nlen = 0;
    if (!recvAll(fd, &nlen, sizeof(nlen))) return false;
    uint32_t len = ntohl(nlen);
    std::string buf;
    buf.resize(len);
    if (len > 0) {
        if (!recvAll(fd, buf.data(), len)) return false;
    }
    out = std::move(buf);
    return true;
}
}

std::string now_hhmmss() {
    using namespace std::chrono;
    auto tp = system_clock::now();
    std::time_t t = system_clock::to_time_t(tp);
    std::tm tm{};
    localtime_r(&t, &tm);
    std::ostringstream oss;
    oss << std::setfill('0') << std::setw(2) << tm.tm_hour << ":" << std::setw(2) << tm.tm_min << ":" << std::setw(2) << tm.tm_sec;
    return oss.str();
}

// LinkUpChat implementation

LinkUpChat::LinkUpChat(std::string user, int send_fd, int recv_fd, BlockingQueue<std::string>* external_gui_queue)
        : user_id_(std::move(user)), outbound_fd_(send_fd), inbound_fd_(recv_fd), 
            owns_sockets_(false), external_gui_queue_(external_gui_queue) {
    std::cout << "[DEBUG] LinkUpChat constructor called. user_id=" << user_id_ << ", send_fd=" << outbound_fd_ << ", recv_fd=" << inbound_fd_ << std::endl;
    std::cout << "[DEBUG] external_gui_queue_=" << (external_gui_queue_ ? "valid" : "nullptr") << std::endl;
}

LinkUpChat::~LinkUpChat() { 
    std::cout << "[DEBUG] LinkUpChat destructor called." << std::endl; 
    stop(); 
}

void LinkUpChat::start() {
    std::cout << "[DEBUG] LinkUpChat::start() called." << std::endl;
    if (running_.load()) return;
    running_.store(true);
    inbound_ready_ = true;
    outbound_ready_ = true;
    send_thread_ = std::thread(&LinkUpChat::send_message_thread, this);
    net_recv_thread_ = std::thread(&LinkUpChat::network_recv_thread, this);
    recv_thread_ = std::thread(&LinkUpChat::receive_message_thread, this);
    std::cout << "[CHAT] Chat module started with external sockets." << std::endl;
}

void LinkUpChat::stop() {
    std::cout << "[DEBUG] LinkUpChat::stop() called." << std::endl;
    bool expected = true;
    if (!running_.compare_exchange_strong(expected, false)) return;
    send_queue_.notify_all();
    recv_queue_.notify_all();
    if (send_thread_.joinable()) send_thread_.join();
    if (net_recv_thread_.joinable()) net_recv_thread_.join();
    if (recv_thread_.joinable()) recv_thread_.join();
    // Don't close sockets if we don't own them
    if (owns_sockets_) {
        close_safely(inbound_fd_);
        close_safely(outbound_fd_);
    }
    std::cout << "[CHAT] Chat module stopped." << std::endl;
}

void LinkUpChat::send_message(const std::string& text) {
    Message m;
    m.id = gen_id();
    m.sender_id = user_id_;
    m.text = text;
    m.timestamp = std::chrono::system_clock::now();
    m.status = MessageStatus::TYPED;
    send_queue_.push(std::move(m.text)); // Only push the text to the queue
    // NOTE: GUI event with user_id is now pushed from REST endpoint in linkup_main.cpp
}

void LinkUpChat::send_message_thread() {
    while (running_.load()) {
        std::string text;
        if (!send_queue_.wait_pop(text, running_)) break;
        Message gui_msg;
        gui_msg.text = text;
        gui_msg.sender_id = user_id_;
        gui_msg.timestamp = std::chrono::system_clock::now();
        gui_msg.status = MessageStatus::SENT;
        int fd = outbound_fd_;
        if (fd == -1) {
            gui_msg.status = MessageStatus::TYPED;
            continue;
        }
        std::string payload = user_id_ + "\n" + text;
        bool ok = net::sendFrame(fd, payload);
        if (!ok) {
            std::cerr << "[CHAT] Send failed.\n";
            gui_msg.status = MessageStatus::TYPED;
        } else {
            gui_msg.status = MessageStatus::SENT;
            // Send is handled by linkup_main gui_thread after this returns
        }
    }
}

void LinkUpChat::network_recv_thread() {
    while (running_.load()) {
        int fd = inbound_fd_;
        if (fd == -1) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        std::string payload;
        bool ok = net::recvFrame(fd, payload);
        if (!ok) {
            if (running_.load()) {
                std::cerr << "[CHAT] Receive channel closed or error.\n";
            }
            break;
        }
        auto pos = payload.find('\n');
        std::string sender = (pos == std::string::npos) ? std::string("peer") : payload.substr(0, pos);
        std::string text = (pos == std::string::npos) ? payload : payload.substr(pos + 1);
        recv_queue_.push(std::move(text));
    }
}

std::string LinkUpChat::json_escape(const std::string& s) {
    std::string result;
    for (char c : s) {
        if (c == '"' || c == '\\') result += '\\';
        result += c;
    }
    return result;
}

void LinkUpChat::receive_message_thread() {
    while (running_.load()) {
        std::string text;
        if (!recv_queue_.wait_pop(text, running_)) break;
        
        // Print received message to terminal
        std::cout << "\n[CHAT RECEIVED] " << text << std::endl;
        std::cout << "> " << std::flush;  // Re-prompt for input
        
        // Push to GUI queue with current user_id
        if (external_gui_queue_) {
            uint64_t current_user = g_current_user_id.load();
            std::ostringstream oss;
            oss << "{\"type\":\"chat\",\"text\":\"" << json_escape(text) 
                << "\",\"sent\":false,\"user_id\":" << current_user << "}";
            external_gui_queue_->push(oss.str());
        }
    }
}

void LinkUpChat::close_safely(int& fd) {
    if (fd != -1) { ::close(fd); fd = -1; }
}

std::string LinkUpChat::gen_id() {
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    auto now = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    std::uniform_int_distribution<uint64_t> dist;
    std::ostringstream oss; oss << user_id_ << "_" << now << "_" << std::hex << dist(rng);
    return oss.str();
}



