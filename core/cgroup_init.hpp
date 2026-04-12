#ifndef CGROUP_INIT_HPP
#define CGROUP_INIT_HPP

#include <string>
#include <vector>
#include <map>
#ifdef __ANDROID__
#include <unistd.h>
#endif
#include "../config/config_types.hpp"
#include "../utils/file_utils.hpp"
#include "../utils/logger.hpp"
#include "../utils/cpu_mask.hpp"

class CgroupInitializer {
public:
    // 全局标志：记录内核是否支持 uclamp
    static inline bool uclamp_supported = true;

    static bool init(const Config& config) {
        bool success = true;
        success &= init_cpuset(config);
        success &= init_cpuctl(config);

        // 如果 uclamp 不支持，尝试 schedtune 回退
        if (!uclamp_supported) {
            LOG_W("CgroupInit", "Uclamp NOT supported. Attempting schedtune fallback...");
            success &= init_schedtune(config);
        }

        return success;
    }

private:
    static bool ensure_cgroup_file_exists(const std::string& path, int max_retries = 5) {
        for (int i = 0; i < max_retries; ++i) {
            if (FileUtils::file_exists(path)) {
                return true;
            }
#ifdef __ANDROID__
            usleep(5000);
#endif
        }
        return false;
    }

    static bool init_cpuset(const Config& config) {
        LOG_I("CgroupInit", "Initializing cpuset cgroups...");
        if (!FileUtils::dir_exists("/dev/cpuset")) {
            LOG_W("CgroupInit", "/dev/cpuset not found, skipping cpuset init");            return true;
        }
        if (!FileUtils::file_exists("/dev/cpuset/cpus")) {
            LOG_W("CgroupInit", "cpuset not properly mounted (missing /dev/cpuset/cpus), skipping");
            return true;
        }

        // 检测所有物理 CPU 核心
        std::string all_cpus = CpuMask::get_all_cpus_string();
        LOG_I("CgroupInit", "Detected all CPUs: " + all_cpus);
        if (all_cpus.empty()) {
            LOG_E("CgroupInit", "No CPUs detected, cannot initialize cpuset");
            return false;
        }

        std::string base_path = "/dev/cpuset";
        std::string root_mems = FileUtils::read_file(base_path + "/mems");
        int created = 0, failed = 0;

        // 创建各规则子组
        for (const auto& cpumask : config.sched.cpumask) {
            std::string child_path = base_path + "/ReUperf_" + cpumask.first;
            if (!FileUtils::mkdir_recursive(child_path)) {
                LOG_W("CgroupInit", "Failed to create cpuset: " + child_path);
                failed++;
                continue;
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

            // 先写 mems，再写 cpus（Android 内核要求）
            if (!root_mems.empty() && ensure_cgroup_file_exists(child_path + "/mems")) {
                if (!FileUtils::write_file(child_path + "/mems", root_mems)) {
                    LOG_W("CgroupInit", "Failed to set mems for " + child_path);
                    failed++;
                }
#ifdef __ANDROID__
                usleep(10000); // 10ms 延迟
#endif
            }
            if (!FileUtils::write_file(child_path + "/cpus", cpus_str)) {
                LOG_W("CgroupInit", "Failed to set cpus for " + child_path + " (cpus=" + cpus_str + ")");
                failed++;
                continue;
            }
            LOG_D("CgroupInit", "Set " + child_path + "/cpus = " + cpus_str);
            created++;

            if (FileUtils::file_exists(child_path + "/cpu_exclusive")) {
                int fd = open((child_path + "/cpu_exclusive").c_str(), O_WRONLY);
                if (fd >= 0) {
                    const char* v = "1";
                    ssize_t written = write(fd, v, 1);
                    int err = errno;
                    close(fd);
                    if (written != 1) {
                        LOG_W("CgroupInit", "Failed to write cpu_exclusive: " + std::string(strerror(err)));
                    }
                }
            }
            if (FileUtils::file_exists(child_path + "/mem_exclusive")) {
                int fd = open((child_path + "/mem_exclusive").c_str(), O_WRONLY);
                if (fd >= 0) {
                    const char* v = "1";
                    ssize_t written = write(fd, v, 1);
                    int err = errno;
                    close(fd);
                    if (written != 1) {
                        LOG_W("CgroupInit", "Failed to write mem_exclusive: " + std::string(strerror(err)));
                    }
                }
            }
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
            return false;
        }
        bool first_uclamp_attempt = true;
        for (const auto& rule : config.sched.rules) {
            std::string path = reuperf_path + "/" + rule.name;
            if (!FileUtils::mkdir_recursive(path)) {
                LOG_W("CgroupInit", "Failed to create cpuctl: " + path);
                continue;
            }

            for (const auto& tr : rule.thread_rules) {
                if (tr.enable_limit && tr.uclamp_max.has_value()) {
                    std::string uclamp_path = path + "/cpu.uclamp.max";
                    if (FileUtils::file_exists(uclamp_path)) {
                        if (!FileUtils::write_file(uclamp_path, std::to_string(tr.uclamp_max.value()))) {
                            if (first_uclamp_attempt) {
                                LOG_W("CgroupInit", "Uclamp write failed, marking unsupported.");
                                uclamp_supported = false;
                                first_uclamp_attempt = false;
                            }
                        }
                    } else {
                        if (first_uclamp_attempt) {
                            LOG_W("CgroupInit", "Uclamp file not found, marking unsupported.");
                            uclamp_supported = false;
                            first_uclamp_attempt = false;
                        }
                    }
                }
            }
        }
        LOG_I("CgroupInit", "cpuctl initialized (Uclamp: " + std::string(uclamp_supported ? "Yes" : "No") + ")");
        return true;
    }

    // Schedtune 回退逻辑
    static bool init_schedtune(const Config& config) {
        std::string stune_base = "/dev/stune";
        if (!FileUtils::dir_exists(stune_base)) {
            LOG_W("CgroupInit", "Schedtune directory not found.");
            return false;
        }

        std::string reuperf_stune_path = stune_base + "/ReUperf";
        if (!FileUtils::mkdir_recursive(reuperf_stune_path)) {
            return false;
        }

        bool any_applied = false;
        for (const auto& rule : config.sched.rules) {
            std::string path = reuperf_stune_path + "/" + rule.name;            if (!FileUtils::mkdir_recursive(path)) {
                continue;
            }

            int max_uclamp = -1;
            for (const auto& tr : rule.thread_rules) {
                if (tr.uclamp_max.has_value() && tr.uclamp_max.value() > max_uclamp) {
                    max_uclamp = tr.uclamp_max.value();
                }
            }

            if (max_uclamp >= 0) {
                int boost = (max_uclamp * 100) / 1024;
                std::string boost_path = path + "/schedtune.boost";
                
                if (FileUtils::file_exists(boost_path) && FileUtils::write_file(boost_path, std::to_string(boost))) {
                    LOG_I("CgroupInit", "Applied schedtune: " + rule.name + " boost=" + std::to_string(boost));
                    any_applied = true;
                    
                    if (boost > 50) {
                        std::string idle_path = path + "/schedtune.prefer_idle";
                        if (FileUtils::file_exists(idle_path)) {
                            FileUtils::write_file(idle_path, "1");
                        }
                    }
                }
            }
        }
        return any_applied;
    }
};

#endif // CGROUP_INIT_HPP