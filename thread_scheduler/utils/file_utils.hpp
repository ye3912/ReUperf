#ifndef FILE_UTILS_HPP
#define FILE_UTILS_HPP

#include <string>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <dirent.h>
#include <fcntl.h>
#include <cstring>
#include <vector>
#include "logger.hpp"

namespace FileUtils {

inline bool file_exists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

inline bool dir_exists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

inline bool mkdir_recursive(const std::string& path) {
    if (dir_exists(path)) return true;
    
    size_t pos = 0;
    std::string dir;
    while ((pos = path.find('/', pos + 1)) != std::string::npos) {
        dir = path.substr(0, pos);
        if (!dir.empty() && !dir_exists(dir)) {
            if (mkdir(dir.c_str(), 0755) != 0 && errno != EEXIST) {
                LOG_E("FileUtils", "mkdir failed: " + dir + " (" + std::string(strerror(errno)) + ")");
                return false;
            }
        }
    }
    
    if (!dir_exists(path)) {
        if (mkdir(path.c_str(), 0755) != 0 && errno != EEXIST) {
            LOG_E("FileUtils", "mkdir failed: " + path + " (" + std::string(strerror(errno)) + ")");
            return false;
        }
    }
    
    return true;
}

inline bool write_file(const std::string& path, const std::string& content) {
    int fd = open(path.c_str(), O_WRONLY | O_TRUNC);
    if (fd < 0) {
        LOG_E("FileUtils", "write_file open failed: " + path + " (" + std::string(strerror(errno)) + ")");
        return false;
    }
    
    ssize_t written = 0;
    ssize_t total = static_cast<ssize_t>(content.size());
    const char* buf = content.c_str();
    
    while (written < total) {
        ssize_t ret = write(fd, buf + written, total - written);
        if (ret < 0) {
            LOG_E("FileUtils", "write failed: " + path + " (" + std::string(strerror(errno)) + ")");
            close(fd);
            return false;
        }
        written += ret;
    }
    
    close(fd);
    return true;
}

inline std::string read_file(const std::string& path) {
    std::ifstream ifs(path);
    if (!ifs.is_open()) {
        return "";
    }
    std::stringstream ss;
    ss << ifs.rdbuf();
    std::string content = ss.str();
    while (!content.empty() && content.back() == '\n') {
        content.pop_back();
    }
    return content;
}

inline std::vector<int> list_pids() {
    std::vector<int> pids;
    DIR* dir = opendir("/proc");
    if (!dir) return pids;
    
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_type == DT_DIR) {
            int pid = atoi(entry->d_name);
            if (pid > 0) {
                pids.push_back(pid);
            }
        }
    }
    closedir(dir);
    return pids;
}

inline std::vector<int> list_tids(int pid) {
    std::vector<int> tids;
    std::string task_path = "/proc/" + std::to_string(pid) + "/task";
    DIR* dir = opendir(task_path.c_str());
    if (!dir) return tids;
    
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_type == DT_DIR) {
            int tid = atoi(entry->d_name);
            if (tid > 0) {
                tids.push_back(tid);
            }
        }
    }
    closedir(dir);
    return tids;
}

inline std::string get_process_name(int pid) {
    std::string cmdline = read_file("/proc/" + std::to_string(pid) + "/cmdline");
    if (!cmdline.empty()) {
        size_t pos = cmdline.find('\0');
        if (pos != std::string::npos) cmdline = cmdline.substr(0, pos);
        pos = cmdline.rfind('/');
        if (pos != std::string::npos) cmdline = cmdline.substr(pos + 1);
        return cmdline;
    }
    return read_file("/proc/" + std::to_string(pid) + "/comm");
}

inline std::string get_thread_name(int pid, int tid) {
    return read_file("/proc/" + std::to_string(pid) + "/task/" + std::to_string(tid) + "/comm");
}

inline std::string get_cgroup_path(int pid, const std::string& controller) {
    std::string cgroups = read_file("/proc/" + std::to_string(pid) + "/cgroup");
    std::istringstream iss(cgroups);
    std::string line;
    while (std::getline(iss, line)) {
        if (line.find(controller) != std::string::npos) {
            size_t pos = line.find(':');
            if (pos != std::string::npos) {
                return line.substr(pos + 1);
            }
        }
    }
    return "";
}

inline bool is_in_cgroup(int pid, const std::string& cgroup_name) {
    std::string path = get_cgroup_path(pid, "cpuset");
    return path.find(cgroup_name) != std::string::npos;
}

inline std::vector<int> read_cgroup_procs(const std::string& path) {
    std::vector<int> pids;
    std::string content = read_file(path + "/cgroup.procs");
    std::istringstream iss(content);
    std::string line;
    while (std::getline(iss, line)) {
        int pid = atoi(line.c_str());
        if (pid > 0) pids.push_back(pid);
    }
    return pids;
}

inline bool write_cgroup_procs(const std::string& path, int tid) {
    return write_file(path + "/cgroup.procs", std::to_string(tid));
}

}

#endif
