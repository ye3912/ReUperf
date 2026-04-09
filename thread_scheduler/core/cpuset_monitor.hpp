#ifndef CPUSET_MONITOR_HPP
#define CPUSET_MONITOR_HPP

#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <functional>
#include <map>
#include <sstream>
#include <sys/inotify.h>
#include <unistd.h>
#include "../utils/logger.hpp"
#include "../utils/file_utils.hpp"

class CpusetMonitor {
public:
    using Callback = std::function<void(const std::string& cgroup, const std::vector<int>& pids)>;
    
    CpusetMonitor() : running_(false), inotify_fd_(-1) {}
    
    ~CpusetMonitor() {
        stop();
    }
    
    bool start(Callback callback) {
        callback_ = callback;
        
        inotify_fd_ = inotify_init();
        if (inotify_fd_ < 0) {
            LOG_E("CpusetMonitor", "inotify_init failed");
            return false;
        }
        
        watch_paths_.push_back("/dev/cpuset/top-app/cgroup.procs");
        watch_paths_.push_back("/dev/cpuset/foreground/cgroup.procs");
        watch_paths_.push_back("/dev/cpuset/background/cgroup.procs");
        
        for (const auto& path : watch_paths_) {
            // 先检查文件是否存在
            if (!FileUtils::file_exists(path)) {
                LOG_D("CpusetMonitor", "Path not found, skipping: " + path);
                continue;
            }
            
            int wd = inotify_add_watch(inotify_fd_, path.c_str(), IN_MODIFY);
            if (wd < 0) {
                LOG_W("CpusetMonitor", "Failed to watch: " + path);
            } else {
                watch_fds_[wd] = path;
                LOG_D("CpusetMonitor", "Watching: " + path);
            }
        }
        
        running_ = true;
        thread_ = std::thread(&CpusetMonitor::monitor_loop, this);
        
        LOG_I("CpusetMonitor", "Started monitoring cpuset changes");
        return true;
    }
    
    void stop() {
        running_ = false;
        if (thread_.joinable()) {
            thread_.join();
        }
        
        if (inotify_fd_ >= 0) {
            for (auto& kv : watch_fds_) {
                inotify_rm_watch(inotify_fd_, kv.first);
            }
            close(inotify_fd_);
            inotify_fd_ = -1;
        }
        
        LOG_I("CpusetMonitor", "Stopped monitoring");
    }
    
    bool is_running() const { return running_; }

private:
    void monitor_loop() {
        char buffer[4096];
        
        while (running_) {
            ssize_t length = read(inotify_fd_, buffer, sizeof(buffer));
            if (length < 0) {
                if (!running_) break;
                LOG_E("CpusetMonitor", "read error on inotify");
                continue;
            }
            
            ssize_t i = 0;
            while (i < length) {
                struct inotify_event* event = (struct inotify_event*)&buffer[i];
                
                if (event->mask & IN_MODIFY) {
                    auto it = watch_fds_.find(event->wd);
                    if (it != watch_fds_.end()) {
                        std::string path = it->second;
                        std::string cgroup;
                        
                        if (path.find("top-app") != std::string::npos) {
                            cgroup = "top-app";
                        } else if (path.find("foreground") != std::string::npos) {
                            cgroup = "foreground";
                        } else if (path.find("background") != std::string::npos) {
                            cgroup = "background";
                        }
                        
                        std::vector<int> pids = read_pids_from_file(path);
                        
                        LOG_D("CpusetMonitor", "Change detected in " + cgroup 
                              + ", " + std::to_string(pids.size()) + " pids");
                        
                        if (callback_) {
                            callback_(cgroup, pids);
                        }
                    }
                }
                
                i += sizeof(struct inotify_event) + event->len;
            }
        }
    }
    
    std::vector<int> read_pids_from_file(const std::string& path) {
        std::vector<int> pids;
        std::string content = FileUtils::read_file(path);
        
        std::istringstream iss(content);
        std::string line;
        while (std::getline(iss, line)) {
            try {
                int pid = std::stoi(line);
                if (pid > 0) pids.push_back(pid);
            } catch (...) {}
        }
        
        return pids;
    }
    
    std::atomic<bool> running_;
    int inotify_fd_;
    std::thread thread_;
    Callback callback_;
    
    std::vector<std::string> watch_paths_;
    std::map<int, std::string> watch_fds_;
};

#endif