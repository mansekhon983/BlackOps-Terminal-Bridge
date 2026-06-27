#include "queues.h"

StringQueue g_inbound;
StringQueue g_outbound;

void StringQueue::push(std::string s) {
    std::lock_guard<std::mutex> lk(mtx_);
    q_.push_back(std::move(s));
}

std::optional<std::string> StringQueue::tryPop() {
    std::lock_guard<std::mutex> lk(mtx_);
    if (q_.empty()) return std::nullopt;
    std::string s = std::move(q_.front());
    q_.pop_front();
    return s;
}

bool StringQueue::empty() {
    std::lock_guard<std::mutex> lk(mtx_);
    return q_.empty();
}
