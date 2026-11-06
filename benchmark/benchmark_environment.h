#pragma once

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <pthread.h>
#include <sched.h>

struct CpuEnvironment {
    std::vector<int> allowed_cpus;
    std::string cpu_model;
    std::string governor;
    std::string cgroup_cpu_limit;
    double load1 = 0.0;
    double load5 = 0.0;
    double load15 = 0.0;
    double cgroup_cpu_count = -1.0;
};

inline std::string read_first_line(const char* path)
{
    std::ifstream in(path);
    std::string line;
    if (in && std::getline(in, line)) {
        return line;
    }
    return {};
}

inline std::vector<int> query_allowed_cpus()
{
    std::vector<int> cpus;
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof(set), &set) != 0) {
        return cpus;
    }
    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
        if (CPU_ISSET(cpu, &set)) {
            cpus.push_back(cpu);
        }
    }
    return cpus;
}

inline bool pin_current_thread(int cpu)
{
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    return pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0;
}

inline CpuEnvironment inspect_cpu_environment()
{
    CpuEnvironment env;
    env.allowed_cpus = query_allowed_cpus();

    std::ifstream cpuinfo("/proc/cpuinfo");
    std::string line;
    while (std::getline(cpuinfo, line)) {
        if (line.rfind("model name", 0) == 0) {
            const size_t colon = line.find(':');
            if (colon != std::string::npos) {
                const size_t start = line.find_first_not_of(" \t", colon + 1);
                env.cpu_model = start == std::string::npos ? "" : line.substr(start);
            }
            break;
        }
    }

    if (!env.allowed_cpus.empty()) {
        const std::string governor_path =
            "/sys/devices/system/cpu/cpu" +
            std::to_string(env.allowed_cpus.front()) +
            "/cpufreq/scaling_governor";
        env.governor = read_first_line(governor_path.c_str());
    }

    env.cgroup_cpu_limit = read_first_line("/sys/fs/cgroup/cpu.max");
    if (env.cgroup_cpu_limit.empty()) {
        const std::string quota =
            read_first_line("/sys/fs/cgroup/cpu/cpu.cfs_quota_us");
        const std::string period =
            read_first_line("/sys/fs/cgroup/cpu/cpu.cfs_period_us");
        if (!quota.empty()) {
            env.cgroup_cpu_limit =
                "quota=" + quota + " period=" + (period.empty() ? "?" : period);
        }
    }

    if (env.cgroup_cpu_limit.rfind("quota=", 0) == 0) {
        const size_t quota_pos = env.cgroup_cpu_limit.find('=');
        const size_t period_pos = env.cgroup_cpu_limit.find("period=");
        if (quota_pos != std::string::npos && period_pos != std::string::npos) {
            const size_t quota_start = quota_pos + 1;
            const size_t quota_end = env.cgroup_cpu_limit.find(' ', quota_start);
            const double quota = std::strtod(
                env.cgroup_cpu_limit.substr(
                    quota_start,
                    quota_end == std::string::npos
                        ? std::string::npos
                        : quota_end - quota_start).c_str(),
                nullptr);
            const double period = std::strtod(
                env.cgroup_cpu_limit.substr(period_pos + 7).c_str(), nullptr);
            if (quota > 0.0 && period > 0.0) {
                env.cgroup_cpu_count = quota / period;
            }
        }
    } else if (!env.cgroup_cpu_limit.empty()) {
        std::istringstream in(env.cgroup_cpu_limit);
        std::string quota;
        double period = 0.0;
        if (in >> quota >> period && quota != "max") {
            const double parsed_quota = std::strtod(quota.c_str(), nullptr);
            if (parsed_quota > 0.0 && period > 0.0) {
                env.cgroup_cpu_count = parsed_quota / period;
            }
        }
    }

    double load[3] = {0.0, 0.0, 0.0};
    if (getloadavg(load, 3) == 3) {
        env.load1 = load[0];
        env.load5 = load[1];
        env.load15 = load[2];
    }
    return env;
}

inline std::string format_cpu_list(const std::vector<int>& cpus, size_t max_count)
{
    std::ostringstream out;
    const size_t count = std::min(cpus.size(), max_count);
    for (size_t i = 0; i < count; ++i) {
        if (i != 0) {
            out << ',';
        }
        out << cpus[i];
    }
    if (cpus.size() > max_count) {
        out << ",...";
    }
    return out.str();
}

inline void print_cpu_environment(
    const CpuEnvironment& env, int required_cpus, bool strict)
{
    std::cout << "=== environment ===\n";
    std::cout << "cpu_model=" << (env.cpu_model.empty() ? "unknown" : env.cpu_model)
              << "\n";
    std::cout << "allowed_cpus=" << env.allowed_cpus.size()
              << " list=" << format_cpu_list(env.allowed_cpus, 16) << "\n";
    std::cout << "required_cpus=" << required_cpus
              << " per_thread_affinity="
              << (env.allowed_cpus.empty() ? "unavailable" : "enabled") << "\n";
    std::cout << std::fixed << std::setprecision(2)
              << "loadavg=" << env.load1 << '/' << env.load5 << '/' << env.load15
              << "\n";
    std::cout << "governor="
              << (env.governor.empty() ? "unavailable" : env.governor) << "\n";
    std::cout << "cgroup_cpu_limit="
              << (env.cgroup_cpu_limit.empty() ? "unavailable"
                                                : env.cgroup_cpu_limit);
    if (env.cgroup_cpu_count > 0.0) {
        std::cout << " (~" << env.cgroup_cpu_count << " CPUs)";
    }
    std::cout << "\n";

    if (static_cast<int>(env.allowed_cpus.size()) < required_cpus) {
        std::cout << "WARNING: fewer than " << required_cpus
                  << " CPUs are available; some worker threads will share a CPU\n";
    }
    if (env.cgroup_cpu_count > 0.0 &&
        env.cgroup_cpu_count + 0.01 < static_cast<double>(required_cpus)) {
        std::cout << "WARNING: cgroup CPU quota is below the required thread count\n";
    }
    if (env.governor == "powersave") {
        std::cout << "WARNING: CPU governor is powersave; frequency may be unstable\n";
    }
    if (env.load1 > static_cast<double>(required_cpus) * 0.5) {
        std::cout << "WARNING: one-minute load average is high relative to the "
                     "benchmark thread count\n";
    }
    std::cout << "strict_env=" << (strict ? "enabled" : "disabled") << "\n";
}

inline bool cpu_environment_is_suitable(
    const CpuEnvironment& env, int required_cpus)
{
    if (static_cast<int>(env.allowed_cpus.size()) < required_cpus) {
        return false;
    }
    if (env.cgroup_cpu_count > 0.0 &&
        env.cgroup_cpu_count + 0.01 < static_cast<double>(required_cpus)) {
        return false;
    }
    return true;
}
