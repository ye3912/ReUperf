#ifndef CGROUP_INIT_HPP
#define CGROUP_INIT_HPP

#include <string>
#include <vector>
#include <map>
#include <cerrno>
#include <fcntl.h>
#ifdef __ANDROID__
#include <unistd.h>
#endif

#include "../config/config_types.hpp"
#include "../utils/file_utils.hpp"
#include "../utils/logger.hpp"
#include "../utils/cpu_mask.hpp"

class CgroupInitializer {
public:
    // 静态标志：追踪 uclamp 是否可用，默认为 true
    static bool uclamp_supported;

    static bool init(const Config& config) {
        LOG_I("CgroupInit", "Starting cgroup initialization...");
        uclamp_supported = true; // 重置状态

        bool success = true;
        success &= init_cpuset(config);
        success &= init_cpuctl(config);

        // ✅ 核心逻辑：如果 uclamp 初始化失败或不支持，尝试 schedtune 回退
        if (!uclamp_supported) {
            LOG_W("CgroupInit", "Uclamp is NOT supported/failed. Attempting schedtune fallback...");
            success &= init_schedtune(config);
        }

        if (success) {
            LOG_I("CgroupInit", "Cgroup initialization completed successfully");
        } else {
            LOG_W("CgroupInit", "Cgroup initialization completed with warnings");
        }
        return success;
    }

private:
    static bool ensure_cgroup_file_exists(const std::string& path, int max_retries = 5) {
        for (int i = 0; i < max_retries; ++i) {
            if (FileUtils::file_exists(path)) {
                return true;
            }#ifdef __ANDROID__
            usleep(5000);
#endif
        }
        return false;
    }

    static bool init_cpuset(const Config& config) {
        LOG_I("CgroupInit", "Initializing cpuset cgroups...");
        
        if (!FileUtils::dir_exists("/dev/cpuset")) {
            LOG_W("CgroupInit", "/dev/cpuset not found, skipping cpuset init");
            return true;
        }
        if (!FileUtils::file_exists("/dev/cpuset/cpus")) {
            LOG_W("CgroupInit", "cpuset not properly mounted (missing /dev/cpuset/cpus), skipping");
            return true;
        }

        std::string all_cpus = CpuMask::get_all_cpus_string();
        LOG_I("CgroupInit", "Detected all CPUs: " + all_cpus);
        if (all_cpus.empty()) {
            LOG_E("CgroupInit", "No CPUs detected, cannot initialize cpuset");
            return false;
        }

        std::string base_path = "/dev/cpuset";
        std::string root_mems = FileUtils::read_file(base_path + "/mems");
        
        // 1. 创建父节点 /dev/cpuset/ReUperf
        std::string parent_path = base_path + "/ReUperf";
        if (FileUtils::mkdir_recursive(parent_path)) {
            if (!root_mems.empty() && ensure_cgroup_file_exists(parent_path + "/mems")) {
                FileUtils::write_file(parent_path + "/mems", root_mems);
            }
            if (ensure_cgroup_file_exists(parent_path + "/cpus")) {
                FileUtils::write_file(parent_path + "/cpus", all_cpus);
            }
            // Android 内核同步延迟
#ifdef __ANDROID__
            usleep(10000); 
#endif
        }

        int created = 0, failed = 0;
        for (const auto& cpumask : config.sched.cpumask) {
            std::string child_path = base_path + "/ReUperf_" + cpumask.first;
            if (!FileUtils::mkdir_recursive(child_path)) {
                LOG_W("CgroupInit", "Failed to create cpuset: " + child_path);
                failed++;                continue;
            }
            if (!ensure_cgroup_file_exists(child_path + "/cpus")) {
                LOG_W("CgroupInit", "cpuset control files not created for " + child_path);
                failed++;
                continue;
            }

            std::string cpus_str = CpuMask::to_string(cpumask.second);
            if (cpus_str.empty()) {
                LOG_W("CgroupInit", "Empty cpumask for " + cpumask.first + ", skipping");
                continue;
            }

            // 2. ✅ 关键修复：先写 mems，再写 cpus（Android 内核顺序要求）
            if (!root_mems.empty() && ensure_cgroup_file_exists(child_path + "/mems")) {
                if (!FileUtils::write_file(child_path + "/mems", root_mems)) {
                    LOG_W("CgroupInit", "Failed to set mems for " + child_path);
                    failed++; 
                }
#ifdef __ANDROID__
                usleep(10000); // 写入 mems 后延迟
#endif
            }

            if (!FileUtils::write_file(child_path + "/cpus", cpus_str)) {
                LOG_W("CgroupInit", "Failed to set cpus for " + child_path + " (cpus=" + cpus_str + ")");
                failed++;
                continue;
            }

            LOG_D("CgroupInit", "Set " + child_path + "/cpus = " + cpus_str);
            created++;
        }
        LOG_I("CgroupInit", "cpuset cgroups: " + std::to_string(created) + " groups created, " + std::to_string(failed) + " failed");
        return true;
    }

