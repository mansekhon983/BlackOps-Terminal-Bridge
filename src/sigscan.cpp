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

bool matches(uintptr_t addr, const char* idaPattern) {
    if (!addr) return false;
    Pattern p = parse(idaPattern);
    if (p.bytes.empty()) return false;

    // Validate the bytes live in committed, readable memory before touching them
    // (so a wrong RVA can never crash the game -- we just report "no match").
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT) return false;
    const DWORD readable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                           PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    if ((mbi.Protect & readable) == 0) return false;
    uintptr_t regionEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    if (addr + p.bytes.size() > regionEnd) return false;  // would cross region edge

    auto* data = reinterpret_cast<const uint8_t*>(addr);
    for (size_t j = 0; j < p.bytes.size(); ++j) {
        if (p.mask[j] && data[j] != p.bytes[j]) return false;
    }
    return true;
}

}  // namespace sigscan
