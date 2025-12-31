#pragma once
#include <string>
#include <vector>
#include <atomic>
#include <thread>
#include "blocking_queue.h"

struct FileControl {
    std::string file_name;
    uint64_t file_size{};
    uint32_t chunk_size{};
    uint32_t chunks_count{};
};
struct Chunk {
    uint32_t seq_number{};
    uint32_t total_chunks{};
    uint32_t data_size{};
    std::vector<uint8_t> data;
};

class FileTransfer {
public:
    FileTransfer(const std::vector<int>& send_fds, const std::vector<int>& recv_fds, BlockingQueue<std::string>* gui_queue);
    ~FileTransfer();
    void start();
    void stop();
    void send_file(const std::string& path);
private:
    std::vector<int> send_fds_;
    std::vector<int> recv_fds_;
    BlockingQueue<std::string>* gui_queue_;
    std::atomic<bool> running_;
    std::vector<std::thread> send_threads_;
    std::vector<std::thread> recv_threads_;
    std::thread collect_thread_;
    std::thread reconstruct_thread_;
    std::vector<std::unique_ptr<BlockingQueue<Chunk>>> send_queues_;
    std::vector<std::unique_ptr<BlockingQueue<Chunk>>> recv_queues_;
    BlockingQueue<Chunk> recv_merge_q_;
    BlockingQueue<FileControl> recv_fcq_;
    void send_thread(size_t idx);
    void recv_thread(size_t idx);
    void collect_loop();
    void reconstruct_loop();
    std::string serialize_fc(const FileControl& fc);
    bool deserialize_fc(const std::string& s, FileControl& fc);
    std::string serialize_chunk_header(const Chunk& c);
    bool parse_chunk_header(const std::string& s, Chunk& c);
    static std::string basename(const std::string& p);
};
