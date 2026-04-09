#ifndef PRIORITY_SETTER_HPP
#define PRIORITY_SETTER_HPP

#include <string>
#include <sched.h>
#include <cstring>
#include <sys/resource.h>
#include "../config/config_types.hpp"
#include "../utils/logger.hpp"
#include "../core/thread_matcher.hpp"

struct CurrentSched {
    int policy;
    int prio;
};

class PrioritySetter {
public:
    PrioritySetter(ThreadMatcher& matcher)
        : matcher_(matcher) {}

    CurrentSched get_current_sched(int tid) {
        CurrentSched curr;
        curr.policy = sched_getscheduler(tid);
        if (curr.policy < 0) {
            curr.policy = -1;
            curr.prio = 0;
            return curr;
        }

        struct sched_param param;
        if (sched_getparam(tid, &param) == 0) {
            curr.prio = param.sched_priority;
        } else {
            curr.prio = 0;
        }
        return curr;
    }

    bool is_sched_changed(int tid, int expected_policy, int expected_prio) {
        auto curr = get_current_sched(tid);
        if (curr.policy != expected_policy) return true;
        if (curr.policy == SCHED_FIFO || curr.policy == SCHED_RR) {
            return curr.prio != expected_prio;
        }
        return false;
    }

    bool set_priority(int tid, int prio_value) {
        if (tid <= 0) {
            LOG_W("PrioritySetter", "Invalid tid: " + std::to_string(tid));
            return false;
        }
        
        if (prio_value == 0) {
            return true;
        }
        
        struct sched_param param;
        
        if (prio_value >= 1 && prio_value <= 98) {
            param.sched_priority = prio_value;
            if (sched_setscheduler(tid, SCHED_FIFO, &param) != 0) {
                int err = errno;
                if (err == EPERM || err == EACCES) {
                    LOG_W("PrioritySetter", "Permission denied to set SCHED_FIFO for tid " 
                          + std::to_string(tid) + " (need CAP_SYS_NICE)");
                } else {
                    LOG_W("PrioritySetter", "Failed to set SCHED_FIFO for tid " 
                          + std::to_string(tid) + " prio=" + std::to_string(prio_value)
                          + ": " + std::string(strerror(err)));
                }
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
        } else {
            LOG_W("PrioritySetter", "Unknown prio_value " + std::to_string(prio_value) 
                  + " for tid " + std::to_string(tid) + ", skipping");
            return false;
        }
        
        return true;
    }
    
    bool apply_with_result(int pid, int tid, const MatchResult& result) {
        (void)pid;
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
