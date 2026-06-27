#include "bridge.h"
#include "queues.h"

#include <windows.h>
#include <thread>
#include <chrono>

namespace bridge {

std::vector<std::string> runCommand(const std::string& command) {
    std::vector<std::string> lines;

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;          // child inherits the write end
    sa.lpSecurityDescriptor = nullptr;

    HANDLE readPipe = nullptr, writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &sa, 0)) {
        lines.push_back("[bridge] CreatePipe failed");
        return lines;
    }
    // The read end must NOT be inherited by the child.
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

    // Build: powershell.exe -NoProfile -Command "<command>"
    // NOTE: command injection is inherent here (see CLAUDE.md). Personal use only.
    std::string cmdline = "powershell.exe -NoProfile -Command \"" + command + "\"";

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = writePipe;
    si.hStdError = writePipe;
    si.hStdInput = nullptr;

    PROCESS_INFORMATION pi{};
    std::vector<char> mutableCmd(cmdline.begin(), cmdline.end());
    mutableCmd.push_back('\0');

    BOOL ok = CreateProcessA(
        nullptr, mutableCmd.data(), nullptr, nullptr,
        TRUE,                       // inherit handles
        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);

    // Parent closes its copy of the write end so the read sees EOF on exit.
    CloseHandle(writePipe);

    if (!ok) {
        CloseHandle(readPipe);
        lines.push_back("[bridge] CreateProcess failed");
        return lines;
    }

    std::string acc;
    char buf[4096];
    DWORD got = 0;
    while (ReadFile(readPipe, buf, sizeof(buf), &got, nullptr) && got > 0) {
        acc.append(buf, got);
    }
    CloseHandle(readPipe);
    WaitForSingleObject(pi.hProcess, INFINITE);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    // Split on newlines, strip trailing CR.
    size_t start = 0;
    while (start <= acc.size()) {
        size_t nl = acc.find('\n', start);
        std::string line = acc.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    return lines;
}

void workerLoop() {
    for (;;) {
        auto cmd = g_inbound.tryPop();
        if (!cmd) {
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
            continue;
        }
        for (auto& line : runCommand(*cmd)) {
            g_outbound.push(std::move(line));
        }
    }
}

}  // namespace bridge
