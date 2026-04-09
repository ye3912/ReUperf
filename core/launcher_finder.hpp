#ifndef LAUNCHER_FINDER_HPP
#define LAUNCHER_FINDER_HPP

#include <string>
#include <vector>
#include <cstdio>
#include <chrono>
#include <thread>
#include <csignal>
#include <unistd.h>
#include <errno.h>
#include "../utils/logger.hpp"

class LauncherFinder {
public:
    static std::string find() {
        LOG_I("LauncherFinder", "Finding launcher package...");
        
        std::string result = execute_command(
            "dumpsys activity activities 2>/dev/null | grep 'mFocusedActivity' | "
            "head -n 1 | sed -n 's/.*\\([a-z][a-z0-9_]*\\/[a-z][a-z0-9_.]*\\).*/\\1/p'",
            5000);
        
        if (!result.empty()) {
            size_t pos = result.find('/');
            if (pos != std::string::npos) {
                result = result.substr(0, pos);
            }
            if (result.empty() || result.length() > 128 || result.find(' ') != std::string::npos) {
                LOG_W("LauncherFinder", "Invalid package name format, using default");
                return "com.android.launcher3";
            }
            LOG_I("LauncherFinder", "Found launcher via mFocusedActivity: " + result);
            return result;
        }
        
        result = execute_command(
            "dumpsys package 2>/dev/null | grep -A 5 'android.intent.category.HOME' | "
            "grep 'packageName' | head -n 1 | sed 's/.*packageName=//'",
            5000);
        
        if (!result.empty()) {
            if (result.length() > 128 || result.find(' ') != std::string::npos) {
                LOG_W("LauncherFinder", "Invalid package name format, using default");
                return "com.android.launcher3";
            }
            LOG_I("LauncherFinder", "Found launcher via HOME category: " + result);
            return result;
        }
        
        result = "com.android.launcher3";
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
