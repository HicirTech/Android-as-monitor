#include "context_wrappers.h"
#include "device_context.h"
#include "event_log.h"
#include "monitor_context.h"

extern "C" DRIVER_INITIALIZE DriverEntry;

static EVT_WDF_DRIVER_DEVICE_ADD OnDeviceAdd;
static EVT_WDF_DEVICE_D0_ENTRY OnDeviceD0Entry;
static EVT_IDD_CX_ADAPTER_INIT_FINISHED OnAdapterInitFinished;
static EVT_IDD_CX_ADAPTER_COMMIT_MODES OnAdapterCommitModes;
static EVT_IDD_CX_PARSE_MONITOR_DESCRIPTION OnParseMonitorDescription;
static EVT_IDD_CX_MONITOR_GET_DEFAULT_DESCRIPTION_MODES OnMonitorGetDefaultModes;
static EVT_IDD_CX_MONITOR_QUERY_TARGET_MODES OnMonitorQueryTargetModes;
static EVT_IDD_CX_MONITOR_ASSIGN_SWAPCHAIN OnMonitorAssignSwapChain;
static EVT_IDD_CX_MONITOR_UNASSIGN_SWAPCHAIN OnMonitorUnassignSwapChain;

// Keeps the rational within the 32 bits of DISPLAYCONFIG_RATIONAL; only hSync can get this large and nothing reads it
// closely, whereas vSync is an exact reduced fraction of the EDID pixel clock.
static DISPLAYCONFIG_RATIONAL ToDisplayRational(aum::Rational value) {
    while (value.numerator > UINT32_MAX || value.denominator > UINT32_MAX) {
        value.numerator >>= 1;
        value.denominator >>= 1;
    }
    return {static_cast<UINT32>(value.numerator), static_cast<UINT32>(value.denominator)};
}

static void FillSignalInfo(DISPLAYCONFIG_VIDEO_SIGNAL_INFO& mode, const aum::DetailedTiming& timing, bool isMonitorMode) {
    const aum::Rational refresh = aum::RefreshRate(timing);

    mode.totalSize.cx = mode.activeSize.cx = timing.hActive;
    mode.totalSize.cy = mode.activeSize.cy = timing.vActive;
    mode.AdditionalSignalInfo.vSyncFreqDivider = isMonitorMode ? 0 : 1;
    mode.AdditionalSignalInfo.videoStandard = 255;
    mode.vSyncFreq = ToDisplayRational(refresh);
    mode.hSyncFreq = ToDisplayRational(aum::Reduce(refresh.numerator * timing.vActive, refresh.denominator));
    mode.scanLineOrdering = DISPLAYCONFIG_SCANLINE_ORDERING_PROGRESSIVE;
    mode.pixelRate = refresh.numerator * timing.hActive * timing.vActive / refresh.denominator;
}

extern "C" BOOL WINAPI DllMain(_In_ HINSTANCE, _In_ UINT, _In_opt_ LPVOID) {
    return TRUE;
}

_Use_decl_annotations_ extern "C" NTSTATUS DriverEntry(PDRIVER_OBJECT driverObject, PUNICODE_STRING registryPath) {
    WDF_DRIVER_CONFIG config;
    WDF_DRIVER_CONFIG_INIT(&config, OnDeviceAdd);

    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);

    return WdfDriverCreate(driverObject, registryPath, &attributes, &config, WDF_NO_HANDLE);
}

_Use_decl_annotations_ static NTSTATUS OnDeviceAdd(WDFDRIVER, PWDFDEVICE_INIT deviceInit) {
    WDF_PNPPOWER_EVENT_CALLBACKS power;
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&power);
    power.EvtDeviceD0Entry = OnDeviceD0Entry;
    WdfDeviceInitSetPnpPowerEventCallbacks(deviceInit, &power);

    IDD_CX_CLIENT_CONFIG config;
    IDD_CX_CLIENT_CONFIG_INIT(&config);
    config.EvtIddCxAdapterInitFinished = OnAdapterInitFinished;
    config.EvtIddCxParseMonitorDescription = OnParseMonitorDescription;
    config.EvtIddCxMonitorGetDefaultDescriptionModes = OnMonitorGetDefaultModes;
    config.EvtIddCxMonitorQueryTargetModes = OnMonitorQueryTargetModes;
    config.EvtIddCxAdapterCommitModes = OnAdapterCommitModes;
    config.EvtIddCxMonitorAssignSwapChain = OnMonitorAssignSwapChain;
    config.EvtIddCxMonitorUnassignSwapChain = OnMonitorUnassignSwapChain;

    NTSTATUS status = IddCxDeviceInitConfig(deviceInit, &config);
    if (!NT_SUCCESS(status)) {
        LogEvent(L"device: IddCxDeviceInitConfig failed 0x%08lX", status);
        return status;
    }

    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, DeviceContextWrapper);
    attributes.EvtCleanupCallback = [](WDFOBJECT object) {
        auto* wrapper = WdfObjectGet_DeviceContextWrapper(object);
        delete wrapper->context;
        wrapper->context = nullptr;
    };

    WDFDEVICE device = nullptr;
    status = WdfDeviceCreate(&deviceInit, &attributes, &device);
    if (!NT_SUCCESS(status)) {
        LogEvent(L"device: WdfDeviceCreate failed 0x%08lX", status);
        return status;
    }

    status = IddCxDeviceInitialize(device);
    if (!NT_SUCCESS(status)) {
        LogEvent(L"device: IddCxDeviceInitialize failed 0x%08lX", status);
        return status;
    }

    WdfObjectGet_DeviceContextWrapper(device)->context = new DeviceContext(device);
    LogEvent(L"device: added");
    return STATUS_SUCCESS;
}

