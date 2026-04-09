# ReUperf Thread Scheduler Memory

## Project Overview

Android thread scheduler based on uperf configuration format. Uses sched_setaffinity, cgroup cpuset/cpuctl, and sched_setscheduler to manage thread CPU affinity, scheduling priority, and resource limits.

## File Structure

```
thread_scheduler/
├── main.cpp                          # Entry point
├── CMakeLists.txt                    # CMake build config
├── config.json                       # Example config file
├── config/
│   ├── config_parser.hpp             # JSON parsing (nlohmann/json)
│   └── config_types.hpp              # Data structures
├── core/
│   ├── cgroup_init.hpp               # Create cpuset/cpuctl directories
│   ├── cpuset_monitor.hpp            # inotify monitor for cgroup changes
│   ├── launcher_finder.hpp           # dumpsys to find launcher package
│   ├── process_scanner.hpp           # /proc scanner
│   └── thread_matcher.hpp            # Regex matching + macro expansion
├── scheduler/
│   ├── cpuctl_setter.hpp             # uclamp.max / cpu.shares
│   ├── cpuset_setter.hpp             # CPU affinity + cpuset cgroup
│   └── priority_setter.hpp           # sched_setscheduler (SCHED_FIFO/NORMAL/BATCH/IDLE)
└── utils/
    ├── cpu_mask.hpp                  # cpu_set_t operations
    ├── file_utils.hpp                # File I/O, /proc parsing
    └── logger.hpp                    # Configurable logging
```

## Default Paths

| Item | Path |
|------|------|
| Config file | `/data/adb/ReUperf/ReUperf.json` |
| Log file | `/data/adb/ReUperf/ReUperf.log` |
| Data directory | `/data/adb/ReUperf` (0755) |

## Build

```bash
cd thread_scheduler
mkdir build && cd build
cmake ..
make
```

## Run

```bash
./thread_scheduler                    # Use default config path
./thread_scheduler /path/to/config.json  # Custom config
```

## Key Design Decisions

### Process States
- **bg**: Background (not in foreground/top-app cpuset)
- **fg**: Foreground (in foreground cpuset)
- **top**: Top-app (in top-app cpuset)

### State Modifiers
- `pinned=true`: Always treat as TOP state
- `topfore=true`: Treat as TOP when in FG state

### Priority Values (from uperf)
| Value | Meaning |
|-------|---------|
| 0 | Skip scheduling policy |
| 1-98 | SCHED_FIFO with priority |
| 100-139 | SCHED_NORMAL with nice (value-120) |
| -1 | SCHED_NORMAL |
| -2 | SCHED_BATCH |
| -3 | SCHED_IDLE |

### Special Regex Macros
- `/HOME_PACKAGE/`: Replaced with launcher package name
- `/MAIN_THREAD/`: Replaced with process name (main thread name)

### Cgroup Structure Created

**cpuset** (for CPU affinity):
```
/dev/cpuset/{top-app,foreground,background}/ReUperf/{cpumask_name}
```

**cpuctl** (for resource limits):
```
/dev/cpuctl/{top-app,foreground,background}/ReUperf/{rule_name}
```

## Main Loop

1. Parse config, find launcher package
2. Initialize cgroup directories
3. Scan all processes, apply rules
4. Start inotify monitor for cpuset changes
5. Loop: refresh fg/top processes every `refresh_interval_ms`

## Dependencies

- C++17
- nlohmann/json (fetched by CMake)
- Linux/Android system headers (sched.h, sys/inotify.h, etc.)

## Config File Format

Compatible with uperf v3 JSON format. See config.md for details.