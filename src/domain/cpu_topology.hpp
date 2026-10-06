#pragma once

// Portable CPU topology seam. OS-specific physical-core detection belongs in a
// platform backend; this slice provides the deterministic value/fallback rules
// that both backends must honor.

#include "platform/api/cpu_topology.hpp"

#include <cstdint>
#include <string_view>

namespace voicetyper::domain {

/// A testable topology seed. `physical == 0` means "unknown" and selects the
/// documented .NET fallback.
[[nodiscard]] platform::CpuTopology make_cpu_topology(
    std::uint32_t logical_processors,
    std::uint32_t physical_cores = 0,
    std::string_view source = "injected");

/// Portable provider used until a Windows/Arch backend is selected. It reports
/// logical hardware concurrency and explicitly marks physical cores unknown.
class StandardCpuTopologyProvider final : public platform::CpuTopologyProvider {
public:
    [[nodiscard]] Result<platform::CpuTopology> detect() const override;
};

} // namespace voicetyper::domain
