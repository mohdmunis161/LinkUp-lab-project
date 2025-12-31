#include "linkup_channels.h"
#include <iostream>
#include <fstream>
#include <thread>
#include <vector>
#include <string>
#include <atomic>
#include <mutex>
#include <sstream>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

// Utility: find a free port by binding to port 0
int find_free_port() {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = 0;
    bind(fd, (sockaddr*)&addr, sizeof(addr));
    socklen_t len = sizeof(addr);
    getsockname(fd, (sockaddr*)&addr, &len);
    int port = ntohs(addr.sin_port);
    close(fd);
    return port;
}

struct HandshakePair { int out_fd{-1}; int in_fd{-1}; };

// Directed handshake using two sockets: send on out_fd, recv on in_fd to avoid deadlock
HandshakePair setup_handshake_pair(const std::string& role, int my_port, const std::string& peer_ip, int peer_port) {
    HandshakePair hp;
    if (role == "m1") {
        // m1: connect out first, then accept inbound
        std::cout << "[flow] role=m1: connecting to peer " << peer_ip << ":" << peer_port << "...\n";
        while (true) {
            int fd = ::socket(AF_INET, SOCK_STREAM, 0);
            sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_port = htons(peer_port); inet_pton(AF_INET, peer_ip.c_str(), &addr.sin_addr);
            if (::connect(fd, (sockaddr*)&addr, sizeof(addr)) == 0) { hp.out_fd = fd; break; }
            close(fd); std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        std::cout << "[flow] role=m1: outbound handshake established fd=" << hp.out_fd << "\n";
        int listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
        int opt = 1; setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = INADDR_ANY; a.sin_port = htons(my_port);
        bind(listen_fd, (sockaddr*)&a, sizeof(a)); listen(listen_fd,1);
        hp.in_fd = accept(listen_fd, nullptr, nullptr);
        close(listen_fd);
        std::cout << "[flow] role=m1: inbound handshake accepted fd=" << hp.in_fd << "\n";
    } else {
        // m2: accept inbound first, then connect out
        std::cout << "[flow] role=m2: listening for inbound handshake on port " << my_port << "...\n";
        int listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
        int opt = 1; setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = INADDR_ANY; a.sin_port = htons(my_port);
        bind(listen_fd, (sockaddr*)&a, sizeof(a)); listen(listen_fd,1);
        hp.in_fd = accept(listen_fd, nullptr, nullptr);
        close(listen_fd);
        std::cout << "[flow] role=m2: inbound handshake accepted fd=" << hp.in_fd << "\n";
        while (true) {
            int fd = ::socket(AF_INET, SOCK_STREAM, 0);
            sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_port = htons(peer_port); inet_pton(AF_INET, peer_ip.c_str(), &addr.sin_addr);
            if (::connect(fd, (sockaddr*)&addr, sizeof(addr)) == 0) { hp.out_fd = fd; break; }
            close(fd); std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        std::cout << "[flow] role=m2: outbound handshake established fd=" << hp.out_fd << "\n";
    }
    // Cross-direction READY to avoid both sides blocking on same fd
    send(hp.out_fd, "READY", 5, 0);
    char buf[8] = {};
    recv(hp.in_fd, buf, 5, 0);
    std::cout << "[flow] Handshake pair ready (out_fd=" << hp.out_fd << ", in_fd=" << hp.in_fd << ")\n";
    return hp;
}

// Exchange port numbers over handshake channel
void exchange_ports(int out_fd, int in_fd, std::vector<int>& my_ports, std::vector<int>& peer_ports) {
    std::ostringstream oss; for (int p : my_ports) oss << p << ","; std::string port_packet = oss.str();
    send(out_fd, port_packet.data(), port_packet.size(), 0);
    char buf[128] = {}; int n = recv(in_fd, buf, sizeof(buf)-1, 0);
    std::string peer_ports_str(buf, n);
    std::istringstream iss(peer_ports_str);
    std::string tok;
    while (std::getline(iss, tok, ',')) {
        if (!tok.empty()) peer_ports.push_back(std::stoi(tok));
    }
    std::cout << "[flow] Exchanged port numbers.\n";
}

// Main function to establish all channels
ChannelSet establish_channels(int my_main_port, int peer_main_port, const std::string& peer_ip, const std::string& role) {
    std::cout << "[flow] Starting channel setup...\n";
    HandshakePair hp = setup_handshake_pair(role, my_main_port, peer_ip, peer_main_port);
    // Step 2: Find 7 free ports and launch listening threads
    std::vector<int> my_ports;
    std::vector<std::thread> listen_threads;
    std::vector<int> recv_fds(7, -1);
    std::mutex recv_mutex;
    for (int i = 0; i < 7; ++i) {
        int port = find_free_port();
        my_ports.push_back(port);
        listen_threads.emplace_back([i, port, &recv_fds, &recv_mutex]{
            int listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
            int opt = 1;
            setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = INADDR_ANY;
            addr.sin_port = htons(port);
            bind(listen_fd, (sockaddr*)&addr, sizeof(addr));
            listen(listen_fd, 1);
            int fd = accept(listen_fd, nullptr, nullptr);
            {
                std::lock_guard<std::mutex> lk(recv_mutex);
                recv_fds[i] = fd;
            }
            close(listen_fd);
            std::cout << "[flow] Listening thread accepted connection on port " << port << "\n";
        });
    }
    // Step 2: Exchange port numbers
    std::vector<int> peer_ports;
    exchange_ports(hp.out_fd, hp.in_fd, my_ports, peer_ports);
    // Step 3: Launch outgoing connection threads
    std::vector<int> send_fds(7, -1);
    std::vector<std::thread> connect_threads;
    for (int i = 0; i < 7; ++i) {
        connect_threads.emplace_back([i, &send_fds, &peer_ports, &peer_ip]{
            int fd = -1;
            while (fd == -1) {
                int sock = ::socket(AF_INET, SOCK_STREAM, 0);
                sockaddr_in addr{};
                addr.sin_family = AF_INET;
                addr.sin_port = htons(peer_ports[i]);
                inet_pton(AF_INET, peer_ip.c_str(), &addr.sin_addr);
                if (::connect(sock, (sockaddr*)&addr, sizeof(addr)) == 0) {
                    fd = sock;
                } else {
                    close(sock);
                    std::this_thread::sleep_for(std::chrono::milliseconds(200));
                }
            }
            send_fds[i] = fd;
            std::cout << "[flow] Outgoing connection established to peer port " << peer_ports[i] << "\n";
        });
    }
    // Wait for all threads
    for (auto& t : listen_threads) t.join();
    for (auto& t : connect_threads) t.join();
    std::cout << "[flow] All 14 channels established!\n";
    // Assign purposes
    ChannelSet channels;
    channels.chat_send_fd = send_fds[0];
    channels.chat_recv_fd = recv_fds[0];
    for (int i = 0; i < 5; ++i) {
        channels.file_send_fds[i] = send_fds[i+1];
        channels.file_recv_fds[i] = recv_fds[i+1];
    }
    channels.audio_send_fd = send_fds[6];
    channels.audio_recv_fd = recv_fds[6];
    std::cout << "[flow] Channel purposes:\n";
    std::cout << "  chat_send_fd: " << channels.chat_send_fd << "\n";
    std::cout << "  chat_recv_fd: " << channels.chat_recv_fd << "\n";
    for (int i = 0; i < 5; ++i) {
        std::cout << "  file_send_fd[" << i << "]: " << channels.file_send_fds[i] << "\n";
        std::cout << "  file_recv_fd[" << i << "]: " << channels.file_recv_fds[i] << "\n";
    }
    std::cout << "  audio_send_fd: " << channels.audio_send_fd << "\n";
    std::cout << "  audio_recv_fd: " << channels.audio_recv_fd << "\n";
    std::cout << "[flow] All channels are ready for use!\n";
    
    // Write channels to JSON file for other programs to consume
    std::ofstream json_out("channels.json");
    json_out << "{\n";
    json_out << "  \"chat_send_fd\": " << channels.chat_send_fd << ",\n";
    json_out << "  \"chat_recv_fd\": " << channels.chat_recv_fd << ",\n";
    json_out << "  \"file_send_fds\": [";
    for (int i = 0; i < 5; ++i) {
        json_out << channels.file_send_fds[i];
        if (i < 4) json_out << ", ";
    }
    json_out << "],\n";
    json_out << "  \"file_recv_fds\": [";
    for (int i = 0; i < 5; ++i) {
        json_out << channels.file_recv_fds[i];
        if (i < 4) json_out << ", ";
    }
    json_out << "],\n";
    json_out << "  \"audio_send_fd\": " << channels.audio_send_fd << ",\n";
    json_out << "  \"audio_recv_fd\": " << channels.audio_recv_fd << "\n";
    json_out << "}\n";
    json_out.close();
    std::cout << "[flow] Channel FDs written to channels.json\n";
    
    close(hp.out_fd);
    close(hp.in_fd);
    
    return channels;
}
