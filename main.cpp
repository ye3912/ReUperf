#include <iostream>
#include <chrono>
#include <thread>
#include <atomic>
#include <csignal>
#include <set>
#include <cstdint>
#include <fstream>
#ifdef __linux__
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#endif
#include <memory>
#include <ctime>
#include <regex>

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
#include "core/scan_worker.hpp"
#include "scheduler/cpuset_setter.hpp"
#include "scheduler/priority_setter.hpp"
#include "scheduler/cpuctl_setter.hpp"

namespace {
    std::atomic<bool> running(true);
    std::atomic<bool> cgroup_changed(false);
    std::atomic<bool> full_rescan_needed(true);
    std::atomic<bool> shutdown_requested(false);
}

void signal_handler(int sig) {
    (void)sig;
    running.store(false, std::memory_order_seq_cst);
    shutdown_requested.store(true, std::memory_order_seq_cst);
    std::atomic_thread_fence(std::memory_order_seq_cst);
}

void ensure_data_dir() {
    std::string path = "/data/adb/ReUperf";
    if (!FileUtils::mkdir_recursive(path)) {
        LOG_E("Main", "Failed to create data dir: " + path);
    }
    (void)FileUtils::dir_exists(path);
}

time_t get_file_mtime(const std::string& path) {
    struct stat st;
    if (stat(path.c_str(), &st) == 0) {
        return st.st_mtime;
    }
    return 0;
}

// Simple hash for config change detection (non-cryptographic)
uint64_t compute_config_hash(const std::string& path) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs.is_open()) return 0;
    
    uint64_t hash = 1469598103934665603ULL; // FNV-1a offset basis
    char buf[4096];
    while (ifs.read(buf, sizeof(buf)) || ifs.gcount() > 0) {
        std::streamsize len = ifs.gcount();
        for (std::streamsize i = 0; i < len; ++i) {
            hash ^= static_cast<unsigned char>(buf[i]);
            hash *= 1099511628211ULL; // FNV-1a prime
        }
    }
    return hash;
}

template<typename T>
class PidCache {
public:
    void update(const std::set<int>& pids) {
        std::lock_guard<std::mutex> lock(mutex_);
        pids_ = pids;
    }

    const std::set<int>& get() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return pids_;
    }

    void remove_pid(int pid) {
        std::lock_guard<std::mutex> lock(mutex_);
        pids_.erase(pid);
    }

    bool has_pid(int pid) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return pids_.count(pid) > 0;
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        pids_.clear();
    }

private:
    mutable std::mutex mutex_;
    std::set<int> pids_;
};

using PinnedCache = PidCache<void>;
using TopForeCache = PidCache<void>;

void scan_and_update_rule_cache(ThreadMatcher& matcher,
                                std::set<int>& pinned_pids,
                                std::set<int>& topfore_pids,
                                std::set<int>& dead_pids) {
    auto all_pids = FileUtils::list_pids();
    
    for (int pid : all_pids) {
        std::string proc_name = FileUtils::get_process_name(pid);
        if (proc_name == "[dead]") {
            dead_pids.insert(pid);
            continue;
        }

        std::string cmdline = FileUtils::read_file("/proc/" + std::to_string(pid) + "/cmdline");
        if (!cmdline.empty()) {
            size_t pos = cmdline.find('\0');
            if (pos != std::string::npos) cmdline = cmdline.substr(0, pos);
        }

        MatchResult result = matcher.match_process_only(proc_name, proc_name, ProcessState::BG, pid, cmdline);

        if (!result.matched) continue;

        if (result.matched_rule_name == "Default rule") {
            continue;
        }

        if (result.pinned) {
            pinned_pids.insert(pid);
        }
        if (result.topfore && result.effective_state == ProcessState::TOP) {
            topfore_pids.insert(pid);
        }
    }
}

