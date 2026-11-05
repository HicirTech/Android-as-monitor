#include "device_context.h"

#include "context_wrappers.h"
#include "event_log.h"
#include "monitor_context.h"

// The OS keeps presenting the last frame this many times after the desktop goes idle,
// which lets a low-latency rate controlled encoder refine static text.
static constexpr UINT kStaticDesktopReencodeFrames = 60;

void DeviceContext::InitAdapter() {
    if (adapter_) {
        return;
    }

    IDDCX_ADAPTER_CAPS caps{};
    caps.Size = sizeof(caps);
    caps.MaxMonitorsSupported = 1;
    caps.StaticDesktopReencodeFrameCount = kStaticDesktopReencodeFrames;
    caps.EndPointDiagnostics.Size = sizeof(caps.EndPointDiagnostics);
    caps.EndPointDiagnostics.GammaSupport = IDDCX_FEATURE_IMPLEMENTATION_NONE;
    caps.EndPointDiagnostics.TransmissionType = IDDCX_TRANSMISSION_TYPE_WIRED_USB;
    caps.EndPointDiagnostics.pEndPointFriendlyName = L"Android USB Monitor";
    caps.EndPointDiagnostics.pEndPointManufacturerName = L"Android USB Monitor";
    caps.EndPointDiagnostics.pEndPointModelName = L"Android USB Monitor";

    IDDCX_ENDPOINT_VERSION version{};
    version.Size = sizeof(version);
    version.MajorVer = 1;
    caps.EndPointDiagnostics.pFirmwareVersion = &version;
    caps.EndPointDiagnostics.pHardwareVersion = &version;

    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, DeviceContextWrapper);

    IDARG_IN_ADAPTER_INIT init{};
    init.WdfDevice = device_;
    init.pCaps = &caps;
    init.ObjectAttributes = &attributes;

    IDARG_OUT_ADAPTER_INIT initOut{};
    const NTSTATUS status = IddCxAdapterInitAsync(&init, &initOut);
    if (!NT_SUCCESS(status)) {
        LogEvent(L"adapter: IddCxAdapterInitAsync failed 0x%08lX", status);
        return;
    }
    adapter_ = initOut.AdapterObject;
    WdfObjectGet_DeviceContextWrapper(initOut.AdapterObject)->context = this;
}

void DeviceContext::OnAdapterReady() {
    if (controlPipe_) {
        return;
    }
    controlPipe_ = std::make_unique<ControlPipe>(
        [this](const aum::ControlRequest& request) { return HandleRequest(request); },
        [this] { Unplug(); });
    if (!controlPipe_->Start()) {
        controlPipe_.reset();
    }
}

aum::ControlResponse DeviceContext::HandleRequest(const aum::ControlRequest& request) {
    aum::ControlResponse response{};
    switch (request.command) {
    case aum::ControlCommand::Plug:
        response.status = Plug(request, response);
        break;
    case aum::ControlCommand::Unplug:
        Unplug();
        response.status = STATUS_SUCCESS;
        break;
    default:
        response.status = STATUS_INVALID_PARAMETER;
        break;
    }
    return response;
}

NTSTATUS DeviceContext::Plug(const aum::ControlRequest& request, aum::ControlResponse& response) {
    if (monitor_) {
        return STATUS_DEVICE_ALREADY_ATTACHED;
    }
    if (request.edidSize > aum::kMaxEdidBytes) {
        return STATUS_INVALID_PARAMETER;
    }
    const auto timing = aum::ParseFirstDetailedTiming(request.edid, request.edidSize);
    if (!timing) {
        LogEvent(L"monitor: rejected EDID of %u bytes", request.edidSize);
        return STATUS_INVALID_PARAMETER;
    }

    if (request.preferredRenderAdapter.LowPart != 0 || request.preferredRenderAdapter.HighPart != 0) {
        IDARG_IN_ADAPTERSETRENDERADAPTER renderAdapter{};
        renderAdapter.PreferredRenderAdapter = request.preferredRenderAdapter;
        IddCxAdapterSetRenderAdapter(adapter_, &renderAdapter);
    }

    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, MonitorContextWrapper);
    attributes.EvtCleanupCallback = [](WDFOBJECT object) {
        auto* wrapper = WdfObjectGet_MonitorContextWrapper(object);
        delete wrapper->context;
        wrapper->context = nullptr;
    };

    IDDCX_MONITOR_INFO info{};
    info.Size = sizeof(info);
    info.MonitorType = DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INDIRECT_WIRED;
    info.ConnectorIndex = 0;
    info.MonitorContainerId = request.containerId;
    info.MonitorDescription.Size = sizeof(info.MonitorDescription);
    info.MonitorDescription.Type = IDDCX_MONITOR_DESCRIPTION_TYPE_EDID;
    info.MonitorDescription.DataSize = request.edidSize;
    memcpy(edid_.data(), request.edid, request.edidSize);
    info.MonitorDescription.pData = edid_.data();

    IDARG_IN_MONITORCREATE create{};
    create.ObjectAttributes = &attributes;
    create.pMonitorInfo = &info;

    IDARG_OUT_MONITORCREATE createOut{};
    NTSTATUS status = IddCxMonitorCreate(adapter_, &create, &createOut);
    if (!NT_SUCCESS(status)) {
        LogEvent(L"monitor: IddCxMonitorCreate failed 0x%08lX", status);
        return status;
    }
    WdfObjectGet_MonitorContextWrapper(createOut.MonitorObject)->context = new MonitorContext(*timing, frameTapChannel_);

    IDARG_OUT_MONITORARRIVAL arrival{};
    status = IddCxMonitorArrival(createOut.MonitorObject, &arrival);
    if (!NT_SUCCESS(status)) {
        LogEvent(L"monitor: IddCxMonitorArrival failed 0x%08lX", status);
        WdfObjectDelete(createOut.MonitorObject);
        return status;
    }

    monitor_ = createOut.MonitorObject;
    response.osAdapterLuid = arrival.OsAdapterLuid;
    response.osTargetId = arrival.OsTargetId;
    const aum::Rational refresh = aum::RefreshRate(*timing);
    LogEvent(L"monitor: plugged %ux%u at %llu/%llu Hz, %ux%u mm", timing->hActive, timing->vActive, refresh.numerator,
             refresh.denominator, timing->widthMm, timing->heightMm);
    return STATUS_SUCCESS;
}

void DeviceContext::Unplug() {
    if (!monitor_) {
        return;
    }
    const NTSTATUS status = IddCxMonitorDeparture(monitor_);
    LogEvent(L"monitor: unplugged 0x%08lX", status);
    monitor_ = nullptr;
}
