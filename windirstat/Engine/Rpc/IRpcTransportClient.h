#pragma once

#include "Engine/Rpc/RpcMessages.h"

class IRpcTransportEventHandler
{
public:
    virtual ~IRpcTransportEventHandler() = default;
    virtual void OnRemoteDirectoryProgress(const RpcDirectoryProgressEvent& event) = 0;
    virtual void OnRemoteScanCompleted(const RpcScanCompletedEvent& event) = 0;
    virtual void OnRemoteScanCanceled(const RpcScanCanceledEvent& event) = 0;
    virtual void OnRemoteScanFailed(const RpcScanFailedEvent& event) = 0;
};

class IRpcTransportClient
{
public:
    virtual ~IRpcTransportClient() = default;
    virtual void SetEventHandler(IRpcTransportEventHandler* handler) = 0;
    virtual bool SendStartScan(const RpcStartScanRequest& request) = 0;
    virtual bool SendEnqueue(const RpcEnqueueRequest& request) = 0;
    virtual bool SendCloseRequestInput(const RpcCloseRequestInput& request) = 0;
    virtual bool SendCancelScan(const RpcCancelScanRequest& request) = 0;
};
