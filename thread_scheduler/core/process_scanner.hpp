#ifndef PROCESS_SCANNER_HPP
#define PROCESS_SCANNER_HPP

#include <vector>
#include <string>
#include <map>
#include <set>
#include <sstream>
#include "../config/config_types.hpp"
#include "../utils/file_utils.hpp"
#include "../utils/logger.hpp"
#include "../core/thread_matcher.hpp"

struct ProcessInfo {
    int pid;
    std::string name;
    ProcessState state;
    std::vector<int> tids;
    std::map<int, std::string> thread_names;
};

class ProcessScanner {
public:
    ProcessScanner(ThreadMatcher& matcher) : matcher_(matcher) {}
    
    std::vector<ProcessInfo> scan_all() {
        LOG_D("ProcessScanner", "Scanning all processes...");
        
        std::vector<ProcessInfo> processes;
        auto pids = FileUtils::list_pids();
        
        for (int pid : pids) {
            ProcessInfo info;
            info.pid = pid;
            info.name = FileUtils::get_process_name(pid);
            info.state = determine_state(pid);
            
            auto tids = FileUtils::list_tids(pid);
            info.tids = tids;
            
            for (int tid : tids) {
                info.thread_names[tid] = FileUtils::get_thread_name(pid, tid);
            }
            
            processes.push_back(info);
        }
        
        LOG_D("ProcessScanner", "Found " + std::to_string(processes.size()) + " processes");
        return processes;
    }
    
    std::vector<ProcessInfo> scan_fg_top() {
        LOG_D("ProcessScanner", "Scanning foreground and top-app processes...");
        
        std::vector<ProcessInfo> processes;
        std::set<int> fg_pids = get_cgroup_pids("foreground");
        std::set<int> top_pids = get_cgroup_pids("top-app");
        
        auto all_pids = FileUtils::list_pids();
        for (int pid : all_pids) {
            bool is_fg = fg_pids.count(pid) > 0;
            bool is_top = top_pids.count(pid) > 0;
            
            if (!is_fg && !is_top) continue;
            
            ProcessInfo info;
            info.pid = pid;
            info.name = FileUtils::get_process_name(pid);
            info.state = is_top ? ProcessState::TOP : ProcessState::FG;
            
            auto tids = FileUtils::list_tids(pid);
            info.tids = tids;
            
            for (int tid : tids) {
                info.thread_names[tid] = FileUtils::get_thread_name(pid, tid);
            }
            
            processes.push_back(info);
        }
        
        LOG_D("ProcessScanner", "Found " + std::to_string(processes.size()) + " fg/top processes");
        return processes;
    }
    
    std::set<int> get_cgroup_pids(const std::string& cgroup_name) {
        std::set<int> result;
        
        std::string path = "/dev/cpuset/" + cgroup_name + "/cgroup.procs";
        std::string content = FileUtils::read_file(path);
        
        std::istringstream iss(content);
        std::string line;
        while (std::getline(iss, line)) {
            if (line.empty()) continue;
            try {
                int pid = std::stoi(line);
                if (pid > 0) result.insert(pid);
            } catch (...) {}
        }
        
        return result;
    }

private:
    ThreadMatcher& matcher_;
    
    ProcessState determine_state(int pid) {
        std::string cgroup = FileUtils::get_cgroup_path(pid, "cpuset");
        
        if (cgroup.find("top-app") != std::string::npos) {
            return ProcessState::TOP;
        } else if (cgroup.find("foreground") != std::string::npos) {
            return ProcessState::FG;
        } else {
            return ProcessState::BG;
        }
    }
};

#endif