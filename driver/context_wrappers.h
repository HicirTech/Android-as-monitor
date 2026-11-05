#pragma once

#include "driver_includes.h"

class DeviceContext;
class MonitorContext;

struct DeviceContextWrapper {
    DeviceContext* context;
};

struct MonitorContextWrapper {
    MonitorContext* context;
};

WDF_DECLARE_CONTEXT_TYPE(DeviceContextWrapper);
WDF_DECLARE_CONTEXT_TYPE(MonitorContextWrapper);
