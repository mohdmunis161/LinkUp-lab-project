// All necessary includes
#include "linkup_file_transfer.h"
#include "blocking_queue.h"
#include <fstream>
#include <iostream>
#include <algorithm>
#include <cstring>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iomanip>
#include <map>
#include <mutex>
#include <optional>
#include <queue>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <cstdint>
#include <ctime>

// Access to global current user ID from linkup_main.cpp
extern std::atomic<uint64_t> g_current_user_id;

// Helper: get current time as HH:MM:SS
static std::string now_hms() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_r(&t, &tm);
    char buf[16];
    std::strftime(buf, sizeof(buf), "%H:%M:%S", &tm);
    return std::string(buf);
}

// Helper: get file size
static bool file_stat(const std::string& path, uint64_t& size) {
    struct stat st{};
    if (stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
        size = st.st_size;
        return true;
    }
    return false;
}

namespace net {
static inline bool sendAll(int fd, const void* data, size_t len) {
    const char* p = static_cast<const char*>(data);
    size_t off = 0;
    while (off < len) {
        ssize_t n = ::send(fd, p + off, len - off, 0);
        if (n < 0) { if (errno == EINTR) continue; return false; }
        if (n == 0) return false;
        off += static_cast<size_t>(n);
    }
    return true;
}
static inline bool recvAll(int fd, void* data, size_t len) {
    char* p = static_cast<char*>(data);
    size_t off = 0;
    while (off < len) {
        ssize_t n = ::recv(fd, p + off, len - off, 0);
        if (n < 0) { if (errno == EINTR) continue; return false; }
        if (n == 0) return false;
        off += static_cast<size_t>(n);
    }
    return true;
}
static inline bool sendFrame(int fd, const std::string& payload) {
    uint32_t nlen = htonl(static_cast<uint32_t>(payload.size()));
    return sendAll(fd, &nlen, sizeof(nlen)) && (payload.empty() || sendAll(fd, payload.data(), payload.size()));
}
static inline bool recvFrame(int fd, std::string& out) {
    uint32_t nlen = 0; if (!recvAll(fd, &nlen, sizeof(nlen))) return false;
    uint32_t len = ntohl(nlen);
    std::string buf; buf.resize(len);
    if (len && !recvAll(fd, buf.data(), len)) return false;
    out = std::move(buf);
    return true;
}
}

struct Config { const char* user_id="user"; const char* role="m1"; int base_port=6000; int peer_port=6000; const char* peer_ip="127.0.0.1"; } CFG;
[[maybe_unused]] static void apply_env(Config& c){ if(const char* v=getenv("LINKUP_USER_ID")) c.user_id=v; if(const char* v=getenv("LINKUP_ROLE")) c.role=v; if(const char* v=getenv("LINKUP_BASE_PORT")) { int p=atoi(v); if(p>0&&p<65536) c.base_port=p; } if(const char* v=getenv("LINKUP_PEER_PORT")) { int p=atoi(v); if(p>0&&p<65536) c.peer_port=p; } if(const char* v=getenv("LINKUP_PEER_IP")) c.peer_ip=v; }

