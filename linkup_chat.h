#ifndef LINKUP_CHAT_H
#define LINKUP_CHAT_H

#include "blocking_queue.h"
#include <string>
#include <thread>
#include <atomic>
#include <chrono>

enum class MessageStatus { TYPED = 0, SENT = 1, DELIVERED = 2, READ = 3 };

struct Message {
    std::string id;
    std::string sender_id;
    std::string text;
    std::chrono::time_point<std::chrono::system_clock> timestamp;
    MessageStatus status{MessageStatus::TYPED};
};

class LinkUpChat {
public:
    // Constructor takes pre-established socket FDs and external GUI queue
    explicit LinkUpChat(std::string user, int send_fd, int recv_fd, BlockingQueue<std::string>* external_gui_queue);
    ~LinkUpChat();

    void start();
    void stop();
    void send_message(const std::string& text);

private:
    void send_message_thread();
    void network_recv_thread();
    void receive_message_thread();
    std::string gen_id();
    void close_safely(int& fd);
    std::string json_escape(const std::string& s);

    std::string user_id_;
    int outbound_fd_;
    int inbound_fd_;
    bool owns_sockets_;
    bool inbound_ready_;
    bool outbound_ready_;
    BlockingQueue<std::string>* external_gui_queue_;
    
    std::atomic<bool> running_{false};
    
    BlockingQueue<std::string> send_queue_;
    BlockingQueue<std::string> recv_queue_;
    
    std::thread send_thread_;
    std::thread net_recv_thread_;
    std::thread recv_thread_;
};

#endif // LINKUP_CHAT_H
