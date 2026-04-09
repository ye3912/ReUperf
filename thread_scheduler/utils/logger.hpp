#ifndef LOGGER_HPP
#define LOGGER_HPP

#include <string>
#include <fstream>
#include <mutex>
#include <ctime>
#include <sstream>
#include <iostream>
#include <chrono>
#include <iomanip>

enum class LogLevel {
    ERR,
    WARN,
    INFO,
    DEBUG,
    TRACE
};

class Logger {
public:
    static Logger& instance() {
        static Logger inst;
        return inst;
    }

    void init(LogLevel level, const std::string& log_file, bool console_output = true) {
        std::lock_guard<std::mutex> lock(mutex_);
        level_ = level;
        log_file_ = log_file;
        console_output_ = console_output;
        
        if (file_.is_open()) {
            file_.close();
        }
        
        if (!log_file_.empty()) {
            file_.open(log_file_, std::ios::app);
        }
    }

    void set_level(LogLevel level) {
        std::lock_guard<std::mutex> lock(mutex_);
        level_ = level;
    }

    LogLevel get_level() const {
        return level_;
    }

    void log(LogLevel level, const std::string& tag, const std::string& msg) {
        if (level > level_) return;

        std::lock_guard<std::mutex> lock(mutex_);
        std::string level_str;
        switch (level) {
            case LogLevel::ERR:   level_str = "E"; break;
            case LogLevel::WARN:  level_str = "W"; break;
            case LogLevel::INFO:  level_str = "I"; break;
            case LogLevel::DEBUG: level_str = "D"; break;
            case LogLevel::TRACE: level_str = "T"; break;
        }

        auto now = std::chrono::system_clock::now();
        auto time = std::chrono::system_clock::to_time_t(now);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()) % 1000;
        std::tm tm_buf;
        localtime_r(&time, &tm_buf);

        std::ostringstream oss;
        oss << "[" << std::put_time(&tm_buf, "%H:%M:%S") << "." 
            << std::setfill('0') << std::setw(3) << ms.count() << "]"
            << "[" << level_str << "] " << tag << ": " << msg;

        std::string line = oss.str();

        if (console_output_) {
            std::cerr << line << std::endl;
        }

        if (file_.is_open()) {
            file_ << line << std::endl;
            file_.flush();
        }
    }

    void e(const std::string& tag, const std::string& msg) { log(LogLevel::ERR, tag, msg); }
    void w(const std::string& tag, const std::string& msg) { log(LogLevel::WARN, tag, msg); }
    void i(const std::string& tag, const std::string& msg) { log(LogLevel::INFO, tag, msg); }
    void d(const std::string& tag, const std::string& msg) { log(LogLevel::DEBUG, tag, msg); }
    void t(const std::string& tag, const std::string& msg) { log(LogLevel::TRACE, tag, msg); }

private:
    Logger() : level_(LogLevel::INFO), log_file_(""), console_output_(true) {}
    ~Logger() { if (file_.is_open()) file_.close(); }
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    LogLevel level_;
    std::string log_file_;
    bool console_output_;
    std::ofstream file_;
    std::mutex mutex_;
};

#define LOG_E(tag, msg) Logger::instance().e(tag, msg)
#define LOG_W(tag, msg) Logger::instance().w(tag, msg)
#define LOG_I(tag, msg) Logger::instance().i(tag, msg)
#define LOG_D(tag, msg) Logger::instance().d(tag, msg)
#define LOG_T(tag, msg) Logger::instance().t(tag, msg)

#endif