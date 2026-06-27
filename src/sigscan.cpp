#include "sigscan.h"

#include <windows.h>
#include <psapi.h>
#include <vector>

#pragma comment(lib, "psapi.lib")

namespace {

struct Pattern {
    std::vector<uint8_t> bytes;
    std::vector<bool>    mask;   // true = compare, false = wildcard
};

Pattern parse(const char* ida) {
    Pattern p;
    for (const char* c = ida; *c; ) {
        if (*c == ' ') { ++c; continue; }
        if (*c == '?') {
            p.bytes.push_back(0);
            p.mask.push_back(false);
            c += (c[1] == '?') ? 2 : 1;
        } else {
            p.bytes.push_back(static_cast<uint8_t>(strtoul(c, nullptr, 16)));
            p.mask.push_back(true);
            c += 2;
        }
    }
    return p;
}

uintptr_t scanRange(uintptr_t base, size_t size, const Pattern& p) {
    if (p.bytes.empty() || size < p.bytes.size()) return 0;
    auto* data = reinterpret_cast<const uint8_t*>(base);
    const size_t n = p.bytes.size();
    for (size_t i = 0; i + n <= size; ++i) {
        bool hit = true;
        for (size_t j = 0; j < n; ++j) {
            if (p.mask[j] && data[i + j] != p.bytes[j]) { hit = false; break; }
        }
        if (hit) return base + i;
    }
    return 0;
}

bool moduleRange(const char* name, uintptr_t& base, size_t& size) {
    HMODULE mod = name ? GetModuleHandleA(name) : GetModuleHandleA(nullptr);
    if (!mod) return false;
    MODULEINFO mi{};
    if (!GetModuleInformation(GetCurrentProcess(), mod, &mi, sizeof(mi)))
        return false;
    base = reinterpret_cast<uintptr_t>(mi.lpBaseOfDll);
    size = mi.SizeOfImage;
    return true;
}

}  // namespace

namespace sigscan {

uintptr_t findInModule(const char* moduleName, const char* idaPattern) {
    uintptr_t base = 0; size_t size = 0;
    if (!moduleRange(moduleName, base, size)) return 0;
    return scanRange(base, size, parse(idaPattern));
}

uintptr_t find(const char* idaPattern) {
    return findInModule(nullptr, idaPattern);
}

}  // namespace sigscan
