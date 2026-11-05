#include "adb.h"

#include "win_error.h"

#include <filesystem>
#include <stdexcept>
#include <thread>

namespace aum {

Adb::Adb(std::wstring executable, std::string serial) : executable_(std::move(executable)), serial_(std::move(serial)) {}

std::wstring Adb::Locate(const std::wstring& override) {
    if (!override.empty()) {
        return override;
    }
    wchar_t found[MAX_PATH];
    if (SearchPathW(nullptr, L"adb.exe", nullptr, MAX_PATH, found, nullptr) > 0) {
        return found;
    }
    wchar_t localAppData[MAX_PATH];
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH) > 0) {
        const std::wstring candidate = std::wstring(localAppData) + L"\\Android\\Sdk\\platform-tools\\adb.exe";
        if (std::filesystem::exists(candidate)) {
            return candidate;
        }
    }
    throw std::runtime_error("adb.exe not found; pass --adb <path>");
}

Adb::Started Adb::Start(const std::string& arguments) const {
    SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
    HANDLE readPipe = nullptr;
    HANDLE writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &inherit, 0)) {
        throw std::runtime_error("CreatePipe failed");
    }
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = writePipe;
    startup.hStdError = writePipe;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    std::wstring command = L"\"" + executable_ + L"\"";
    if (!serial_.empty()) {
        command += L" -s " + Widen(serial_);
    }
    command += L" " + Widen(arguments);

    PROCESS_INFORMATION process{};
    const BOOL created = CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
    CloseHandle(writePipe);
    if (!created) {
        CloseHandle(readPipe);
        throw std::runtime_error("cannot start adb: " + Narrow(command));
    }
    CloseHandle(process.hThread);
    return {process.hProcess, readPipe};
}

std::string Adb::Run(const std::string& arguments, unsigned long timeoutMs) const {
    const Started started = Start(arguments);
    const HANDLE readPipe = started.output;

    std::string output;
    std::thread reader([&] {
        char buffer[4096];
        DWORD read = 0;
        while (ReadFile(readPipe, buffer, sizeof(buffer), &read, nullptr) && read > 0) {
            output.append(buffer, read);
        }
    });

    const DWORD wait = WaitForSingleObject(started.process, timeoutMs);
    if (wait != WAIT_OBJECT_0) {
        TerminateProcess(started.process, 1);
    }
    DWORD exitCode = 1;
    GetExitCodeProcess(started.process, &exitCode);
    reader.join();
    CloseHandle(readPipe);
    CloseHandle(started.process);

    if (wait != WAIT_OBJECT_0) {
        throw std::runtime_error("adb timed out: " + arguments);
    }
    if (exitCode != 0) {
        throw std::runtime_error("adb " + arguments + " failed: " + output);
    }
    return output;
}

void Adb::Stream(const std::string& arguments, HANDLE stop, const std::function<void(std::string_view)>& onOutput) const {
    const Started started = Start(arguments);
    std::thread stopper([&] {
        const HANDLE handles[] = {stop, started.process};
        if (WaitForMultipleObjects(ARRAYSIZE(handles), handles, FALSE, INFINITE) == WAIT_OBJECT_0) {
            TerminateProcess(started.process, 1);
        }
    });
    char buffer[4096];
    DWORD read = 0;
    while (ReadFile(started.output, buffer, sizeof(buffer), &read, nullptr) && read > 0) {
        onOutput({buffer, read});
    }
    stopper.join();
    CloseHandle(started.output);
    CloseHandle(started.process);
}

std::string Adb::RunOrEmpty(const std::string& arguments) const {
    try {
        return Run(arguments);
    } catch (const std::runtime_error&) {
        return {};
    }
}

}  // namespace aum