class FileTransferApp {
public:
    explicit FileTransferApp(Config cfg):cfg_(cfg){}
    ~FileTransferApp(){ stop(); }
    void start(){
        if(running_.load()) return;
        running_.store(true);
        
        bool is_m1 = (std::string(cfg_.role) == "m1");
        
        if(is_m1) {
            // m1: Listen first, then connect
            std::cout << "[" << now_hms() << "] Role: m1 (server) - Starting listeners on ports " << cfg_.base_port << "-" << (cfg_.base_port+4) << "..." << std::endl;
            start_listeners();
            std::cout << "[" << now_hms() << "] m1: Waiting 3 seconds for m2 to be ready..." << std::endl;
            std::this_thread::sleep_for(std::chrono::seconds(3));
            std::cout << "[" << now_hms() << "] m1: Now connecting to peer ports " << cfg_.peer_port << "-" << (cfg_.peer_port+4) << "..." << std::endl;
            start_connectors();
        } else {
            // m2: Wait, then connect, then listen
            std::cout << "[" << now_hms() << "] Role: m2 (client) - Waiting 2 seconds for m1 to start listening..." << std::endl;
            std::this_thread::sleep_for(std::chrono::seconds(2));
            std::cout << "[" << now_hms() << "] m2: Connecting to peer ports " << cfg_.peer_port << "-" << (cfg_.peer_port+4) << "..." << std::endl;
            start_connectors();
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            std::cout << "[" << now_hms() << "] m2: Now starting listeners on ports " << cfg_.base_port << "-" << (cfg_.base_port+4) << "..." << std::endl;
            start_listeners();
        }
        
        std::cout << "[" << now_hms() << "] Waiting for all channels to connect..." << std::endl;
        wait_channels();
        
        // Handshake: send and receive READY over control channel BEFORE starting workers
        std::cout << "[" << now_hms() << "] Performing handshake..." << std::endl;
        net::sendFrame(ctrl_out_fd_, "READY");
        std::string peer_ready;
        if (!net::recvFrame(ctrl_in_fd_, peer_ready) || peer_ready != "READY") {
            std::cerr << "[" << now_hms() << "] Handshake failed!" << std::endl;
            return;
        }
        std::cout << "[" << now_hms() << "] All 10 channels connected and confirmed by peer!" << std::endl;
        
        // Now start worker threads after handshake
        start_workers();
    }
    void stop(){ bool exp=true; if(!running_.compare_exchange_strong(exp,false)) return; for(auto& q:send_queues_) q.notify_all(); for(auto& q:recv_queues_) q.notify_all(); recv_fcq_.notify_all(); if(ctrl_accept_thr_.joinable()) ctrl_accept_thr_.join(); for(auto& t:acc_thr_) if(t.joinable()) t.join(); if(ctrl_connect_thr_.joinable()) ctrl_connect_thr_.join(); for(auto& t:conn_thr_) if(t.joinable()) t.join(); for(auto& t:send_thr_) if(t.joinable()) t.join(); for(auto& t:recv_data_thr_) if(t.joinable()) t.join(); if(recv_ctrl_thr_.joinable()) recv_ctrl_thr_.join(); if(collect_thr_.joinable()) collect_thr_.join(); if(reconstruct_thr_.joinable()) reconstruct_thr_.join(); if(progress_thr_.joinable()) progress_thr_.join(); for(int& fd : data_in_fds_) close_fd(fd); for(int& fd : data_out_fds_) close_fd(fd); close_fd(ctrl_in_fd_); close_fd(ctrl_out_fd_); close_fd(ctrl_listen_fd_); for(int& fd:listens_) close_fd(fd); }

    void enqueue_file(const std::string& path){ send_file_queue_.push(path); }

private:
    void start_listeners(){ ctrl_listen_fd_=listen_on(cfg_.base_port+0); listens_.resize(4); for(int i=0;i<4;++i) listens_[i]=listen_on(cfg_.base_port+1+i); ctrl_accept_thr_=std::thread([&]{ ctrl_in_fd_=accept_one(ctrl_listen_fd_); }); for(int i=0;i<4;++i) acc_thr_.emplace_back([this,i]{ data_in_fds_[i]=accept_one(listens_[i]); }); }
    void start_connectors(){ ctrl_connect_thr_=std::thread([&]{ ctrl_out_fd_=connect_to(cfg_.peer_ip,cfg_.peer_port+0); }); for(int i=0;i<4;++i) conn_thr_.emplace_back([this,i]{ data_out_fds_[i]=connect_to(cfg_.peer_ip,cfg_.peer_port+1+i); }); }
    void wait_channels(){ std::unique_lock<std::mutex> lk(ch_m_); ch_cv_.wait_for(lk,std::chrono::seconds(30),[&]{ return ctrl_ready_in()&&ctrl_ready_out()&&data_ready_in()&&data_ready_out(); }); }
    void start_workers(){
        recv_ctrl_thr_ = std::thread(&FileTransferApp::recv_control_loop, this);
        for(int i = 0; i < 4; ++i) send_thr_.emplace_back(&FileTransferApp::send_data_loop, this, i);
        // Only start recv_data threads for channels 1-4 (skip 0)
        for(int i = 1; i < 5; ++i) recv_data_thr_.emplace_back(&FileTransferApp::recv_data_loop, this, i);
        collect_thr_ = std::thread(&FileTransferApp::collect_loop, this);
        reconstruct_thr_ = std::thread(&FileTransferApp::reconstruct_loop, this);
        orchestrator_thr_ = std::thread(&FileTransferApp::send_orchestrator_loop, this);
        progress_thr_ = std::thread(&FileTransferApp::progress_loop, this);
    }

