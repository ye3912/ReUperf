#ifndef CGROUP_INIT_HPP
#define CGROUP_INIT_HPP

#include <string>
#include <vector>
#include <map>
#include <unistd.h>
#include "../config/config_types.hpp"
#include "../utils/file_utils.hpp"
#include "../utils/logger.hpp"
#include "../utils/cpu_mask.hpp"

class CgroupInitializer {
public:
    static bool init(const Config& config) {
        bool success = true;
        
        success &= init_cpuset(config);
        success &= init_cpuctl(config);
        
        return success;
    }

private:
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
        
        // 检测所有物理 CPU 核心
        std::string all_cpus = CpuMask::get_all_cpus_string();
        LOG_I("CgroupInit", "Detected all CPUs: " + all_cpus);
        if (all_cpus.empty()) {
            LOG_E("CgroupInit", "No CPUs detected, cannot initialize cpuset");
            return false;
        }
        
        std::vector<std::string> groups = {"top-app", "foreground", "background", "restricted"};
        std::string base_path = "/dev/cpuset";
        int created = 0, failed = 0;
        
        for (const auto& group : groups) {
            std::string group_path = base_path + "/" + group;
            if (!FileUtils::dir_exists(group_path)) {
                LOG_D("CgroupInit", "Group " + group_path + " not found, skipping");
                continue;
            }
            
            if (!FileUtils::file_exists(group_path + "/cpus")) {
                LOG_D("CgroupInit", "Group " + group_path + " missing cpus file, skipping");
                continue;
            }
            
            // 读取原始组的 mems 值（用于传播到子组）
            std::string orig_mems = FileUtils::read_file(group_path + "/mems");
            
            // 步骤 1: 创建 ReUperf 父目录
            std::string reuperf_path = group_path + "/ReUperf";
            if (!FileUtils::mkdir_recursive(reuperf_path)) {
                LOG_W("CgroupInit", "Failed to create " + reuperf_path);
                failed += static_cast<int>(config.sched.cpumask.size());
                continue;
            }
            usleep(10000);
            
            // 步骤 2: 给父组 ReUperf/cpus 写入全部核心
            if (!FileUtils::file_exists(reuperf_path + "/cpus")) {
                LOG_W("CgroupInit", reuperf_path + "/cpus not created by kernel, skipping group");
                failed += static_cast<int>(config.sched.cpumask.size());
                continue;
            }
            
            if (!FileUtils::write_file(reuperf_path + "/cpus", all_cpus)) {
                LOG_W("CgroupInit", "Failed to set parent cpus for " + reuperf_path 
                      + " (cpus=" + all_cpus + ")");
                failed += static_cast<int>(config.sched.cpumask.size());
                continue;
            }
            LOG_D("CgroupInit", "Set " + reuperf_path + "/cpus = " + all_cpus);
            usleep(10000);
            
            // 步骤 2b: 写入父组 mems（从原始组传播）
            if (!orig_mems.empty() && FileUtils::file_exists(reuperf_path + "/mems")) {
                FileUtils::write_file(reuperf_path + "/mems", orig_mems);
                LOG_D("CgroupInit", "Set " + reuperf_path + "/mems = " + orig_mems);
                usleep(10000);
            }
            
            // 步骤 3: 创建子组并写入子集 cpus + mems
            for (const auto& cpumask : config.sched.cpumask) {
                std::string child_path = reuperf_path + "/" + cpumask.first;
                
                if (!FileUtils::mkdir_recursive(child_path)) {
                    LOG_W("CgroupInit", "Failed to create cpuset: " + child_path);
                    failed++;
                    continue;
                }
                usleep(10000);
                
                if (!FileUtils::file_exists(child_path + "/cpus")) {
                    LOG_W("CgroupInit", "cpuset control files not created for " + child_path);
                    failed++;
                    continue;
                }
                
                std::string cpus_str = CpuMask::to_string(cpumask.second);
                if (cpus_str.empty()) {
                    LOG_W("CgroupInit", "Empty cpumask for " + cpumask.first + ", skipping");
                    continue;
                }
                
                if (!FileUtils::write_file(child_path + "/cpus", cpus_str)) {
                    LOG_W("CgroupInit", "Failed to set cpus for " + child_path 
                          + " (cpus=" + cpus_str + ")");
                    failed++;
                    continue;
                }
                
                LOG_D("CgroupInit", "Set " + child_path + "/cpus = " + cpus_str);
                usleep(10000);
                
                // 写入子组 mems（从原始组传播）
                if (!orig_mems.empty() && FileUtils::file_exists(child_path + "/mems")) {
                    FileUtils::write_file(child_path + "/mems", orig_mems);
                    LOG_D("CgroupInit", "Set " + child_path + "/mems = " + orig_mems);
                    usleep(10000);
                }
                
                created++;
                
                // exclusive 标志：非关键操作，失败不记 ERROR
                if (FileUtils::file_exists(child_path + "/cpu_exclusive")) {
                    int fd = open((child_path + "/cpu_exclusive").c_str(), O_WRONLY);
                    if (fd >= 0) { const char* v = "1"; write(fd, v, 1); close(fd); }
                }
                if (FileUtils::file_exists(child_path + "/mem_exclusive")) {
                    int fd = open((child_path + "/mem_exclusive").c_str(), O_WRONLY);
                    if (fd >= 0) { const char* v = "1"; write(fd, v, 1); close(fd); }
                }
            }
        }
        
        if (failed > 0 && created == 0) {
            LOG_W("CgroupInit", "cpuset cgroups: all " + std::to_string(failed) 
                  + " groups failed");
            return false;
        }
        LOG_I("CgroupInit", "cpuset cgroups: " + std::to_string(created) 
              + " groups created, " + std::to_string(failed) + " failed");
        return true;
    }

    static bool init_cpuctl(const Config& config) {
        LOG_I("CgroupInit", "Initializing cpuctl cgroups...");
        
        if (!FileUtils::dir_exists("/dev/cpuctl")) {
            LOG_W("CgroupInit", "/dev/cpuctl not found, skipping cpuctl init");
            return true;
        }
        
        std::vector<std::string> groups = {"top-app", "foreground", "background"};
        std::string base_path = "/dev/cpuctl";
        
        for (const auto& group : groups) {
            for (const auto& rule : config.sched.rules) {
                std::string path = base_path + "/" + group + "/ReUperf/" + rule.name;
                
                if (!FileUtils::mkdir_recursive(path)) {
                    LOG_W("CgroupInit", "Failed to create cpuctl: " + path);
                    continue;
                }
                
                LOG_D("CgroupInit", "Created cpuctl group: " + path);
                
                for (const auto& tr : rule.thread_rules) {
                    if (tr.enable_limit) {
                        if (tr.uclamp_max.has_value()) {
                            std::string uclamp_path = path + "/uclamp.max";
                            if (FileUtils::file_exists(uclamp_path)) {
                                FileUtils::write_file(uclamp_path, 
                                    std::to_string(tr.uclamp_max.value()));
                            } else {
                                LOG_W("CgroupInit", uclamp_path + " not found, skipping");
                            }
                        }
                        if (tr.cpu_share.has_value()) {
                            std::string shares_path = path + "/cpu.shares";
                            if (FileUtils::file_exists(shares_path)) {
                                FileUtils::write_file(shares_path, 
                                    std::to_string(tr.cpu_share.value()));
                            } else {
                                LOG_W("CgroupInit", shares_path + " not found, skipping");
                            }
                        }
                    }
                }
            }
        }
        
        LOG_I("CgroupInit", "cpuctl cgroups initialized");
        return true;
    }
};

#endif
