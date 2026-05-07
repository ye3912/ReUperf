#ifndef CONFIG_PARSER_HPP
#define CONFIG_PARSER_HPP

#include <nlohmann/json.hpp>
#include <fstream>
#include <string>
#include "config_types.hpp"
#include "../utils/logger.hpp"
#include "../utils/cpu_mask.hpp"

using json = nlohmann::json;

class ConfigParser {
public:
    static Config parse(const std::string& path) {
        Config config;
        
        std::ifstream ifs(path);
        if (!ifs.is_open()) {
            LOG_W("ConfigParser", "Config file not found: " + path + ", using default settings");
            config.sched.enable = false;
            return config;
        }
        
        json j;
        try {
            j = json::parse(ifs);
        } catch (const json::parse_error& e) {
            LOG_E("ConfigParser", "JSON parse error: " + std::string(e.what()));
            config.sched.enable = false;
            return config;
        }
        
        parse_meta(j, config);
        parse_sched(j, config);
        
        LOG_I("ConfigParser", "Parsed config: " + config.meta_name + " by " + config.meta_author);
        return config;
    }

private:
    static void parse_meta(const json& j, Config& config) {
        if (j.contains("meta")) {
            auto& meta = j["meta"];
            config.meta_name = meta.value("name", "unknown");
            config.meta_author = meta.value("author", "unknown");
        }
    }

    static void parse_sched(const json& j, Config& config) {
        if (!j.contains("modules") || !j["modules"].contains("sched")) {
            LOG_W("ConfigParser", "No sched module found, disabling scheduler");
            config.sched.enable = false;
            return;
        }
        
        auto& sched = j["modules"]["sched"];
        auto& cfg = config.sched;
        
        cfg.enable = sched.value("enable", true);
        cfg.case_insensitive = sched.value("case_insensitive", false);
        int refresh_interval = sched.value("refresh_interval_ms", 1000);
        if (refresh_interval <= 0) {
            LOG_W("ConfigParser", "Invalid refresh_interval_ms " + std::to_string(refresh_interval) + ", using default 1000");
            refresh_interval = 1000;
        }
        cfg.refresh_interval_ms = refresh_interval;
        
        int highspeed = sched.value("highspeed_sched_ms", 100);
        if (highspeed <= 0) {
            LOG_W("ConfigParser", "Invalid highspeed_sched_ms " + std::to_string(highspeed) + ", using default 100");
            highspeed = 100;
        }
        cfg.highspeed_sched_ms = highspeed;
        
        if (sched.contains("log")) {
            auto& log = sched["log"];
            cfg.log.level = log.value("level", "info");
            cfg.log.output = log.value("output", "/data/adb/ReUperf/ReUperf.log");
        }
        
        parse_cpumask(sched, cfg);
        parse_affinity(sched, cfg);
        parse_prio(sched, cfg);
        parse_rules(sched, cfg);
    }

    static void parse_cpumask(const json& sched, SchedConfig& cfg) {
        if (!sched.contains("cpumask")) return;
        
        auto& cm = sched["cpumask"];
        for (auto it = cm.begin(); it != cm.end(); ++it) {
            std::string name = it.key();
            std::vector<int> cpus;
            if (it.value().is_array()) {
                for (auto& cpu : it.value()) {
                    try {
                        cpus.push_back(cpu.get<int>());
                    } catch (const std::exception& e) {
                        LOG_W("ConfigParser", "Invalid CPU value in cpumask[" + name + "]: " + std::string(e.what()));
                    }
                }
            }
            cfg.cpumask[name] = cpus;
            LOG_D("ConfigParser", "cpumask[" + name + "] = " + CpuMask::to_string(cpus));
        }
    }

    static void parse_affinity(const json& sched, SchedConfig& cfg) {
        if (!sched.contains("affinity")) return;
        
        auto& af = sched["affinity"];
        for (auto it = af.begin(); it != af.end(); ++it) {
            std::string name = it.key();
            AffinityScene scene;
            
            auto& val = it.value();
            scene.bg = val.value("bg", "");
            scene.fg = val.value("fg", "");
            
            if (val.contains("touch")) {
                try {
                    if (val["touch"].is_string()) {
                        scene.top = val["touch"].get<std::string>();
                    } else {
                        LOG_W("ConfigParser", "Invalid type for affinity[" + name + "].touch, expected string");
                    }
                } catch (const std::exception& e) {
                    LOG_W("ConfigParser", "Failed to parse affinity[" + name + "].touch: " + std::string(e.what()));
                }
            } else if (val.contains("top")) {
                try {
                    if (val["top"].is_string()) {
                        scene.top = val["top"].get<std::string>();
                    } else {
                        LOG_W("ConfigParser", "Invalid type for affinity[" + name + "].top, expected string");
                    }
                } catch (const std::exception& e) {
                    LOG_W("ConfigParser", "Failed to parse affinity[" + name + "].top: " + std::string(e.what()));
                }
            }
            
            cfg.affinity[name] = scene;
            LOG_D("ConfigParser", "affinity[" + name + "]: bg=" + scene.bg 
                  + ", fg=" + scene.fg + ", top=" + scene.top);
        }
    }