    int listen_on(int port){ int fd=::socket(AF_INET,SOCK_STREAM,0); if(fd<0){ perror("socket"); return -1;} int opt=1; setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&opt,sizeof(opt)); sockaddr_in a{}; a.sin_family=AF_INET; a.sin_addr.s_addr=INADDR_ANY; a.sin_port=htons((uint16_t)port); if(bind(fd,(sockaddr*)&a,sizeof(a))<0){ perror("bind"); close_fd(fd); return -1;} if(listen(fd,1)<0){ perror("listen"); close_fd(fd); return -1;} return fd; }
    int accept_one(int lfd){ if(lfd<0) return -1; sockaddr_in c{}; socklen_t cl=sizeof(c); int fd=::accept(lfd,(sockaddr*)&c,&cl); if(fd<0){ perror("accept"); return -1;} { std::lock_guard<std::mutex> lk(ch_m_); } ch_cv_.notify_all(); return fd; }
    int connect_to(const std::string& ip,int port){ while(running_.load()) { int fd=::socket(AF_INET,SOCK_STREAM,0); if(fd<0){ perror("socket"); std::this_thread::sleep_for(std::chrono::milliseconds(200)); continue;} sockaddr_in a{}; a.sin_family=AF_INET; a.sin_port=htons((uint16_t)port); if(::inet_pton(AF_INET,ip.c_str(),&a.sin_addr)!=1){ std::cerr<<"bad ip\n"; close_fd(fd); break;} if(::connect(fd,(sockaddr*)&a,sizeof(a))==0){ { std::lock_guard<std::mutex> lk(ch_m_); } ch_cv_.notify_all(); return fd; } close_fd(fd); std::this_thread::sleep_for(std::chrono::milliseconds(200)); } return -1; }
    void close_fd(int& fd){ if(fd!=-1){ ::close(fd); fd=-1; } }

    bool ctrl_ready_in() const { return ctrl_in_fd_!=-1; }
    bool ctrl_ready_out() const { return ctrl_out_fd_!=-1; }
    bool data_ready_in() const { for(int i=0;i<4;++i) if(data_in_fds_[i]==-1) return false; return true; }
    bool data_ready_out() const { for(int i=0;i<4;++i) if(data_out_fds_[i]==-1) return false; return true; }

    std::string serialize_fc(const FileControl& fc){ std::ostringstream oss; oss<<fc.file_name<<"|"<<fc.file_size<<"|"<<fc.chunk_size<<"|"<<fc.chunks_count; return oss.str(); }
    bool deserialize_fc(const std::string& s, FileControl& fc){ std::istringstream iss(s); std::string tok; if(!std::getline(iss,fc.file_name,'|'))return false; if(!std::getline(iss,tok,'|'))return false; fc.file_size=std::strtoull(tok.c_str(),nullptr,10); if(!std::getline(iss,tok,'|'))return false; fc.chunk_size=(uint32_t)std::strtoul(tok.c_str(),nullptr,10); if(!std::getline(iss,tok,'|'))return false; fc.chunks_count=(uint32_t)std::strtoul(tok.c_str(),nullptr,10); return true; }

    std::string serialize_chunk_header(const Chunk& c){ std::ostringstream oss; oss<<c.seq_number<<","<<c.total_chunks<<","<<c.data_size; return oss.str(); }
    bool parse_chunk_header(const std::string& s, Chunk& c){ std::istringstream iss(s); std::string tok; if(!std::getline(iss,tok,','))return false; c.seq_number=(uint32_t)std::strtoul(tok.c_str(),nullptr,10); if(!std::getline(iss,tok,','))return false; c.total_chunks=(uint32_t)std::strtoul(tok.c_str(),nullptr,10); if(!std::getline(iss,tok,','))return false; c.data_size=(uint32_t)std::strtoul(tok.c_str(),nullptr,10); return true; }

    void send_orchestrator_loop(){
        while(running_.load()) {
            std::string path;
            std::cout << "[DEBUG] Orchestrator: Waiting for file path...\n";
            if(!send_file_queue_wait_pop(path)) {
                std::cout << "[DEBUG] Orchestrator: No file path, exiting loop.\n";
                break;
            }
            std::cout << "[DEBUG] Orchestrator: Got file path: " << path << "\n";
            uint64_t fsize=0;
            if(!file_stat(path,fsize)){
                std::cerr << "[DEBUG] Orchestrator: Bad file: " << path << "\n";
                continue;
            }
            std::cout << "[DEBUG] Orchestrator: File size: " << fsize << "\n";
            const uint32_t chunk_size=65536;
            uint32_t chunks=(uint32_t)((fsize + chunk_size - 1)/chunk_size);
            std::cout << "[DEBUG] Orchestrator: Chunks: " << chunks << "\n";
            FileControl fc{basename_only(path), fsize, chunk_size, chunks};
            std::cout << "[DEBUG] Orchestrator: Sending FileControl over control channel...\n";
            net::sendFrame(ctrl_out_fd_, serialize_fc(fc));
            std::ifstream in(path, std::ios::binary);
            if(!in){
                std::cerr << "[DEBUG] Orchestrator: Open fail: " << path << "\n";
                continue;
            }
            sent_bytes_.store(0);
            current_send_total_.store(fsize);
            std::cout << "[" << now_hms() << "] Sending: " << fc.file_name << " (" << fsize << " bytes)" << std::endl;
            for(uint32_t seq=0; seq<chunks && running_.load(); ++seq){
                std::vector<uint8_t> buf; buf.resize(chunk_size);
                in.read(reinterpret_cast<char*>(buf.data()), chunk_size);
                std::streamsize got = in.gcount();
                buf.resize((size_t)got);
                Chunk c; c.seq_number=seq; c.total_chunks=chunks; c.data_size=(uint32_t)got; c.data=std::move(buf);
                int idx = (seq % 4);
                std::cout << "[DEBUG] Orchestrator: Pushing chunk seq=" << seq << " size=" << got << " to send_queues_[" << idx << "]\n";
                send_queues_[idx].push(std::move(c));
            }
            std::cout << "[DEBUG] Orchestrator: All chunks pushed. Waiting for send queues to empty...\n";
            while(running_.load() && !all_send_queues_empty()) std::this_thread::sleep_for(std::chrono::milliseconds(50));
            std::cout << "[DEBUG] Orchestrator: Send queues empty. Waiting 1 second before next file.\n";
            std::this_thread::sleep_for(std::chrono::seconds(1));
        } }

    bool send_file_queue_wait_pop(std::string& out){ std::unique_lock<std::mutex> lk(sfq_m_); sfq_cv_.wait(lk,[&]{return !send_file_queue_.empty()||!running_.load();}); if(!running_.load()&&send_file_queue_.empty()) return false; out=send_file_queue_.front(); send_file_queue_.pop(); return true; }
    void enqueue_path(const std::string& p){ { std::lock_guard<std::mutex> lk(sfq_m_); send_file_queue_.push(p);} sfq_cv_.notify_one(); }
    bool all_send_queues_empty(){ for(auto& q:send_queues_) if(q.size()!=0) return false; return true; }

    void send_data_loop(int idx){ int fd = data_out_fds_[idx]; while(running_.load()) { Chunk c; if(!send_queues_[idx].wait_pop(c, running_)) break; std::string hdr = serialize_chunk_header(c); if(!net::sendFrame(fd, hdr)) break; if(c.data_size){ if(!net::sendAll(fd, c.data.data(), c.data_size)) break; } sent_bytes_.fetch_add(c.data_size); } }

    void recv_control_loop(){
        while(running_.load()) {
            std::string s;
            std::cout << "[DEBUG] Control: Waiting for FileControl frame...\n";
            if(!net::recvFrame(ctrl_in_fd_, s)) {
                std::cout << "[DEBUG] Control: recvFrame failed, exiting loop.\n";
                break;
            }
            std::cout << "[DEBUG] Control: Got FileControl frame: " << s << "\n";
            FileControl fc;
            if(!deserialize_fc(s, fc)) {
                std::cout << "[DEBUG] Control: Failed to deserialize FileControl.\n";
                continue;
            }
            std::cout << "[DEBUG] Control: Deserialized FileControl: " << fc.file_name << " size=" << fc.file_size << " chunks=" << fc.chunks_count << "\n";
            recv_fcq_.push(std::move(fc));
            received_bytes_.store(0);
            current_recv_total_.store(fc.file_size);
            std::cout << "[" << now_hms() << "] Receiving: " << fc.file_name << " (" << fc.file_size << " bytes)" << std::endl;
        }
    }

    void recv_data_loop(int idx){ int fd=data_in_fds_[idx]; while(running_.load()) { std::string hdr; if(!net::recvFrame(fd, hdr)) break; Chunk c; if(!parse_chunk_header(hdr, c)) break; c.data.resize(c.data_size); if(c.data_size && !net::recvAll(fd, c.data.data(), c.data_size)) break; recv_queues_[idx].push(std::move(c)); received_bytes_.fetch_add(c.data_size); } }

    void collect_loop(){ while(running_.load()) { bool had=false; for(int i=0;i<4;++i){ Chunk c; if(recv_queues_[i].try_pop(c)) { recv_merge_q_.push(std::move(c)); had=true; } } if(!had) std::this_thread::sleep_for(std::chrono::milliseconds(10)); } }

    void reconstruct_loop(){
        while(running_.load()) {
            FileControl fc;
            std::cout << "[DEBUG] Reconstruct: Waiting for FileControl from queue...\n";
            if(!recv_fcq_.wait_pop(fc, running_)) {
                std::cout << "[DEBUG] Reconstruct: No FileControl, exiting loop.\n";
                break;
            }
            std::cout << "[DEBUG] Reconstruct: Got FileControl: " << fc.file_name << " size=" << fc.file_size << " chunks=" << fc.chunks_count << "\n";
            std::vector<Chunk> chunks;
            chunks.reserve(fc.chunks_count);
            while(running_.load() && chunks.size() < fc.chunks_count){
                Chunk c;
                if(recv_merge_q_.wait_pop(c, running_)) {
                    std::cout << "[DEBUG] Reconstruct: Got chunk seq=" << c.seq_number << " size=" << c.data_size << "\n";
                    chunks.push_back(std::move(c));
                }
            }
            std::cout << "[DEBUG] Reconstruct: All chunks received. Sorting...\n";
            std::sort(chunks.begin(), chunks.end(), [](const Chunk&a,const Chunk&b){return a.seq_number<b.seq_number;});
            std::string out_path = std::string("recv_") + fc.file_name;
            std::ofstream out(out_path, std::ios::binary);
            for(const auto& c: chunks){
                if(!c.data.empty()) {
                    out.write(reinterpret_cast<const char*>(c.data.data()), c.data.size());
                }
            }
            out.close();
            std::cout << "[" << now_hms() << "] received file saved: " << out_path << " (" << fc.file_size << " bytes)\n";
        }
    }

    static std::string basename_only(const std::string& p){ auto pos = p.find_last_of("/\\"); return (pos==std::string::npos)?p:p.substr(pos+1); }

    void progress_loop(){
        while(running_.load()){
            std::this_thread::sleep_for(std::chrono::seconds(1));
            uint64_t sent = sent_bytes_.load();
            uint64_t send_total = current_send_total_.load();
            uint64_t recv = received_bytes_.load();
            uint64_t recv_total = current_recv_total_.load();
            
            if(send_total > 0 && sent < send_total){
                double pct = (sent * 100.0) / send_total;
                double mb_sent = sent / (1024.0 * 1024.0);
                double mb_total = send_total / (1024.0 * 1024.0);
                std::cout << "\r[SEND] Progress: " << std::fixed << std::setprecision(1) << pct << "% (" << mb_sent << "/" << mb_total << " MB)" << std::flush;
            }
            
            if(recv_total > 0 && recv < recv_total){
                double pct = (recv * 100.0) / recv_total;
                double mb_recv = recv / (1024.0 * 1024.0);
                double mb_total = recv_total / (1024.0 * 1024.0);
                std::cout << "\r[RECV] Progress: " << std::fixed << std::setprecision(1) << pct << "% (" << mb_recv << "/" << mb_total << " MB)" << std::flush;
            }
        }
    }

