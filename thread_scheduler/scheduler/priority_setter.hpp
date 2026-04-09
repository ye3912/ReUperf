#ifndef PRIORITY_SETTER_HPP
#define PRIORITY_SETTER_HPP

#include <string>
#include <sched.h>
#include <cstring>
#include <sys/resource.h>
#include "../config/config_types.hpp"
#include "../utils/logger.hpp"
#include "../core/thread_matcher.hpp"

class PrioritySetter {
public:
    PrioritySetter(const Config& /*config*/, ThreadMatcher& matcher) 
        : matcher_(matcher) {}
    
    bool set_priority(int tid, int prio_value) {
        if (prio_value == 0) {
            return true;
        }
        
        struct sched_param param;
        
        if (prio_value >= 1 && prio_value <= 98) {
            param.sched_priority = prio_value;
            if (sched_setscheduler(tid, SCHED_FIFO, &param) != 0) {
                LOG_W("PrioritySetter", "Failed to set SCHED_FIFO for tid " 
                      + std::to_string(tid) + " prio=" + std::to_string(prio_value));
                return false;
            }
            LOG_T("PrioritySetter", "Set tid " + std::to_string(tid) 
                  + " to SCHED_FIFO prio=" + std::to_string(prio_value));
        } else if (prio_value >= 100 && prio_value <= 139) {
            param.sched_priority = 0;
            if (sched_setscheduler(tid, SCHED_NORMAL, &param) != 0) {
                LOG_W("PrioritySetter", "Failed to set SCHED_NORMAL for tid " 
                      + std::to_string(tid));
                return false;
            }
            set_nice_value(tid, prio_value - 120);
            LOG_T("PrioritySetter", "Set tid " + std::to_string(tid) 
                  + " to SCHED_NORMAL nice=" + std::to_string(prio_value - 120));
        } else if (prio_value == -1) {
            param.sched_priority = 0;
            if (sched_setscheduler(tid, SCHED_NORMAL, &param) != 0) {
                LOG_W("PrioritySetter", "Failed to set SCHED_NORMAL for tid " 
                      + std::to_string(tid));
                return false;
            }
            LOG_T("PrioritySetter", "Set tid " + std::to_string(tid) + " to SCHED_NORMAL");
        } else if (prio_value == -2) {
            param.sched_priority = 0;
            if (sched_setscheduler(tid, SCHED_BATCH, &param) != 0) {
                LOG_W("PrioritySetter", "Failed to set SCHED_BATCH for tid " 
                      + std::to_string(tid));
                return false;
            }
            LOG_T("PrioritySetter", "Set tid " + std::to_string(tid) + " to SCHED_BATCH");
        } else if (prio_value == -3) {
            param.sched_priority = 0;
            if (sched_setscheduler(tid, SCHED_IDLE, &param) != 0) {
                LOG_W("PrioritySetter", "Failed to set SCHED_IDLE for tid " 
                      + std::to_string(tid));
                return false;
            }
            LOG_T("PrioritySetter", "Set tid " + std::to_string(tid) + " to SCHED_IDLE");
        }
        
        return true;
    }
    
    bool apply_with_result(int pid, int tid, const MatchResult& result) {
        if (!result.matched) {
            return true;
        }
        
        int prio = matcher_.get_prio_value(result.prio_class, result.effective_state);
        
        if (prio == 0) {
            return true;
        }
        
        return set_priority(tid, prio);
    }

private:
    ThreadMatcher& matcher_;
    
    bool set_nice_value(int tid, int nice) {
        if (setpriority(PRIO_PROCESS, tid, nice) != 0) {
            LOG_W("PrioritySetter", "Failed to set nice " + std::to_string(nice) 
                  + " for tid " + std::to_string(tid));
            return false;
        }
        LOG_T("PrioritySetter", "Set tid " + std::to_string(tid) + " nice=" + std::to_string(nice));
        return true;
    }
};

#endif
