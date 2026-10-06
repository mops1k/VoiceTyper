#include "domain/cpu_topology.hpp"

#include <thread>

namespace voicetyper::domain {

platform::CpuTopology make_cpu_topology(
    std::uint32_t logical_processors,
    std::uint32_t physical_cores,
    std::string_view source)
{
    platform::CpuTopology topology;
    topology.logical_processors = logical_processors == 0 ? 1 : logical_processors;
    if (physical_cores == 0) {
        topology.physical_cores = platform::physical_cores_fallback(topology.logical_processors);
        topology.physical_cores_known = false;
    } else {
        topology.physical_cores = physical_cores;
        topology.physical_cores_known = true;
    }
    topology.source = source;
    return topology;
}

Result<platform::CpuTopology> StandardCpuTopologyProvider::detect() const
{
    const auto logical = static_cast<std::uint32_t>(std::thread::hardware_concurrency());
    return make_cpu_topology(logical, 0, "hardware_concurrency");
}

} // namespace voicetyper::domain