private:
    Config cfg_;
    std::atomic<bool> running_{false};

    int ctrl_listen_fd_{-1};
    int ctrl_in_fd_{-1};
    int ctrl_out_fd_{-1};
    int data_in_fds_[4]{-1,-1,-1,-1};
    int data_out_fds_[4]{-1,-1,-1,-1};
    std::vector<int> listens_;

    std::mutex ch_m_;
    std::condition_variable ch_cv_;

    std::thread ctrl_accept_thr_;
    std::vector<std::thread> acc_thr_;
    std::thread ctrl_connect_thr_;
    std::vector<std::thread> conn_thr_;

    std::vector<std::thread> send_thr_;
    std::vector<std::thread> recv_data_thr_;
    std::thread recv_ctrl_thr_;
    std::thread collect_thr_;
    std::thread reconstruct_thr_;
    std::thread orchestrator_thr_;

    BlockingQueue<Chunk> send_queues_[4];
    BlockingQueue<Chunk> recv_queues_[4];
    BlockingQueue<Chunk> recv_merge_q_;
    BlockingQueue<FileControl> recv_fcq_;

    std::mutex sfq_m_;
    std::condition_variable sfq_cv_;
    std::queue<std::string> send_file_queue_;

    std::atomic<uint64_t> sent_bytes_{0};
    std::atomic<uint64_t> received_bytes_{0};
    std::atomic<uint64_t> current_send_total_{0};
    std::atomic<uint64_t> current_recv_total_{0};
    std::thread progress_thr_;
};

