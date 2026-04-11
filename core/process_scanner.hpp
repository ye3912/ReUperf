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

struct ProcessInfo {
    int pid;
    std::string name;
    ProcessState state;
    std::vector<int> tids;
    std::map<int, std::string> thread_names;
};

class ProcessScanner {
public:
    ProcessScanner() = default;
    
    std::vector<ProcessInfo> scan_all() {
        LOG_D("ProcessScanner", "Scanning all processes...");
        
        std::vector<ProcessInfo> processes;
        auto pids = FileUtils::list_pids();
        
        for (int pid : pids) {
            ProcessInfo info;
            info.pid = pid;
<<<<<<< HEAD
            info.name = FileUtils::get_process_name(pid);
=======
            info.name = FileUtils::get_process_name_from_status(pid);
            
>>>>>>> fd74538 (更新: 修复CI配置，优化构建脚本 2026-04-11 22:49)
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
        
        auto all_pids = FileUtils::list_pids();
        for (int pid : all_pids) {
            FileUtils::CgroupState state = FileUtils::get_cgroup_state(pid);
            
            if (state != FileUtils::CgroupState::TOP && state != FileUtils::CgroupState::FG) {
                continue;
            }
            
<<<<<<< HEAD
            std::string proc_name = FileUtils::get_process_name(pid);
=======
            std::string proc_name = FileUtils::get_process_name_from_status(pid);
>>>>>>> fd74538 (更新: 修复CI配置，优化构建脚本 2026-04-11 22:49)
            if (proc_name == "[dead]") continue;
            
            ProcessInfo info;
            info.pid = pid;
            info.name = proc_name;
            info.state = (state == FileUtils::CgroupState::TOP) ? ProcessState::TOP : ProcessState::FG;
            
            auto tids = FileUtils::list_tids(pid);
            info.tids = tids;
            
            for (int tid : tids) {
                std::string thread_name = FileUtils::get_thread_name(pid, tid);
                if (thread_name != "[dead]") {
                    info.thread_names[tid] = thread_name;
                }
            }
            
            processes.push_back(info);
        }
        
        if (processes.empty()) {
            LOG_W("ProcessScanner", "scan_fg_top: no fg/top processes found");
        }
        LOG_D("ProcessScanner", "Found " + std::to_string(processes.size()) + " fg/top processes");
        return processes;
    }
    
    std::set<int> get_cgroup_pids(const std::string& cgroup_name) {
        std::set<int> result;
        
        std::string path = "/dev/cpuset/cgroup.procs";
        
        if (!FileUtils::file_exists(path)) {
            LOG_W("ProcessScanner", "Root cgroup.procs not found: " + path);
            return result;
        }
        
        std::string content = FileUtils::read_file(path);
        
        if (content.empty()) {
            LOG_W("ProcessScanner", "Empty cgroup.procs file: " + path);
            return result;
        }
        
        std::istringstream iss(content);
        std::string line;
        while (std::getline(iss, line)) {
            if (line.empty()) continue;
            try {
                int pid = std::stoi(line);
                if (pid > 0) {
                    FileUtils::CgroupState state = FileUtils::get_cgroup_state(pid);
                    if (cgroup_name == "top-app" && state == FileUtils::CgroupState::TOP) {
                        result.insert(pid);
                    } else if (cgroup_name == "foreground" && state == FileUtils::CgroupState::FG) {
                        result.insert(pid);
                    } else if (cgroup_name == "background" && state == FileUtils::CgroupState::BG) {
                        result.insert(pid);
                    }
                }
            } catch (const std::exception& e) {
                LOG_W("ProcessScanner", "Failed to parse PID in " + path + ": " + std::string(e.what()));
            }
        }
        
        return result;
    }

private:
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

<<<<<<< HEAD
#endif
=======
#endif
>>>>>>> fd74538 (更新: 修复CI配置，优化构建脚本 2026-04-11 22:49)
