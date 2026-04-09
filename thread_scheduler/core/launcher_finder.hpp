#ifndef LAUNCHER_FINDER_HPP
#define LAUNCHER_FINDER_HPP

#include <string>
#include <array>
#include <cstdio>
#include <chrono>
#include <thread>
#include <csignal>
#include <unistd.h>
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
            LOG_I("LauncherFinder", "Found launcher via mFocusedActivity: " + result);
            return result;
        }
        
        result = execute_command(
            "dumpsys package 2>/dev/null | grep -A 5 'android.intent.category.HOME' | "
            "grep 'packageName' | head -n 1 | sed 's/.*packageName=//'",
            5000);
        
        if (!result.empty()) {
            LOG_I("LauncherFinder", "Found launcher via HOME category: " + result);
            return result;
        }
        
        result = "com.android.launcher3";
        LOG_W("LauncherFinder", "Could not find launcher, using default: " + result);
        return result;
    }

private:
    static std::string execute_command(const std::string& cmd, int timeout_ms) {
        std::array<char, 256> buffer;
        std::string result;
        
        FILE* pipe = popen(cmd.c_str(), "r");
        if (!pipe) {
            LOG_E("LauncherFinder", "popen failed for: " + cmd);
            return "";
        }
        
        auto start = std::chrono::steady_clock::now();
        
        while (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
            result += buffer.data();
            
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count();
            if (elapsed > timeout_ms) {
                LOG_W("LauncherFinder", "Command timed out after "
                      + std::to_string(timeout_ms) + "ms, killing");
                kill_child(pipe);
                return "";
            }
        }
        
        pclose(pipe);
        
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
            // 终止整个进程组
            kill(-pgid, SIGTERM);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            kill(-pgid, SIGKILL);
        }
        // pclose 会等待子进程结束
        pclose(pipe);
    }
};

#endif