// --- FileTransfer class implementation for integration ---
#include "linkup_file_transfer.h"

// Simple blocking queue for chunks
// (already present in file, can be reused)

FileTransfer::FileTransfer(const std::vector<int>& send_fds, const std::vector<int>& recv_fds, BlockingQueue<std::string>* gui_queue)
    : send_fds_(send_fds), recv_fds_(recv_fds), gui_queue_(gui_queue), running_(false) {
    std::cout << "[DEBUG] FileTransfer constructor called." << std::endl;
    std::cout << "[DEBUG] send_fds_.size()=" << send_fds_.size() << ", recv_fds_.size()=" << recv_fds_.size() << std::endl;
    std::cout << "[DEBUG] gui_queue_=" << (gui_queue_ ? "valid" : "nullptr") << std::endl;
    send_queues_.clear();
    recv_queues_.clear();
    for (size_t i = 0; i < send_fds_.size(); ++i) {
        send_queues_.emplace_back(std::make_unique<BlockingQueue<Chunk>>());
    }
    for (size_t i = 0; i < recv_fds_.size(); ++i) {
        recv_queues_.emplace_back(std::make_unique<BlockingQueue<Chunk>>());
    }
    std::cout << "[DEBUG] send_queues_ and recv_queues_ initialized as unique_ptr." << std::endl;
}

