#include <iostream>
#include <chrono>
#include <thread>
#include <atomic>
#include <csignal>
#include <set>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <memory>
#include <ctime>

#include "config/config_parser.hpp"
#include "config/config_types.hpp"
#include "utils/logger.hpp"
#include "utils/file_utils.hpp"
#include "core/cgroup_init.hpp"
#include "core/launcher_finder.hpp"
#include "core/process_scanner.hpp"
#include "core/cpuset_monitor.hpp"
#include "core/thread_matcher.hpp"
#include "core/thread_cache.hpp"
#include "scheduler/cpuset_setter.hpp"
#include "scheduler/priority_setter.hpp"
#include "scheduler/cpuctl_setter.hpp"

std::atomic<bool> running(true);

void signal_handler(int sig) {
    (void)sig;
    running = false;
}

void ensure_data_dir() {
    std::string path = "/data/adb/ReUperf";
    if (!FileUtils::dir_exists(path)) {
        mkdir(path.c_str(), 0755);
    }
    chmod(path.c_str(), 0755);
}

time_t get_file_mtime(const std::string& path) {
    struct stat st;
    if (stat(path.c_str(), &st) == 0) {
        return st.st_mtime;
    }
    return 0;
}

void process_fg_top_threads(ProcessScanner& scanner, ThreadMatcher& matcher,
                            CpusetSetter& cpuset, PrioritySetter& prio,
                            CpuctlSetter& cpuctl, ThreadCache& cache,
                            std::set<int>& processed_tids) {
    auto processes = scanner.scan_fg_top();

    for (const auto& info : processes) {
        std::string cpuset_base;
        switch (info.state) {
            case ProcessState::TOP: cpuset_base = "/dev/cpuset/top-app"; break;
            case ProcessState::FG:  cpuset_base = "/dev/cpuset/foreground"; break;
            case ProcessState::BG:  cpuset_base = "/dev/cpuset/background"; break;
        }

        for (int tid : info.tids) {
            auto it = info.thread_names.find(tid);
            if (it == info.thread_names.end()) continue;
            const std::string& thread_name = it->second;

            auto* entry = cache.lookup(tid, thread_name, info.state);
            if (entry) {
                cpuset.apply_with_result(info.pid, tid, entry->result, cpuset_base);
                prio.apply_with_result(info.pid, tid, entry->result);
                cpuctl.apply_with_result(info.pid, tid, entry->result);
            } else {
                MatchResult result = matcher.match(info.name, thread_name,
                                                    info.state, info.pid);

                std::string cpuctl_base = cpuctl.get_cpuctl_base(result.effective_state);
                cache.update(tid, thread_name, info.state, result,
                             cpuset_base, cpuctl_base);

                cpuset.apply_with_result(info.pid, tid, result, cpuset_base);
                prio.apply_with_result(info.pid, tid, result);
                cpuctl.apply_with_result(info.pid, tid, result);
            }

            processed_tids.insert(tid);
        }
    }
}

void process_pinned_threads(const Config& config, ThreadMatcher& matcher,
                            CpusetSetter& cpuset, PrioritySetter& prio,
                            CpuctlSetter& cpuctl, ThreadCache& cache,
                            std::set<int>& processed_tids) {
    static const std::string TOP_CPUSET_BASE = "/dev/cpuset/top-app";

    for (const auto& cr : matcher.compiled_rules()) {
        if (!cr.rule.pinned) continue;

        auto all_pids = FileUtils::list_pids();
        for (int pid : all_pids) {
            std::string proc_name = FileUtils::get_process_name(pid);

            if (!std::regex_search(proc_name, cr.pattern)) continue;

            auto tids = FileUtils::list_tids(pid);
            for (int tid : tids) {
                if (processed_tids.count(tid) > 0) continue;

                std::string thread_name = FileUtils::get_thread_name(pid, tid);
                ProcessState state = ProcessState::TOP;

                auto* entry = cache.lookup(tid, thread_name, state);
                if (entry) {
                    cpuset.apply_with_result(pid, tid, entry->result, TOP_CPUSET_BASE);
                    prio.apply_with_result(pid, tid, entry->result);
                    cpuctl.apply_with_result(pid, tid, entry->result);
                } else {
                    MatchResult result = matcher.match(proc_name, thread_name,
                                                        state, pid);

                    std::string cpuctl_base = cpuctl.get_cpuctl_base(result.effective_state);
                    cache.update(tid, thread_name, state, result,
                                 TOP_CPUSET_BASE, cpuctl_base);

                    cpuset.apply_with_result(pid, tid, result, TOP_CPUSET_BASE);
                    prio.apply_with_result(pid, tid, result);
                    cpuctl.apply_with_result(pid, tid, result);
                }

                processed_tids.insert(tid);
            }
        }
    }
}