inline void dispatch_pids_to_workers(
                                std::vector<std::unique_ptr<ScanWorker>>& workers,
                                const std::set<int>& pids,
                                ProcessState state, const std::string& cpuset_base,
                                std::set<int>& processed_tids,
                                bool skip_topapp_cgroup = false) {
    for (int pid : pids) {
        int worker_idx = pid % workers.size();
        
        std::string proc_name = FileUtils::get_process_name(pid);
        if (proc_name == "[dead]") continue;

        std::string cmdline = FileUtils::read_file("/proc/" + std::to_string(pid) + "/cmdline");
        if (!cmdline.empty()) {
            size_t pos = cmdline.find('\0');
            if (pos != std::string::npos) cmdline = cmdline.substr(0, pos);
        }

        if (skip_topapp_cgroup) {
            std::string cgroup = FileUtils::get_cgroup_path_cached(pid, "cpuset");
            if (cgroup.find("top-app") != std::string::npos) continue;
        }

        auto tids = FileUtils::list_tids(pid);
        for (int tid : tids) {
            if (processed_tids.count(tid) > 0) continue;

            std::string thread_name = FileUtils::get_thread_name(pid, tid);

            DispatchTask task;
            task.pid = pid;
            task.tid = tid;
            task.thread_name = thread_name;
            task.state = state;
            task.cpuset_base = cpuset_base;
            task.proc_name = proc_name;
            task.cmdline = cmdline;

            workers[worker_idx]->enqueue(task);

            processed_tids.insert(tid);
        }
    }
}

inline void dispatch_pinned_to_workers(std::vector<std::unique_ptr<ScanWorker>>& workers,
                                  const std::set<int>& pids,
                                  std::set<int>& processed_tids) {
    static const std::string CPUSET_BASE = "/dev/cpuset/ReUperf";
    dispatch_pids_to_workers(workers, pids, ProcessState::TOP, CPUSET_BASE, processed_tids, false);
}

inline void dispatch_topfore_to_workers(std::vector<std::unique_ptr<ScanWorker>>& workers,
                                   const std::set<int>& pids,
                                   std::set<int>& processed_tids) {
    static const std::string CPUSET_BASE = "/dev/cpuset/ReUperf";
    dispatch_pids_to_workers(workers, pids, ProcessState::FG, CPUSET_BASE, processed_tids, true);
}

inline void dispatch_fg_top_to_workers(std::vector<std::unique_ptr<ScanWorker>>& workers,
                                   ProcessScanner& scanner,
                                   std::set<int>& processed_tids) {
    auto processes = scanner.scan_fg_top();
    if (processes.empty()) return;

    static const std::string CPUSET_BASE = "/dev/cpuset/ReUperf";

    for (const auto& info : processes) {
        int worker_idx = info.pid % workers.size();
        
        std::string cmdline = FileUtils::read_file("/proc/" + std::to_string(info.pid) + "/cmdline");
        if (!cmdline.empty()) {
            size_t pos = cmdline.find('\0');
            if (pos != std::string::npos) cmdline = cmdline.substr(0, pos);
        }

        for (int tid : info.tids) {
            auto it = info.thread_names.find(tid);
            if (it == info.thread_names.end()) continue;
            const std::string& thread_name = it->second;

            DispatchTask task;
            task.pid = info.pid;
            task.tid = tid;
            task.thread_name = thread_name;
            task.state = info.state;
            task.cpuset_base = CPUSET_BASE;
            task.proc_name = info.name;
            task.cmdline = cmdline;

            workers[worker_idx]->enqueue(task);

            processed_tids.insert(tid);
        }
    }
}

void cleanup_dead_pids(ThreadCache& cache, PinnedCache& pinned_cache,
                       TopForeCache& topfore_cache, std::set<int>& dead_pids,
                       std::set<int>& pinned_pids, std::set<int>& topfore_pids) {
    for (int pid : dead_pids) {
        cache.reset_for_pid(pid);
        pinned_cache.remove_pid(pid);
        topfore_cache.remove_pid(pid);
        pinned_pids.erase(pid);
        topfore_pids.erase(pid);
    }
}

