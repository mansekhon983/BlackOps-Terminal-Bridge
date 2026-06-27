#pragma once
// Thread-safe FIFO queues bridging the engine thread and the shell worker.
//
//   submit hook (main thread)  --push-->  inbound  --pop-->  worker thread
//   worker thread              --push-->  outbound --pop-->  per-frame (main)
//
// Keep the engine side lock-hold time tiny: pop, release, then call engine fns.

#include <string>
#include <deque>
#include <mutex>
#include <optional>

class StringQueue {
public:
    void push(std::string s);
    // Non-blocking: returns std::nullopt if empty.
    std::optional<std::string> tryPop();
    bool empty();

private:
    std::mutex mtx_;
    std::deque<std::string> q_;
};

// Global queues (defined in queues.cpp).
extern StringQueue g_inbound;   // captured commands -> worker
extern StringQueue g_outbound;  // shell output lines -> per-frame render