FileTransfer::~FileTransfer() { stop(); }

void FileTransfer::start() {
    running_.store(true);
    // Start send threads for all channels (0-4)
    for(size_t i=0; i<send_fds_.size(); ++i) {
        send_threads_.emplace_back([this,i]{ send_thread(i); });
    }
    // Start recv threads for all channels (0-4)
    for(size_t i=0; i<recv_fds_.size(); ++i) {
        recv_threads_.emplace_back([this,i]{ recv_thread(i); });
    }
    collect_thread_ = std::thread([this]{ collect_loop(); });
    reconstruct_thread_ = std::thread([this]{ reconstruct_loop(); });
    if(gui_queue_) gui_queue_->push("[FILE] File transfer ready!");
}

void FileTransfer::stop() {
    running_.store(false);
    for(auto& q:send_queues_) q->notify_all();
    for(auto& q:recv_queues_) q->notify_all();
    recv_merge_q_.notify_all();
    recv_fcq_.notify_all();
    for(auto& t:send_threads_) if(t.joinable()) t.join();
    for(auto& t:recv_threads_) if(t.joinable()) t.join();
    if(collect_thread_.joinable()) collect_thread_.join();
    if(reconstruct_thread_.joinable()) reconstruct_thread_.join();
}

void FileTransfer::send_file(const std::string& path) {
    std::cout << "[DEBUG] FileTransfer::send_file called with path: " << path << std::endl;
    
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if(!in) { 
        std::cerr << "\n[FILE ERROR] Failed to open file: " << path << std::endl;
        std::cout << "> " << std::flush;
        if(gui_queue_) gui_queue_->push("[FILE] Failed to open: " + path); 
        return; 
    }
    
    uint64_t fsize = in.tellg(); in.seekg(0);
    std::cout << "[DEBUG] File size: " << fsize << std::endl;
    
    // Print to terminal
    std::cout << "\n[FILE SENDING] " << path << " (" << fsize << " bytes)" << std::endl;
    std::cout << "> " << std::flush;
    
    // Push file event as JSON to GUI queue (sent=true for sender)
    if(gui_queue_) {
        uint64_t current_user = g_current_user_id.load();
        std::ostringstream oss;
        oss << "{\"type\":\"file\",\"text\":\"[FILE] Sending: " << path 
            << "\",\"sent\":true,\"user_id\":" << current_user << "}";
        gui_queue_->push(oss.str());
    }
    
    const uint32_t chunk_size = 65536;
    uint32_t chunks = (uint32_t)((fsize + chunk_size - 1)/chunk_size);
    std::cout << "[DEBUG] Total chunks: " << chunks << std::endl;
    FileControl fc{basename(path), fsize, chunk_size, chunks};
    std::string fc_str = serialize_fc(fc);
    std::cout << "[DEBUG] Sending FileControl over channel 0: " << fc_str << std::endl;
    net::sendFrame(send_fds_[0], fc_str); // Send control on first channel
    
    for(uint32_t seq=0; seq<chunks && running_.load(); ++seq){
        std::vector<uint8_t> buf(chunk_size);
        in.read(reinterpret_cast<char*>(buf.data()), chunk_size);
        std::streamsize got = in.gcount();
        buf.resize((size_t)got);
        Chunk c; c.seq_number=seq; c.total_chunks=chunks; c.data_size=(uint32_t)got; c.data=std::move(buf);
        int idx = (seq % send_queues_.size()); // Use all channels 0-4 for chunks
        std::cout << "[DEBUG] Queuing chunk seq=" << seq << " size=" << got << " to send_queues_[" << idx << "]" << std::endl;
        send_queues_[idx]->push(std::move(c));
    }
    std::cout << "[DEBUG] All chunks queued for sending." << std::endl;
}

