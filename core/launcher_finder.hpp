#ifndef LAUNCHER_FINDER_HPP
#define LAUNCHER_FINDER_HPP

#include <string>
#include <vector>
#include <sstream>
#include <cctype>
#include <cstdio>
#include <chrono>
#include <thread>
#include <csignal>
#include <unistd.h>
#include <errno.h>
#include "../utils/logger.hpp"

class LauncherFinder {
private:
    static bool is_home_package(const std::string& pkg) {
        std::string lower;
        lower.reserve(pkg.size());
        for (char c : pkg) {
            lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        return lower.find("home") != std::string::npos || 
               lower.find("launcher") != std::string::npos;
    }

    static bool is_valid_package(const std::string& pkg) {
        return !pkg.empty() && pkg.length() <= 128 && pkg.find(' ') == std::string::npos;
    }

public:
    static std::string find() {
        LOG_I("LauncherFinder", "Finding launcher package...");
        
        std::string result = execute_command(
            "cmd package resolve-activity -a android.intent.action.MAIN "
            "-c android.intent.category.HOME 2>/dev/null | grep 'packageName='",
            5000);
        
        std::vector<std::string> candidates;
        if (!result.empty()) {
            std::istringstream ss(result);
            std::string line;
            while (std::getline(ss, line)) {
                size_t pos = line.find("packageName=");
                if (pos != std::string::npos) {
                    std::string pkg = line.substr(pos + 12);
                    while (!pkg.empty() && (pkg.back() == '\n' || pkg.back() == '\r')) {
                        pkg.pop_back();
                    }
                    if (is_valid_package(pkg)) {
                        candidates.push_back(pkg);
                    }
                }
            }
        }
        
        if (candidates.empty()) {
            result = execute_command(
                "cmd package query-activities -a android.intent.action.MAIN "
                "-c android.intent.category.HOME 2>/dev/null | grep 'packageName='",
                5000);
            
            if (!result.empty()) {
                std::istringstream ss(result);
                std::string line;
                while (std::getline(ss, line)) {
                    size_t pos = line.find("packageName=");
                    if (pos != std::string::npos) {
                        std::string pkg = line.substr(pos + 12);
                        while (!pkg.empty() && (pkg.back() == '\n' || pkg.back() == '\r')) {
                            pkg.pop_back();
                        }
                        if (is_valid_package(pkg)) {
                            candidates.push_back(pkg);
                        }
                    }
                }
            }
        }
        
        std::string non_com_android_home;
        std::string com_android_home;
        std::string other_pkg;
        
        for (const auto& pkg : candidates) {
            bool is_home = is_home_package(pkg);
            bool starts_com_android = (pkg.find("com.android") == 0);
            
            if (is_home && !starts_com_android) {
                if (non_com_android_home.empty()) {
                    non_com_android_home = pkg;
                }
            } else if (is_home && starts_com_android) {
                if (com_android_home.empty()) {
                    com_android_home = pkg;
                }
            } else if (other_pkg.empty()) {
                other_pkg = pkg;
            }
        }
        
        if (!non_com_android_home.empty()) {
            LOG_I("LauncherFinder", "Found launcher (non-com.android home): " + non_com_android_home);
            return non_com_android_home;
        }
        if (!com_android_home.empty()) {
            LOG_I("LauncherFinder", "Found launcher (com.android home): " + com_android_home);
            return com_android_home;
        }
        if (!other_pkg.empty()) {
            LOG_I("LauncherFinder", "Found launcher (other): " + other_pkg);
            return other_pkg;
        }
        
        result = "com.miui.home";
        LOG_W("LauncherFinder", "Could not find launcher, using default: " + result);
        return result;
    }

private:
    static constexpr size_t kMaxOutputSize = 64 * 1024;  // 64KB max output

    static std::string execute_command(const std::string& cmd, int timeout_ms) {
        std::string result;
        
        FILE* pipe = popen(cmd.c_str(), "r");
        if (!pipe) {
            LOG_E("LauncherFinder", "popen failed for: " + cmd);
            return "";
        }
        
        auto start = std::chrono::steady_clock::now();
        
        char buffer[4096];
        while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
            if (result.size() + strlen(buffer) > kMaxOutputSize) {
                LOG_W("LauncherFinder", "Command output exceeds " + std::to_string(kMaxOutputSize) + " bytes, truncating");
                break;
            }
            result += buffer;
            
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count();
            if (elapsed > timeout_ms) {
                LOG_W("LauncherFinder", "Command timed out after "
                      + std::to_string(timeout_ms) + "ms, killing");
                kill_child(pipe);
                return "";
            }
        }
        
        int exit_status = pclose(pipe);
        if (exit_status != 0) {
            LOG_W("LauncherFinder", "Command exited with status: " + std::to_string(exit_status));
        }
        
        while (!result.empty() && (result.back() == '\n' || result.back() == '\r')) {
            result.pop_back();
        }
        
        return result;
    }
    
    static void kill_child(FILE* pipe) {
        if (!pipe) return;
        int fd = fileno(pipe);
        // 尝试获取子进程组ID
        pid_t pgid = tcgetpgrp(fd);
        if (pgid > 0) {
            // 终止整个进程组，忽略 ESRCH（进程已退出）
            if (kill(-pgid, SIGTERM) != 0 && errno != ESRCH) {
                LOG_W("LauncherFinder", "kill SIGTERM failed: " + std::string(strerror(errno)));
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (kill(-pgid, SIGKILL) != 0 && errno != ESRCH) {
                LOG_W("LauncherFinder", "kill SIGKILL failed: " + std::string(strerror(errno)));
            }
        } else if (pgid == -1) {
            LOG_W("LauncherFinder", "tcgetpgrp failed: " + std::string(strerror(errno)));
        }
        // pclose 会等待子进程结束
        pclose(pipe);
    }
};

#endif
