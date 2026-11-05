#include "frame_tap_channel.h"

#include "event_log.h"

#include <sddl.h>

FrameTapChannel::FrameTapChannel() {
    securityAttributes_.nLength = sizeof(securityAttributes_);
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            aum::kSharedObjectSddl, SDDL_REVISION_1, &securityAttributes_.lpSecurityDescriptor, nullptr)) {
        LogEvent(L"frame tap: security descriptor failed, error %lu", GetLastError());
        return;
    }

    mapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, &securityAttributes_, PAGE_READWRITE, 0,
                                  sizeof(aum::FrameTapHeader), aum::kFrameHeaderName);
    if (!mapping_) {
        LogEvent(L"frame tap: header mapping failed, error %lu", GetLastError());
        return;
    }
    header_ = static_cast<aum::FrameTapHeader*>(MapViewOfFile(mapping_, FILE_MAP_ALL_ACCESS, 0, 0, 0));
    if (!header_) {
        LogEvent(L"frame tap: header view failed, error %lu", GetLastError());
        return;
    }
    ZeroMemory(header_, sizeof(*header_));
    header_->magic = aum::kFrameTapMagic;
    header_->abiVersion = aum::kFrameTapAbiVersion;
    header_->latestSlot = -1;

    readyEvent_ = CreateEventW(&securityAttributes_, FALSE, FALSE, aum::kFrameReadyEventName);
    if (!readyEvent_) {
        LogEvent(L"frame tap: ready event failed, error %lu", GetLastError());
    }
}

FrameTapChannel::~FrameTapChannel() {
    if (readyEvent_) {
        CloseHandle(readyEvent_);
    }
    if (header_) {
        UnmapViewOfFile(header_);
    }
    if (mapping_) {
        CloseHandle(mapping_);
    }
    if (securityAttributes_.lpSecurityDescriptor) {
        LocalFree(securityAttributes_.lpSecurityDescriptor);
    }
}
