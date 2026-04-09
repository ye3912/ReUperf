#ifndef CPUSET_SETTER_HPP
#define CPUSET_SETTER_HPP

#include <string>
#include <vector>
#include <set>
#include <fcntl.h>
#include "../config/config_types.hpp"
#include "../utils/file_utils.hpp"
#include "../utils/cpu_mask.hpp"
#include "../utils/logger.hpp"
#include "../core/thread_matcher.hpp"

class CpusetSetter {
public:
    CpusetSetter(const Config& config, ThreadMatcher& matcher) 
        : config_(config), matcher_(matcher) {}
    
    bool set_affinity(int tid, const std::string& affinity_class, ProcessState state) {
        auto cpus = matcher_.get_cpus_for_affinity(affinity_class, state);
        
        if (cpus.empty()) {
            return true;
        }
        
        if (!CpuMask::set_affinity(tid, cpus)) {
            LOG_W("CpusetSetter", "Failed to set affinity for tid " + std::to_string(tid));
            return false;
        }
        
        LOG_T("CpusetSetter", "Set affinity for tid " + std::to_string(tid) 
              + " to " + CpuMask::to_string(cpus));
        return true;
    }
    
    bool move_to_cpuset_cgroup(int tid, const std::string& affinity_class, 
                               ProcessState state, const std::string& cgroup_base) {
        auto cpus = matcher_.get_cpus_for_affinity(affinity_class, state);
        
        if (cpus.empty()) {
            return true;
        }
        
        std::string cpumask_name;
        for (const auto& kv : config_.sched.cpumask) {
            if (kv.second.size() == cpus.size() && 
                std::equal(kv.second.begin(), kv.second.end(), cpus.begin())) {
                cpumask_name = kv.first;
                break;
            }
        }
        
        if (cpumask_name.empty()) {
            return true;
        }
        
        std::string path = cgroup_base + "/ReUperf/" + cpumask_name;
        
        // 检查该组是否已被标记为无效（cpus 为空）
        if (skip_groups_.count(path) > 0) {
            return false;
        }
        
        // 首次写入前检查 cpus 是否有效
        if (!checked_groups_.count(path)) {
            checked_groups_.insert(path);
            std::string cpus_val = FileUtils::read_file(path + "/cpus");
            if (cpus_val.empty()) {
                LOG_W("CpusetSetter", path + "/cpus is empty, skipping all moves to this group");
                skip_groups_.insert(path);
                return false;
            }
        }
        
        // 两步写入：先确保线程在上级 ReUperf 组，再写入子组
        std::string parent_path = cgroup_base + "/ReUperf";
        write_cgroup_procs_silent(parent_path + "/cgroup.procs", tid);
        
        if (!FileUtils::write_file(path + "/cgroup.procs", std::to_string(tid))) {
            LOG_W("CpusetSetter", "Failed to move tid " + std::to_string(tid) + " to " + path);
            return false;
        }
        
        LOG_T("CpusetSetter", "Moved tid " + std::to_string(tid) + " to cpuset " + path);
        return true;
    }
    
    bool apply_with_result(int /*pid*/, int tid, const MatchResult& result,
                           const std::string& cgroup_base) {
        if (!result.matched) {
            return true;
        }
        
        bool success = true;
        
        if (!result.affinity_class.empty() && result.affinity_class != "auto") {
            success &= set_affinity(tid, result.affinity_class, result.effective_state);
            success &= move_to_cpuset_cgroup(tid, result.affinity_class, 
                                             result.effective_state, cgroup_base);
        }
        
        return success;
    }

private:
    const Config& config_;
    ThreadMatcher& matcher_;
    std::set<std::string> checked_groups_;  // 已检查过 cpus 的组
    std::set<std::string> skip_groups_;     // cpus 为空需跳过的组
    
    // 静默写入 cgroup.procs（不记日志，用于非关键的父组先行写入）
    static bool write_cgroup_procs_silent(const std::string& path, int tid) {
        int fd = open(path.c_str(), O_WRONLY | O_TRUNC);
        if (fd < 0) return false;
        std::string val = std::to_string(tid);
        ssize_t ret = write(fd, val.c_str(), val.size());
        close(fd);
        return ret > 0;
    }
};

#endif