    static bool init_cpuctl(const Config& config) {
        LOG_I("CgroupInit", "Initializing cpuctl cgroups...");
        if (!FileUtils::dir_exists("/dev/cpuctl")) {
            LOG_W("CgroupInit", "/dev/cpuctl not found, skipping cpuctl init");
            return true;
        }

        std::string base_path = "/dev/cpuctl";
        std::string reuperf_path = base_path + "/ReUperf";
        if (!FileUtils::mkdir_recursive(reuperf_path)) {
            LOG_W("CgroupInit", "Failed to create " + reuperf_path);
            return false;        }

        for (const auto& rule : config.sched.rules) {
            std::string path = reuperf_path + "/" + rule.name;
            if (!FileUtils::mkdir_recursive(path)) {
                LOG_W("CgroupInit", "Failed to create cpuctl: " + path);
                continue;
            }

            for (const auto& tr : rule.thread_rules) {
                if (tr.enable_limit) {
                    // 处理 uclamp
                    if (tr.uclamp_max.has_value()) {
                        std::string uclamp_path = path + "/cpu.uclamp.max";
                        if (FileUtils::file_exists(uclamp_path)) {
                            // 尝试写入，如果失败则标记 uclamp 不支持
                            if (!FileUtils::write_file(uclamp_path, std::to_string(tr.uclamp_max.value()))) {
                                LOG_W("CgroupInit", "Uclamp write failed for " + uclamp_path + " (errno=" + std::to_string(errno) + "), marking unsupported.");
                                uclamp_supported = false;
                            }
                        } else {
                            // 文件不存在，通常意味着内核不支持 uclamp
                            LOG_W("CgroupInit", "Uclamp file not found: " + uclamp_path + ", marking unsupported.");
                            uclamp_supported = false;
                        }
                    }
                    
                    // 处理 cpu.shares (作为补充)
                    if (tr.cpu_share.has_value()) {
                        std::string shares_path = path + "/cpu.shares";
                        if (FileUtils::file_exists(shares_path)) {
                            FileUtils::write_file(shares_path, std::to_string(tr.cpu_share.value()));
                        }
                    }
                }
            }
        }
        LOG_I("CgroupInit", "cpuctl cgroups initialized (Uclamp supported: " + std::string(uclamp_supported ? "Yes" : "No") + ")");
        return true;
    }

    // ✅ 新增：Schedtune 回退逻辑
    static bool init_schedtune(const Config& config) {
        std::string stune_base = "/dev/stune";
        if (!FileUtils::dir_exists(stune_base)) {
            LOG_W("CgroupInit", "Schedtune directory not found at " + stune_base + ", skipping fallback.");
            return false;
        }

        std::string reuperf_stune_path = stune_base + "/ReUperf";        if (!FileUtils::mkdir_recursive(reuperf_stune_path)) {
            LOG_W("CgroupInit", "Failed to create schedtune base path: " + reuperf_stune_path);
            return false;
        }

        bool any_applied = false;

        for (const auto& rule : config.sched.rules) {
            std::string path = reuperf_stune_path + "/" + rule.name;
            if (!FileUtils::mkdir_recursive(path)) {
                LOG_W("CgroupInit", "Failed to create schedtune group: " + path);
                continue;
            }

            // 寻找该规则下的最大 uclamp_max 值作为 boost 参考
            int max_uclamp = -1;
            for (const auto& tr : rule.thread_rules) {
                if (tr.uclamp_max.has_value()) {
                    if (tr.uclamp_max.value() > max_uclamp) {
                        max_uclamp = tr.uclamp_max.value();
                    }
                }
            }

            // 如果配置了 uclamp_max，将其映射到 schedtune.boost
            if (max_uclamp >= 0) {
                // 映射公式：uclamp(0-1024) -> boost(0-100)
                int boost = (max_uclamp * 100) / 1024;
                
                std::string boost_path = path + "/schedtune.boost";
                if (FileUtils::file_exists(boost_path)) {
                    if (FileUtils::write_file(boost_path, std::to_string(boost))) {
                        LOG_I("CgroupInit", "Applied schedtune fallback: " + rule.name + " boost=" + std::to_string(boost));
                        any_applied = true;
                        
                        // 高优先级任务倾向于抢占空闲核心 (Prefer Idle)
                        if (boost > 50) {
                            std::string idle_path = path + "/schedtune.prefer_idle";
                            if (FileUtils::file_exists(idle_path)) {
                                FileUtils::write_file(idle_path, "1");
                            }
                        }
                    }
                }
            }
        }
        
        if (!any_applied) {
            LOG_I("CgroupInit", "No schedtune rules applied.");
        }        return any_applied;
    }
};

// 初始化静态成员
bool CgroupInitializer::uclamp_supported = true;

#endif // CGROUP_INIT_HPP