void FileTransfer::send_thread(size_t idx) {
    int fd = send_fds_[idx];
    std::cout << "[DEBUG] send_thread started for idx=" << idx << " fd=" << fd << std::endl;
    while(running_.load()) {
        Chunk c;
        if(!send_queues_[idx]->wait_pop(c, running_)) break;
        std::string hdr = serialize_chunk_header(c);
        std::cout << "[DEBUG] send_thread: sending chunk seq=" << c.seq_number << " size=" << c.data_size << " on channel " << idx << std::endl;
        if(!net::sendFrame(fd, hdr)) { std::cerr << "[DEBUG] send_thread: sendFrame failed on channel " << idx << std::endl; break; }
        if(c.data_size && !net::sendAll(fd, c.data.data(), c.data_size)) { std::cerr << "[DEBUG] send_thread: sendAll failed on channel " << idx << std::endl; break; }
        // No GUI event for individual chunks
    }
}

void FileTransfer::recv_thread(size_t idx) {
    std::cout << "[DEBUG] recv_thread started for idx=" << idx << " fd=" << recv_fds_[idx] << std::endl;
    int fd = recv_fds_[idx];
    while(running_.load()) {
        std::string hdr;
        if(!net::recvFrame(fd, hdr)) { std::cerr << "[DEBUG] recv_thread: recvFrame failed on channel " << idx << std::endl; break; }
        if(hdr.find('|') != std::string::npos) {
            FileControl fc;
            if(deserialize_fc(hdr, fc)) {
                std::cout << "[DEBUG] recv_thread: Received FileControl: " << fc.file_name << " size=" << fc.file_size << " chunks=" << fc.chunks_count << std::endl;
                recv_fcq_.push(std::move(fc));
                // No GUI event for FileControl reception
            }
            continue;
        }
        Chunk c;
        if(!parse_chunk_header(hdr, c)) { std::cerr << "[DEBUG] recv_thread: parse_chunk_header failed on channel " << idx << std::endl; break; }
        c.data.resize(c.data_size);
        if(c.data_size && !net::recvAll(fd, c.data.data(), c.data_size)) { std::cerr << "[DEBUG] recv_thread: recvAll failed for chunk seq=" << c.seq_number << " on channel " << idx << std::endl; break; }
        std::cout << "[DEBUG] recv_thread: received chunk seq=" << c.seq_number << " size=" << c.data_size << " on channel " << idx << std::endl;
        recv_queues_[idx]->push(std::move(c));
        // No GUI event for individual chunks
    }
}

