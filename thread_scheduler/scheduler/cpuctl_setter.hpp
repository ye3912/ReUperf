#ifndef CPUCTL_SETTER_HPP
#define CPUCTL_SETTER_HPP

#include <string>
#include <optional>
#include "../config/config_types.hpp"
#include "../utils/file_utils.hpp"
#include "../utils/logger.hpp"
#include "../core/thread_matcher.hpp"

class CpuctlSetter {
public:
    CpuctlSetter(const Config& /*config*/, ThreadMatcher& matcher) 
        : matcher_(matcher) {}
    
    bool set_uclamp_max(int tid, int uclamp_max, const std::string& cpuctl_base,
                        const std::string& rule_name) {
        std::string path = cpuctl_base + "/ReUperf/" + rule_name;
        
        if (!FileUtils::dir_exists(path)) {
            LOG_W("CpuctlSetter", "cpuctl group not exists: " + path);
            return false;
        }
        
        if (!FileUtils::write_file(path + "/uclamp.max", std::to_string(uclamp_max))) {
            LOG_W("CpuctlSetter", "Failed to write uclamp.max for " + path);
            return false;
        }
        
        LOG_T("CpuctlSetter", "Set uclamp.max=" + std::to_string(uclamp_max) + " for " + path);
        
        if (!FileUtils::write_cgroup_procs(path, tid)) {
            LOG_W("CpuctlSetter", "Failed to move tid " + std::to_string(tid) + " to " + path);
            return false;
        }
        
        return true;
    }
    
    bool set_cpu_share(int tid, int cpu_share, const std::string& cpuctl_base,
                       const std::string& rule_name) {
        std::string path = cpuctl_base + "/ReUperf/" + rule_name;
        
        if (!FileUtils::dir_exists(path)) {
            LOG_W("CpuctlSetter", "cpuctl group not exists: " + path);
            return false;
        }
        
        if (!FileUtils::write_file(path + "/cpu.shares", std::to_string(cpu_share))) {
            LOG_W("CpuctlSetter", "Failed to write cpu.shares for " + path);
            return false;
        }
        
        LOG_T("CpuctlSetter", "Set cpu.shares=" + std::to_string(cpu_share) + " for " + path);
        
        if (!FileUtils::write_cgroup_procs(path, tid)) {
            LOG_W("CpuctlSetter", "Failed to move tid " + std::to_string(tid) + " to " + path);
            return false;
        }
        
        return true;
    }
    
    std::string get_cpuctl_base(ProcessState state) {
        switch (state) {
            case ProcessState::TOP: return "/dev/cpuctl/top-app";
            case ProcessState::FG:  return "/dev/cpuctl/foreground";
            case ProcessState::BG:  return "/dev/cpuctl/background";
        }
        return "/dev/cpuctl/background";
    }
    
    bool apply_with_result(int /*pid*/, int tid, const MatchResult& result) {
        if (!result.matched || !result.enable_limit) {
            return true;
        }
        
        bool success = true;
        std::string base = get_cpuctl_base(result.effective_state);
        
        if (result.uclamp_max.has_value()) {
            success &= set_uclamp_max(tid, result.uclamp_max.value(), 
                                      base, result.matched_rule_name);
        }
        
        if (result.cpu_share.has_value()) {
            success &= set_cpu_share(tid, result.cpu_share.value(),
                                     base, result.matched_rule_name);
        }
        
        return success;
    }

private:
    ThreadMatcher& matcher_;
};

#endif