    static void parse_prio(const json& sched, SchedConfig& cfg) {
        if (!sched.contains("prio")) return;
        
        auto& pr = sched["prio"];
        for (auto it = pr.begin(); it != pr.end(); ++it) {
            std::string name = it.key();
            PrioScene scene;
            
            auto& val = it.value();
            scene.bg = val.value("bg", 0);
            scene.fg = val.value("fg", 0);
            
            if (val.contains("touch")) {
                try {
                    if (val["touch"].is_number()) {
                        scene.top = val["touch"].get<int>();
                    } else {
                        LOG_W("ConfigParser", "Invalid type for prio[" + name + "].touch, expected number");
                    }
                } catch (const std::exception& e) {
                    LOG_W("ConfigParser", "Failed to parse prio[" + name + "].touch: " + std::string(e.what()));
                }
            } else if (val.contains("top")) {
                try {
                    if (val["top"].is_number()) {
                        scene.top = val["top"].get<int>();
                    } else {
                        LOG_W("ConfigParser", "Invalid type for prio[" + name + "].top, expected number");
                    }
                } catch (const std::exception& e) {
                    LOG_W("ConfigParser", "Failed to parse prio[" + name + "].top: " + std::string(e.what()));
                }
            }
            
            cfg.prio[name] = scene;
            LOG_D("ConfigParser", "prio[" + name + "]: bg=" + std::to_string(scene.bg)
                  + ", fg=" + std::to_string(scene.fg) + ", top=" + std::to_string(scene.top));
        }
    }

    static void parse_rules(const json& sched, SchedConfig& cfg) {
        if (!sched.contains("rules")) return;
        
        auto& rules = sched["rules"];
        for (auto& rule : rules) {
            ProcessRule pr;
            pr.name = rule.value("name", "");
            pr.regex_str = rule.value("regex", "^$");
            pr.comm_regex_str = rule.value("comm_regex", "^$");
            pr.pinned = rule.value("pinned", false);
            pr.topfore = rule.value("topfore", false);
            
            if (rule.contains("rules")) {
                for (auto& tr : rule["rules"]) {
                    ThreadRule t;
                    t.keyword = tr.value("k", ".");
                    t.affinity_class = tr.value("ac", "auto");
                    t.prio_class = tr.value("pc", "auto");
                    
                    if (tr.contains("uclamp_max")) {
                        try {
                            if (tr["uclamp_max"].is_number()) {
                                int val = tr["uclamp_max"].get<int>();
                                if (val >= 0 && val <= 100) {
                                    t.uclamp_max = val;
                                } else {
                                    LOG_W("ConfigParser", "Invalid uclamp_max " + std::to_string(val) 
                                          + " for rule[" + pr.name + "], expected 0-100");
                                }
                            } else {
                                LOG_W("ConfigParser", "Invalid type for rule[" + pr.name + "].uclamp_max, expected number");
                            }
                        } catch (const std::exception& e) {
                            LOG_W("ConfigParser", "Failed to parse rule[" + pr.name + "].uclamp_max: " + std::string(e.what()));
                        }
                    }
                    if (tr.contains("cpu_share")) {
                        try {
                            if (tr["cpu_share"].is_number()) {
                                int val = tr["cpu_share"].get<int>();
                                // Android-specific: uses 0-1024 instead of standard cgroup 2-262144
                                if (val >= 0 && val <= 1024) {
                                    t.cpu_share = val;
                                } else {
                                    LOG_W("ConfigParser", "Invalid cpu_share " + std::to_string(val) 
                                          + " for rule[" + pr.name + "], expected 0-1024 (Android-specific)");
                                }
                            } else {
                                LOG_W("ConfigParser", "Invalid type for rule[" + pr.name + "].cpu_share, expected number");
                            }
                        } catch (const std::exception& e) {
                            LOG_W("ConfigParser", "Failed to parse rule[" + pr.name + "].cpu_share: " + std::string(e.what()));
                        }
                    }
                    t.enable_limit = tr.value("enable_limit", false);
                    
                    pr.thread_rules.push_back(t);
                }
            }
            
            cfg.rules.push_back(pr);
            LOG_D("ConfigParser", "Added rule: " + pr.name + " (pinned=" 
                  + std::to_string(pr.pinned) + ", topfore=" + std::to_string(pr.topfore)
                  + ", threads=" + std::to_string(pr.thread_rules.size()));
        }
    }

};

#endif