int main(int /*argc*/, char* argv[]) {
    // Whitelist config path - only allow files under /data/adb/ReUperf/
    std::string config_path = "/data/adb/ReUperf/ReUperf.json";
    std::string allowed_dir = "/data/adb/ReUperf";
    
    if (argv[1] != nullptr) {
        std::string user_path = argv[1];
        
        // Security: validate path is within allowed directory
        if (user_path.find(allowed_dir) == 0 && 
            user_path.find("..") == std::string::npos) {
            config_path = user_path;
        } else {
            std::cerr << "Security: Config path must be under " << allowed_dir << std::endl;
            return 1;
        }
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
    auto scanner = std::make_unique<ProcessScanner>();
    auto cpuset = std::make_unique<CpusetSetter>(*matcher);
    auto prio = std::make_unique<PrioritySetter>(*matcher);
    auto cpuctl = std::make_unique<CpuctlSetter>();
    auto cache = std::make_unique<ThreadCache>();
    auto pinned_cache = std::make_unique<PinnedCache>();
    auto topfore_cache = std::make_unique<TopForeCache>();
    
    LOG_I("Main", "Initial scan...");
    
    std::vector<std::unique_ptr<ScanWorker>> workers;
    workers.push_back(std::make_unique<ScanWorker>("ScanWorker1"));
    workers.push_back(std::make_unique<ScanWorker>("ScanWorker2"));
    workers.push_back(std::make_unique<ScanWorker>("ScanWorker3"));
    workers.push_back(std::make_unique<ScanWorker>("ScanWorker4"));
    
    std::shared_ptr<ThreadMatcher> matcher_ptr(matcher.get(), [](ThreadMatcher*){});
    std::shared_ptr<CpusetSetter> cpuset_ptr(cpuset.get(), [](CpusetSetter*){});
    std::shared_ptr<PrioritySetter> prio_ptr(prio.get(), [](PrioritySetter*){});
    std::shared_ptr<CpuctlSetter> cpuctl_ptr(cpuctl.get(), [](CpuctlSetter*){});
    std::shared_ptr<ThreadCache> cache_ptr(cache.get(), [](ThreadCache*){});
    
    for (auto& w : workers) {
        w->set_configs(matcher_ptr, cpuset_ptr, prio_ptr, cpuctl_ptr, cache_ptr);
        w->start();
    }
    
    {
        std::set<int> dead_pids;
        std::set<int> pinned_pids;
        std::set<int> topfore_pids;
        
        scan_and_update_rule_cache(*matcher, pinned_pids, topfore_pids, dead_pids);
        pinned_cache->update(pinned_pids);
        topfore_cache->update(topfore_pids);
        cleanup_dead_pids(*cache, *pinned_cache, *topfore_cache, dead_pids, pinned_pids, topfore_pids);
        
        std::set<int> processed;
        dispatch_pinned_to_workers(workers, pinned_pids, processed);
        dispatch_topfore_to_workers(workers, topfore_pids, processed);
        dispatch_fg_top_to_workers(workers, *scanner, processed);
        
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    
    ProcMonitor monitor;
    monitor.start([&](int pid) {
        if (pid > 0) {
            LOG_D("Main", "Process created: " + std::to_string(pid));
        } else if (pid < 0) {
            LOG_D("Main", "Process exited: " + std::to_string(-pid));
        }
        cgroup_changed.store(true, std::memory_order_release);
    });
    
    time_t last_mtime = get_file_mtime(config_path);
    uint64_t last_config_hash = compute_config_hash(config_path);
    LOG_I("Main", "Initial config hash: " + std::to_string(last_config_hash));
    
    int highspeed_ms = std::max(config.sched.highspeed_sched_ms > 0 ? config.sched.highspeed_sched_ms : 100, 1);
    int refresh_ms = std::max(config.sched.refresh_interval_ms > 0 ? config.sched.refresh_interval_ms : 1000, highspeed_ms);
    int highspeed_loops = refresh_ms / highspeed_ms;
    
    LOG_I("Main", "Dual-loop scheduler: highspeed=" + std::to_string(highspeed_ms) 
          + "ms, refresh=" + std::to_string(refresh_ms) 
          + "ms (" + std::to_string(highspeed_loops) + " cycles)");
    
    int loop_counter = 0;
    
    while (running) {
        try {
        time_t current_mtime = get_file_mtime(config_path);
        uint64_t current_hash = compute_config_hash(config_path);
        
        if ((current_mtime != last_mtime && current_mtime > 0) || 
            (current_hash != last_config_hash && current_hash != 0)) {
            last_mtime = current_mtime;
            last_config_hash = current_hash;
            
            LOG_I("Main", "Config file changed (mtime=" + std::to_string(current_mtime) 
                  + ", hash=" + std::to_string(current_hash) + "), reloading...");
            
            try {
                Config new_config = ConfigParser::parse(config_path);
                
                if (new_config.sched.enable) {
                    config = new_config;
                    
                    LogLevel new_level = parse_log_level(config.sched.log.level);
                    Logger::instance().set_level(new_level);
                    LOG_I("Main", "Log level updated to: " + config.sched.log.level);
                    
                    config.launcher_package = LauncherFinder::find();
                    if (!CgroupInitializer::init(config)) {
                        LOG_E("Main", "Cgroup initialization failed");
                    }
                    
                    matcher = std::make_unique<ThreadMatcher>(config);
                    scanner = std::make_unique<ProcessScanner>();
                    cpuset = std::make_unique<CpusetSetter>(*matcher);
                    prio = std::make_unique<PrioritySetter>(*matcher);
                    cpuctl = std::make_unique<CpuctlSetter>();
                    cache->clear();
                    pinned_cache->clear();
                    topfore_cache->clear();
                    
                    std::shared_ptr<ThreadMatcher> matcher_ptr(matcher.get(), [](ThreadMatcher*){});
                    std::shared_ptr<CpusetSetter> cpuset_ptr(cpuset.get(), [](CpusetSetter*){});
                    std::shared_ptr<PrioritySetter> prio_ptr(prio.get(), [](PrioritySetter*){});
                    std::shared_ptr<CpuctlSetter> cpuctl_ptr(cpuctl.get(), [](CpuctlSetter*){});
                    std::shared_ptr<ThreadCache> cache_ptr(cache.get(), [](ThreadCache*){});
                    
                    for (auto& w : workers) {
                        w->stop();
                    }
                    workers.clear();
                    workers.push_back(std::make_unique<ScanWorker>("ScanWorker1"));
                    workers.push_back(std::make_unique<ScanWorker>("ScanWorker2"));
                    workers.push_back(std::make_unique<ScanWorker>("ScanWorker3"));
                    workers.push_back(std::make_unique<ScanWorker>("ScanWorker4"));
                    for (auto& w : workers) {
                        w->set_configs(matcher_ptr, cpuset_ptr, prio_ptr, cpuctl_ptr, cache_ptr);
                        w->start();
                    }
                    
                    highspeed_ms = std::max(config.sched.highspeed_sched_ms > 0 ? config.sched.highspeed_sched_ms : 100, 1);
                    refresh_ms = std::max(config.sched.refresh_interval_ms > 0 ? config.sched.refresh_interval_ms : 1000, highspeed_ms);
                    highspeed_loops = refresh_ms / highspeed_ms;
                    
                    LOG_I("Main", "Config reloaded - new intervals: highspeed=" 
                          + std::to_string(highspeed_ms) + "ms, refresh=" 
                          + std::to_string(refresh_ms) + "ms");
                    
                    full_rescan_needed.store(true);
                } else {
                    LOG_I("Main", "New config has sched disabled, exiting");
                    running = false;
                }
            } catch (const std::exception& e) {
                LOG_E("Main", "Config reload failed: " + std::string(e.what())
                      + ", keeping old config");
            }
        }
        
        if (cgroup_changed.load(std::memory_order_acquire)) {
            cgroup_changed.store(false, std::memory_order_release);
            full_rescan_needed.store(true, std::memory_order_release);
            LOG_D("Main", "Cgroup change triggered full rescan");
        }
        
        loop_counter++;
        
        std::set<int> processed;
        
        if (loop_counter >= highspeed_loops || full_rescan_needed.load()) {
            loop_counter = 0;
            
            if (full_rescan_needed.load()) {
                full_rescan_needed.store(false);
                LOG_T("Main", "Full rescan cycle");
            }
            
            std::set<int> dead_pids;
            std::set<int> pinned_pids;
            std::set<int> topfore_pids;
            
            scan_and_update_rule_cache(*matcher, pinned_pids, topfore_pids, dead_pids);
            pinned_cache->update(pinned_pids);
            topfore_cache->update(topfore_pids);
            cleanup_dead_pids(*cache, *pinned_cache, *topfore_cache, dead_pids, pinned_pids, topfore_pids);
            
            dispatch_pinned_to_workers(workers, pinned_pids, processed);
            dispatch_topfore_to_workers(workers, topfore_pids, processed);
            dispatch_fg_top_to_workers(workers, *scanner, processed);
        } else {
            dispatch_pinned_to_workers(workers, pinned_cache->get(), processed);
            dispatch_topfore_to_workers(workers, topfore_cache->get(), processed);
            dispatch_fg_top_to_workers(workers, *scanner, processed);
        }
        
        std::this_thread::sleep_for(std::chrono::milliseconds(highspeed_ms));
        
        for (auto& w : workers) {
            (void)w;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        } catch (const std::exception& e) {
            LOG_E("Main", "Exception in main loop: " + std::string(e.what()));
        } catch (...) {
            LOG_E("Main", "Unknown exception in main loop");
        }
    }
    
    monitor.stop();
    
    for (auto& w : workers) {
        w->stop();
    }
    
    if (shutdown_requested.load(std::memory_order_seq_cst)) {
        LOG_I("Main", "ReUperf Thread Scheduler stopped (signal)");
    } else {
        LOG_I("Main", "ReUperf Thread Scheduler stopped");
    }
    return 0;
}
