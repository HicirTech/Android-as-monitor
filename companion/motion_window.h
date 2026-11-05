#pragma once

#include "gpu.h"

#include <windows.h>

#include <atomic>
#include <thread>

namespace aum {

// Fills one display with a vsync-driven moving bar, so the test can show whether the desktop really produces
// a new frame at the panel's full rate instead of relying on whatever the user happens to be doing.
class MotionWindow {
public:
    MotionWindow(LUID adapter, const RECT& desktopRect);
    ~MotionWindow();

    MotionWindow(const MotionWindow&) = delete;
    MotionWindow& operator=(const MotionWindow&) = delete;

private:
    void Run();

    LUID adapter_;
    RECT rect_;
    std::atomic<bool> stopping_{false};
    std::thread thread_;
};

}  // namespace aum
