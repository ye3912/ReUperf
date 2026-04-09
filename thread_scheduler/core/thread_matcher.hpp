#ifndef THREAD_MATCHER_HPP
#define THREAD_MATCHER_HPP

#include <string>
#include <regex>
#include <vector>
#include <optional>
#include "../config/config_types.hpp"
#include "../utils/logger.hpp"

struct MatchResult {
    bool matched = false;
    std::string affinity_class;
    std::string prio_class;
    std::optional<int> uclamp_max;
    std::optional<int> cpu_share;
    bool enable_limit = false;
    ProcessState effective_state;
    std::string matched_rule_name;
};

struct CompiledThreadRule {
    ThreadRule rule;
    std::regex pattern;
};

struct CompiledProcessRule {
    ProcessRule rule;
    std::regex pattern;
    std::vector<CompiledThreadRule> thread_rules;
};

class ThreadMatcher {
public:
    ThreadMatcher(const Config& config) : config_(config) {
        for (const auto& rule : config_.sched.rules) {
            CompiledProcessRule cr;
            cr.rule = rule;

            std::string expanded = expand_process_regex(rule.regex_str,
                config_.launcher_package, "");
            try {
                cr.pattern = std::regex(expanded);
            } catch (const std::regex_error&) {
                LOG_W("ThreadMatcher", "Skip invalid process regex: " + rule.regex_str);
                continue;
            }

            for (const auto& tr : rule.thread_rules) {
                CompiledThreadRule ctr;
                ctr.rule = tr;

                std::string tk = expand_thread_regex(tr.keyword, "", "");
                try {
                    ctr.pattern = std::regex(tk);
                } catch (const std::regex_error&) {
                    LOG_W("ThreadMatcher", "Skip invalid thread regex: " + tr.keyword);
                    continue;
                }

                cr.thread_rules.push_back(std::move(ctr));
            }

            compiled_rules_.push_back(std::move(cr));
        }
    }

    MatchResult match(const std::string& proc_name, const std::string& thread_name,
                      ProcessState actual_state, int pid) {
        MatchResult result;
        result.effective_state = actual_state;

        for (const auto& cr : compiled_rules_) {
            if (!std::regex_search(proc_name, cr.pattern)) {
                continue;
            }

            result.matched = true;
            result.matched_rule_name = cr.rule.name;

            ProcessState effective_state = actual_state;
            if (cr.rule.pinned) {
                effective_state = ProcessState::TOP;
            } else if (cr.rule.topfore && actual_state == ProcessState::FG) {
                effective_state = ProcessState::TOP;
            }
            result.effective_state = effective_state;

            for (const auto& ctr : cr.thread_rules) {
                if (ctr.rule.keyword == "/MAIN_THREAD/") {
                    // /MAIN_THREAD/ 匹配进程名（主线程名通常等于进程名）
                    if (thread_name == proc_name) {
                        result.affinity_class = ctr.rule.affinity_class;
                        result.prio_class = ctr.rule.prio_class;
                        result.uclamp_max = ctr.rule.uclamp_max;
                        result.cpu_share = ctr.rule.cpu_share;
                        result.enable_limit = ctr.rule.enable_limit;
                        LOG_T("ThreadMatcher", "Thread '" + thread_name + "' matched MAIN_THREAD rule '"
                              + cr.rule.name + "' -> ac=" + ctr.rule.affinity_class
                              + ", pc=" + ctr.rule.prio_class);
                        break;
                    }
                } else {
                    if (std::regex_search(thread_name, ctr.pattern)) {
                        result.affinity_class = ctr.rule.affinity_class;
                        result.prio_class = ctr.rule.prio_class;
                        result.uclamp_max = ctr.rule.uclamp_max;
                        result.cpu_share = ctr.rule.cpu_share;
                        result.enable_limit = ctr.rule.enable_limit;
                        LOG_T("ThreadMatcher", "Thread '" + thread_name + "' matched rule '"
                              + cr.rule.name + "' -> ac=" + ctr.rule.affinity_class
                              + ", pc=" + ctr.rule.prio_class);
                        break;
                    }
                }
            }

            break;
        }

        return result;
    }

    std::vector<int> get_cpus_for_affinity(const std::string& affinity_class,
                                           ProcessState state) {
        if (affinity_class.empty() || affinity_class == "auto") {
            return {};
        }

        auto it = config_.sched.affinity.find(affinity_class);
        if (it == config_.sched.affinity.end()) {
            LOG_W("ThreadMatcher", "Unknown affinity class: " + affinity_class);
            return {};
        }

        std::string cpumask_name;
        switch (state) {
            case ProcessState::BG: cpumask_name = it->second.bg; break;
            case ProcessState::FG: cpumask_name = it->second.fg; break;
            case ProcessState::TOP: cpumask_name = it->second.top; break;
        }

        if (cpumask_name.empty()) {
            return {};
        }

        auto mask_it = config_.sched.cpumask.find(cpumask_name);
        if (mask_it == config_.sched.cpumask.end()) {
            LOG_W("ThreadMatcher", "Unknown cpumask: " + cpumask_name);
            return {};
        }

        return mask_it->second;
    }

    int get_prio_value(const std::string& prio_class, ProcessState state) {
        if (prio_class.empty() || prio_class == "auto") {
            return 0;
        }

        auto it = config_.sched.prio.find(prio_class);
        if (it == config_.sched.prio.end()) {
            LOG_W("ThreadMatcher", "Unknown prio class: " + prio_class);
            return 0;
        }

        int prio = 0;
        switch (state) {
            case ProcessState::BG: prio = it->second.bg; break;
            case ProcessState::FG: prio = it->second.fg; break;
            case ProcessState::TOP: prio = it->second.top; break;
        }

        return prio;
    }

    const std::vector<CompiledProcessRule>& compiled_rules() const {
        return compiled_rules_;
    }

private:
    const Config& config_;
    std::vector<CompiledProcessRule> compiled_rules_;

    std::string expand_process_regex(const std::string& regex_str,
                                     const std::string& launcher,
                                     const std::string& /*main_thread*/) {
        std::string result = regex_str;
        if (result == "/HOME_PACKAGE/") {
            result = "^" + launcher + "$";
        }
        return result;
    }

    std::string expand_thread_regex(const std::string& keyword,
                                    const std::string& /*launcher*/,
                                    const std::string& main_thread) {
        std::string result = keyword;
        if (result == "/MAIN_THREAD/") {
            result = "^" + main_thread + "$";
        }
        return result;
    }
};

#endif