_Use_decl_annotations_ static NTSTATUS OnDeviceD0Entry(WDFDEVICE device, WDF_POWER_DEVICE_STATE) {
    WdfObjectGet_DeviceContextWrapper(device)->context->InitAdapter();
    return STATUS_SUCCESS;
}

_Use_decl_annotations_ static NTSTATUS OnAdapterInitFinished(IDDCX_ADAPTER adapter, const IDARG_IN_ADAPTER_INIT_FINISHED* args) {
    if (!NT_SUCCESS(args->AdapterInitStatus)) {
        LogEvent(L"adapter: init failed 0x%08lX", args->AdapterInitStatus);
        return STATUS_SUCCESS;
    }
    WdfObjectGet_DeviceContextWrapper(adapter)->context->OnAdapterReady();
    return STATUS_SUCCESS;
}

_Use_decl_annotations_ static NTSTATUS OnAdapterCommitModes(IDDCX_ADAPTER, const IDARG_IN_COMMITMODES*) {
    return STATUS_SUCCESS;
}

// The requirement is one mode only: the panel's native mode as the EDID states it. No fallback modes are ever offered.
_Use_decl_annotations_ static NTSTATUS OnParseMonitorDescription(const IDARG_IN_PARSEMONITORDESCRIPTION* args,
                                                                 IDARG_OUT_PARSEMONITORDESCRIPTION* out) {
    static constexpr UINT kModeCount = 1;

    const auto timing = aum::ParseNativeTiming(static_cast<const std::uint8_t*>(args->MonitorDescription.pData),
                                               args->MonitorDescription.DataSize);
    if (!timing) {
        return STATUS_INVALID_PARAMETER;
    }

    out->MonitorModeBufferOutputCount = kModeCount;
    if (args->MonitorModeBufferInputCount < kModeCount) {
        return args->MonitorModeBufferInputCount > 0 ? STATUS_BUFFER_TOO_SMALL : STATUS_SUCCESS;
    }

    IDDCX_MONITOR_MODE mode{};
    mode.Size = sizeof(mode);
    mode.Origin = IDDCX_MONITOR_MODE_ORIGIN_MONITORDESCRIPTOR;
    FillSignalInfo(mode.MonitorVideoSignalInfo, *timing, true);
    args->pMonitorModes[0] = mode;
    out->PreferredMonitorModeIdx = 0;
    return STATUS_SUCCESS;
}

// Every monitor this driver creates has an EDID, so the OS has no reason to ask for description-less defaults.
_Use_decl_annotations_ static NTSTATUS OnMonitorGetDefaultModes(IDDCX_MONITOR, const IDARG_IN_GETDEFAULTDESCRIPTIONMODES*,
                                                                IDARG_OUT_GETDEFAULTDESCRIPTIONMODES* out) {
    out->DefaultMonitorModeBufferOutputCount = 0;
    return STATUS_NOT_SUPPORTED;
}

_Use_decl_annotations_ static NTSTATUS OnMonitorQueryTargetModes(IDDCX_MONITOR monitor, const IDARG_IN_QUERYTARGETMODES* args,
                                                                 IDARG_OUT_QUERYTARGETMODES* out) {
    static constexpr UINT kModeCount = 1;

    out->TargetModeBufferOutputCount = kModeCount;
    if (args->TargetModeBufferInputCount >= kModeCount) {
        IDDCX_TARGET_MODE mode{};
        mode.Size = sizeof(mode);
        FillSignalInfo(mode.TargetVideoSignalInfo.targetVideoSignalInfo,
                       WdfObjectGet_MonitorContextWrapper(monitor)->context->Timing(), false);
        args->pTargetModes[0] = mode;
    }
    return STATUS_SUCCESS;
}

_Use_decl_annotations_ static NTSTATUS OnMonitorAssignSwapChain(IDDCX_MONITOR monitor, const IDARG_IN_SETSWAPCHAIN* args) {
    WdfObjectGet_MonitorContextWrapper(monitor)->context->AssignSwapChain(args->hSwapChain, args->RenderAdapterLuid,
                                                                          args->hNextSurfaceAvailable);
    return STATUS_SUCCESS;
}

_Use_decl_annotations_ static NTSTATUS OnMonitorUnassignSwapChain(IDDCX_MONITOR monitor) {
    WdfObjectGet_MonitorContextWrapper(monitor)->context->UnassignSwapChain();
    return STATUS_SUCCESS;
}