int main(int argc, char* argv[]) {
    std::string config_path = "/data/adb/ReUperf/ReUperf.json";
    
    if (argc > 1) {
        config_path = argv[1];
    }
    
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
    std::signal(SIGPIPE, SIG_IGN);
    
    ensure_data_dir();
    
    Config config;
    try {
        config = ConfigParser::parse(config_path);
    } catch (const std::exception& e) {
        std::cerr << "Failed to parse config: " << e.what() << std::endl;
        return 1;
    }
    
    std::string log_path = config.sched.log.output;
    if (log_path.empty() || log_path == "stdout" || log_path == "stderr") {
        log_path = "/data/adb/ReUperf/ReUperf.log";
    }
    
    LogLevel config_level = parse_log_level(config.sched.log.level);
    Logger::instance().init(config_level, log_path, true);
    
    LOG_I("Main", "ReUperf Thread Scheduler starting...");
    LOG_I("Main", "Config path: " + config_path);
    
    if (!config.sched.enable) {
        LOG_I("Main", "Sched module disabled, exiting");
        return 0;
    }
    
    LOG_I("Main", "Log level set to: " + config.sched.log.level);
    
    config.launcher_package = LauncherFinder::find();
    LOG_I("Main", "Launcher package: " + config.launcher_package);
    
    if (!CgroupInitializer::init(config)) {
        LOG_E("Main", "Failed to initialize cgroups");
        return 1;
    }
    
    auto matcher = std::make_unique<ThreadMatcher>(config);
    auto scanner = std::make_unique<ProcessScanner>(*matcher);
    auto cpuset = std::make_unique<CpusetSetter>(config, *matcher);
    auto prio = std::make_unique<PrioritySetter>(config, *matcher);
    auto cpuctl = std::make_unique<CpuctlSetter>(config, *matcher);
    auto cache = std::make_unique<ThreadCache>();
    
    LOG_I("Main", "Initial scan and apply...");
    {
        std::set<int> processed;
        process_pinned_threads(config, *matcher, *cpuset, *prio, *cpuctl,
                               *cache, processed);
        process_fg_top_threads(*scanner, *matcher, *cpuset, *prio, *cpuctl,
                               *cache, processed);
    }
    
    CpusetMonitor monitor;
    monitor.start([&](const std::string& cgroup, const std::vector<int>& pids) {
        LOG_D("Main", "Cpuset change: " + cgroup + " has "
              + std::to_string(pids.size()) + " pids");
    });
    
    time_t last_mtime = get_file_mtime(config_path);
    
    LOG_I("Main", "Entering main loop (refresh_interval=" 
          + std::to_string(config.sched.refresh_interval_ms) + "ms)");
    
    while (running) {
        time_t current_mtime = get_file_mtime(config_path);
        if (current_mtime != last_mtime && current_mtime > 0) {
            last_mtime = current_mtime;
            
            LOG_I("Main", "Config file changed, reloading...");
            
            try {
                Config new_config = ConfigParser::parse(config_path);
                
                if (new_config.sched.enable) {
                    config = new_config;
                    
                    LogLevel new_level = parse_log_level(config.sched.log.level);
                    Logger::instance().set_level(new_level);
                    LOG_I("Main", "Log level updated to: " + config.sched.log.level);
                    
                    config.launcher_package = LauncherFinder::find();
                    CgroupInitializer::init(config);
                    
                    matcher = std::make_unique<ThreadMatcher>(config);
                    scanner = std::make_unique<ProcessScanner>(*matcher);
                    cpuset = std::make_unique<CpusetSetter>(config, *matcher);
                    prio = std::make_unique<PrioritySetter>(config, *matcher);
                    cpuctl = std::make_unique<CpuctlSetter>(config, *matcher);
                    cache->clear();
                    
                    LOG_I("Main", "Config reloaded successfully");
                } else {
                    LOG_W("Main", "New config has sched disabled, keeping old config");
                }
            } catch (const std::exception& e) {
                LOG_E("Main", "Config reload failed: " + std::string(e.what())
                      + ", keeping old config");
            }
        }
        
        std::set<int> processed_tids;
        process_pinned_threads(config, *matcher, *cpuset, *prio, *cpuctl,
                               *cache, processed_tids);
        process_fg_top_threads(*scanner, *matcher, *cpuset, *prio, *cpuctl,
                               *cache, processed_tids);
        
        std::this_thread::sleep_for(
            std::chrono::milliseconds(config.sched.refresh_interval_ms));
    }
    
    monitor.stop();
    
    LOG_I("Main", "ReUperf Thread Scheduler stopped");
    return 0;
}
