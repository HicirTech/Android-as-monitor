#pragma once

#include "driver_includes.h"

// The driver host has no console or per-user folder, and every driver change costs an elevated reinstall,
// so diagnostics go to the Application event log, which the unelevated companion can read.
void LogEvent(_Printf_format_string_ const wchar_t* format, ...);
