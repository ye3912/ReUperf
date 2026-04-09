#ifndef THREAD_CACHE_HPP
#define THREAD_CACHE_HPP

#include <vector>
#include <string>
#include "../config/config_types.hpp"
#include "../core/thread_matcher.hpp"
#include "../utils/logger.hpp"

struct ThreadCacheEntry {
    int tid;
    std::string thread_name;
    ProcessState actual_state;
    MatchResult result;
    std::string cpuset_base;
    std::string cpuctl_base;
};

class ThreadCache {
public:
    ThreadCacheEntry* lookup(int tid, const std::string& thread_name,
                             ProcessState actual_state) {
        for (auto& entry : entries_) {
            if (entry.tid == tid
                && entry.thread_name == thread_name
                && entry.actual_state == actual_state) {
                return &entry;
            }
        }
        return nullptr;
    }

    void update(int tid, const std::string& thread_name,
                ProcessState actual_state, const MatchResult& result,
                const std::string& cpuset_base, const std::string& cpuctl_base) {
        for (auto& entry : entries_) {
            if (entry.tid == tid) {
                entry.thread_name = thread_name;
                entry.actual_state = actual_state;
                entry.result = result;
                entry.cpuset_base = cpuset_base;
                entry.cpuctl_base = cpuctl_base;
                return;
            }
        }
        entries_.push_back({tid, thread_name, actual_state, result, cpuset_base, cpuctl_base});
    }

    void clear() {
        entries_.clear();
    }

private:
    std::vector<ThreadCacheEntry> entries_;
};

#endif
