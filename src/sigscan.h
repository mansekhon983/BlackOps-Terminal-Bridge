#pragma once
// Pattern (signature) scanning. Prefer this over hardcoded addresses: offsets
// break on any game update, signatures usually survive. See CLAUDE.md.

#include <cstdint>
#include <string>

namespace sigscan {

// IDA-style pattern, e.g. "55 8B EC 83 EC ?? 56 57". '?' / '??' = wildcard.
// Scans the main module's executable range. Returns 0 if not found.
uintptr_t find(const char* idaPattern);

// Scan a specific module by name (e.g. "BlackOps.exe"). 0 if not found.
uintptr_t findInModule(const char* moduleName, const char* idaPattern);

}  // namespace sigscan
