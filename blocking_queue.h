#pragma once
#include <queue>
#include <mutex>
#include <condition_variable>
#include <atomic>

// Thread-safe blocking queue template
// Used for both Message and Chunk passing

template<typename T>
class BlockingQueue {
public:
    void push(T&& item) { { std::lock_guard<std::mutex> lk(m_); q_.push(std::move(item)); } cv_.notify_one(); }
    bool wait_pop(T& out, std::atomic<bool>& running) {
        std::unique_lock<std::mutex> lk(m_);
        cv_.wait(lk,[&]{return !q_.empty()||!running.load();});
        if(!running.load()&&q_.empty()) return false;
        out=std::move(q_.front()); q_.pop(); return true;
    }
    bool try_pop(T& out) { std::lock_guard<std::mutex> lk(m_); if(q_.empty()) return false; out=std::move(q_.front()); q_.pop(); return true; }
    size_t size() const { std::lock_guard<std::mutex> lk(m_); return q_.size(); }
    void notify_all(){ cv_.notify_all(); }
private: mutable std::mutex m_; std::condition_variable cv_; std::queue<T> q_;
};
