#include "event_log.h"

#include <cstdarg>
#include <cstdio>

void LogEvent(const wchar_t* format, ...) {
    static HANDLE source = RegisterEventSourceW(nullptr, L"AndroidUsbMonitor");

    wchar_t message[512];
    va_list arguments;
    va_start(arguments, format);
    vswprintf_s(message, format, arguments);
    va_end(arguments);

    OutputDebugStringW(message);
    if (source) {
        const wchar_t* strings[] = {message};
        ReportEventW(source, EVENTLOG_INFORMATION_TYPE, 0, 1, nullptr, 1, 0, strings, nullptr);
    }
}