void FileTransfer::collect_loop() {
    while(running_.load()) {
        bool had=false;
        for(size_t i=0;i<recv_queues_.size();++i){
            Chunk c;
            if(recv_queues_[i]->try_pop(c)) { recv_merge_q_.push(std::move(c)); had=true; }
        }
        if(!had) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

void FileTransfer::reconstruct_loop() {
    while(running_.load()) {
        FileControl fc;
        std::cout << "[DEBUG] reconstruct_loop: Waiting for FileControl..." << std::endl;
        if(!recv_fcq_.wait_pop(fc, running_)) break;
        std::cout << "[DEBUG] reconstruct_loop: Got FileControl: " << fc.file_name << " size=" << fc.file_size << " chunks=" << fc.chunks_count << std::endl;
        std::vector<Chunk> chunks; chunks.reserve(fc.chunks_count);
        while(running_.load() && chunks.size() < fc.chunks_count){
            Chunk c;
            if(recv_merge_q_.wait_pop(c, running_)) {
                std::cout << "[DEBUG] reconstruct_loop: Got chunk seq=" << c.seq_number << " size=" << c.data_size << std::endl;
                chunks.push_back(std::move(c));
            }
        }
        std::cout << "[DEBUG] reconstruct_loop: All chunks received. Sorting..." << std::endl;
        std::sort(chunks.begin(), chunks.end(), [](const Chunk&a,const Chunk&b){return a.seq_number<b.seq_number;});
        std::string out_path = std::string("recv_") + fc.file_name;
        std::ofstream out(out_path, std::ios::binary);
        for(const auto& c: chunks){ if(!c.data.empty()) out.write(reinterpret_cast<const char*>(c.data.data()), c.data.size()); }
        out.close();
        
        // Print to terminal
        std::cout << "\n[FILE RECEIVED] " << fc.file_name << " (" << fc.file_size << " bytes)" << std::endl;
        std::cout << "[FILE RECEIVED] Saved as: " << out_path << std::endl;
        std::cout << "> " << std::flush;  // Re-prompt for input
        
        std::cout << "[DEBUG] reconstruct_loop: File saved as " << out_path << " (" << fc.file_size << " bytes)" << std::endl;
        // Push file event as JSON to GUI queue (sent=false for receiver)
        if(gui_queue_) {
            uint64_t current_user = g_current_user_id.load();
            std::ostringstream oss;
            oss << "{\"type\":\"file\",\"text\":\"[FILE] Received: " << out_path 
                << "\",\"sent\":false,\"user_id\":" << current_user << "}";
            gui_queue_->push(oss.str());
        }
    }
}

std::string FileTransfer::serialize_fc(const FileControl& fc){ std::ostringstream oss; oss<<fc.file_name<<"|"<<fc.file_size<<"|"<<fc.chunk_size<<"|"<<fc.chunks_count; return oss.str(); }
bool FileTransfer::deserialize_fc(const std::string& s, FileControl& fc){ std::istringstream iss(s); std::string tok; if(!std::getline(iss,fc.file_name,'|'))return false; if(!std::getline(iss,tok,'|'))return false; fc.file_size=std::strtoull(tok.c_str(),nullptr,10); if(!std::getline(iss,tok,'|'))return false; fc.chunk_size=(uint32_t)std::strtoul(tok.c_str(),nullptr,10); if(!std::getline(iss,tok,'|'))return false; fc.chunks_count=(uint32_t)std::strtoul(tok.c_str(),nullptr,10); return true; }
std::string FileTransfer::serialize_chunk_header(const Chunk& c){ std::ostringstream oss; oss<<c.seq_number<<","<<c.total_chunks<<","<<c.data_size; return oss.str(); }
bool FileTransfer::parse_chunk_header(const std::string& s, Chunk& c){ std::istringstream iss(s); std::string tok; if(!std::getline(iss,tok,','))return false; c.seq_number=(uint32_t)std::strtoul(tok.c_str(),nullptr,10); if(!std::getline(iss,tok,','))return false; c.total_chunks=(uint32_t)std::strtoul(tok.c_str(),nullptr,10); if(!std::getline(iss,tok,','))return false; c.data_size=(uint32_t)std::strtoul(tok.c_str(),nullptr,10); return true; }
std::string FileTransfer::basename(const std::string& p){ auto pos = p.find_last_of("/\\"); return (pos==std::string::npos)?p:p.substr(pos+1); }
