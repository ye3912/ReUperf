#ifndef CONFIG_PARSER_HPP
#define CONFIG_PARSER_HPP

#include <nlohmann/json.hpp>
#include <fstream>
#include <string>
#include <regex>
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
            LOG_E("ConfigParser", "Failed to open config file: " + path);
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
        cfg.refresh_interval_ms = sched.value("refresh_interval_ms", 100);
        
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
                    cpus.push_back(cpu.get<int>());
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
                scene.top = val["touch"].get<std::string>();
            } else if (val.contains("top")) {
                scene.top = val["top"].get<std::string>();
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
                scene.top = val["touch"].get<int>();
            } else if (val.contains("top")) {
                scene.top = val["top"].get<int>();
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
            pr.regex_str = rule.value("regex", ".");
            pr.pinned = rule.value("pinned", false);
            pr.topfore = rule.value("topfore", false);
            
            try {
                pr.pattern = std::regex(pr.regex_str);
            } catch (const std::regex_error& e) {
                LOG_W("ConfigParser", "Invalid regex for rule '" + pr.name + "': " + pr.regex_str);
                continue;
            }
            
            if (rule.contains("rules")) {
                for (auto& tr : rule["rules"]) {
                    ThreadRule t;
                    t.keyword = tr.value("k", ".");
                    t.affinity_class = tr.value("ac", "auto");
                    t.prio_class = tr.value("pc", "auto");
                    
                    if (tr.contains("uclamp_max")) {
                        t.uclamp_max = tr["uclamp_max"].get<int>();
                    }
                    if (tr.contains("cpu_share")) {
                        t.cpu_share = tr["cpu_share"].get<int>();
                    }
                    t.enable_limit = tr.value("enable_limit", false);
                    
                    try {
                        t.pattern = std::regex(t.keyword);
                    } catch (const std::regex_error& e) {
                        LOG_W("ConfigParser", "Invalid thread regex: " + t.keyword);
                        continue;
                    }
                    
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