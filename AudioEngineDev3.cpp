bool AudioEngine::openWasapiBackend()
{
#if defined(DAW_ENABLE_WASAPI) && (DAW_ENABLE_WASAPI == 1)
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (SUCCEEDED(hr) || hr == S_FALSE)
    {
        backendComInitialized_ = true;
    }

    IMMDeviceEnumerator* enumerator = nullptr;
    hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator),
        nullptr,
        CLSCTX_ALL,
        IID_PPV_ARGS(&enumerator));

    if (FAILED(hr) || enumerator == nullptr)
    {
        setError(kErrorBackendInit, "Failed to create IMMDeviceEnumerator.");
        return false;
    }

    IMMDevice* device = nullptr;
    std::string resolvedDeviceName = "Default WASAPI Device";

    if (!config_.preferredDeviceName.empty())
    {
        IMMDeviceCollection* collection = nullptr;
        hr = enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection);

        if (SUCCEEDED(hr) && collection != nullptr)
        {
            UINT count = 0;
            collection->GetCount(&count);

            const std::string wantedName = toLowerCopy(config_.preferredDeviceName);
            for (UINT index = 0; index < count && device == nullptr; ++index)
            {
                IMMDevice* candidate = nullptr;
                if (FAILED(collection->Item(index, &candidate)) || candidate == nullptr)
                {
                    continue;
                }

                IPropertyStore* propertyStore = nullptr;
                PROPVARIANT friendlyName;
                PropVariantInit(&friendlyName);

                if (SUCCEEDED(candidate->OpenPropertyStore(STGM_READ, &propertyStore)) &&
                    propertyStore != nullptr &&
                    SUCCEEDED(propertyStore->GetValue(PKEY_Device_FriendlyName, &friendlyName)) &&
                    friendlyName.vt == VT_LPWSTR)
                {
                    char convertedName[512]{};
                    WideCharToMultiByte(
                        CP_UTF8,
                        0,
                        friendlyName.pwszVal,
                        -1,
                        convertedName,
                        static_cast<int>(sizeof(convertedName)),
                        nullptr,
                        nullptr);

                    const std::string candidateName = convertedName;
                    if (toLowerCopy(candidateName).find(wantedName) != std::string::npos)
                    {
                        device = candidate;
                        resolvedDeviceName = candidateName;
                        candidate = nullptr;
                    }
                }

                PropVariantClear(&friendlyName);
                safeRelease(propertyStore);
                safeRelease(candidate);
            }

            safeRelease(collection);
        }
    }

    if (device == nullptr)
    {
        hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
        if (FAILED(hr) || device == nullptr)
        {
            safeRelease(enumerator);
            setError(kErrorBackendInit, "Failed to resolve the default WASAPI render device.");
            return false;
        }
    }

    IAudioClient* audioClient = nullptr;
    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&audioClient));
    if (FAILED(hr) || audioClient == nullptr)
    {
        safeRelease(device);
        safeRelease(enumerator);
        setError(kErrorBackendInit, "Failed to activate IAudioClient.");
        return false;
    }

    WAVEFORMATEX* mixFormat = nullptr;
    hr = audioClient->GetMixFormat(&mixFormat);
    if (FAILED(hr) || mixFormat == nullptr)
    {
        safeRelease(audioClient);
        safeRelease(device);
        safeRelease(enumerator);
        setError(kErrorBackendInit, "Failed to query WASAPI mix format.");
        return false;
    }

    REFERENCE_TIME requestedDuration = static_cast<REFERENCE_TIME>(
        (10'000'000.0 * static_cast<double>(config_.preferredBlockSize)) /
        static_cast<double>(std::max<DWORD>(1u, mixFormat->nSamplesPerSec)));

    DWORD streamFlags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_NOPERSIST;
    hr = audioClient->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        streamFlags,
        requestedDuration,
        0,
        mixFormat,
        nullptr);

    if (FAILED(hr))
    {
        CoTaskMemFree(mixFormat);
        safeRelease(audioClient);
        safeRelease(device);
        safeRelease(enumerator);
        setError(kErrorBackendInit, "Failed to initialize WASAPI shared-mode stream.");
        return false;
    }

    HANDLE bufferEvent = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    if (bufferEvent == nullptr)
    {
        CoTaskMemFree(mixFormat);
        safeRelease(audioClient);
        safeRelease(device);
        safeRelease(enumerator);
        setError(kErrorBackendInit, "Failed to create WASAPI event handle.");
        return false;
    }

    hr = audioClient->SetEventHandle(bufferEvent);
    if (FAILED(hr))
    {
        CloseHandle(bufferEvent);
        CoTaskMemFree(mixFormat);
        safeRelease(audioClient);
        safeRelease(device);
        safeRelease(enumerator);
        setError(kErrorBackendInit, "Failed to bind the WASAPI event handle.");
        return false;
    }

    UINT32 bufferFrames = 0;
    audioClient->GetBufferSize(&bufferFrames);

    IAudioRenderClient* renderClient = nullptr;
    hr = audioClient->GetService(IID_PPV_ARGS(&renderClient));
    if (FAILED(hr) || renderClient == nullptr)
    {
        CloseHandle(bufferEvent);
        CoTaskMemFree(mixFormat);
        safeRelease(audioClient);
        safeRelease(device);
        safeRelease(enumerator);
        setError(kErrorBackendInit, "Failed to acquire IAudioRenderClient.");
        return false;
    }

    deviceState_.backendType = BackendType::Wasapi;
    deviceState_.backendName = "WASAPI Shared/Event";
    deviceState_.deviceName = resolvedDeviceName;
    deviceState_.sampleRate = mixFormat->nSamplesPerSec;
    deviceState_.blockSize = std::min<std::uint32_t>(
        static_cast<std::uint32_t>(config_.preferredBlockSize),
        std::max<std::uint32_t>(1, bufferFrames));
    deviceState_.deviceBufferFrames = bufferFrames;
    deviceState_.inputChannels = static_cast<std::uint32_t>(config_.inputChannelCount);
    deviceState_.outputChannels = mixFormat->nChannels;
    deviceState_.sharedMode = true;
    deviceState_.eventDriven = true;
    deviceState_.usingPreferredFormat = mixFormat->nSamplesPerSec == static_cast<std::uint32_t>(config_.preferredSampleRate);
    deviceState_.format.sampleRate = mixFormat->nSamplesPerSec;
    deviceState_.format.channelCount = mixFormat->nChannels;
    deviceState_.format.bitsPerSample = mixFormat->wBitsPerSample;
    deviceState_.format.floatingPoint = mixFormat->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
    deviceState_.format.extensible = mixFormat->wFormatTag == WAVE_FORMAT_EXTENSIBLE;
    deviceState_.mixFormatBlob.assign(
        reinterpret_cast<const std::uint8_t*>(mixFormat),
        reinterpret_cast<const std::uint8_t*>(mixFormat) + (sizeof(WAVEFORMATEX) + mixFormat->cbSize));
    deviceState_.eventHandle = reinterpret_cast<std::uintptr_t>(bufferEvent);
    deviceState_.enumeratorHandle = reinterpret_cast<std::uintptr_t>(enumerator);
    deviceState_.deviceHandle = reinterpret_cast<std::uintptr_t>(device);
    deviceState_.audioClientHandle = reinterpret_cast<std::uintptr_t>(audioClient);
    deviceState_.renderClientHandle = reinterpret_cast<std::uintptr_t>(renderClient);
    deviceState_.captureClientHandle = 0;
    deviceState_.initialized = true;
    deviceState_.started = false;

    realtimeBuffer_.resize(deviceState_.blockSize);
    anticipativeBuffer_.resize(deviceState_.blockSize);

    CoTaskMemFree(mixFormat);
    return true;
#else
    return false;
#endif
}

void AudioEngine::closeAudioBackend()
{
#if defined(DAW_ENABLE_WASAPI) && (DAW_ENABLE_WASAPI == 1)
    safeCloseHandle(deviceState_.eventHandle);

    if (deviceState_.renderClientHandle != 0)
    {
        IAudioRenderClient* renderClient = reinterpret_cast<IAudioRenderClient*>(deviceState_.renderClientHandle);
        safeRelease(renderClient);
        deviceState_.renderClientHandle = 0;
    }

    if (deviceState_.captureClientHandle != 0)
    {
        IAudioCaptureClient* captureClient = reinterpret_cast<IAudioCaptureClient*>(deviceState_.captureClientHandle);
        safeRelease(captureClient);
        deviceState_.captureClientHandle = 0;
    }

    if (deviceState_.audioClientHandle != 0)
    {
        IAudioClient* audioClient = reinterpret_cast<IAudioClient*>(deviceState_.audioClientHandle);
        safeRelease(audioClient);
        deviceState_.audioClientHandle = 0;
    }

    if (deviceState_.deviceHandle != 0)
    {
        IMMDevice* device = reinterpret_cast<IMMDevice*>(deviceState_.deviceHandle);
        safeRelease(device);
        deviceState_.deviceHandle = 0;
    }

    if (deviceState_.enumeratorHandle != 0)
    {
        IMMDeviceEnumerator* enumerator = reinterpret_cast<IMMDeviceEnumerator*>(deviceState_.enumeratorHandle);
        safeRelease(enumerator);
        deviceState_.enumeratorHandle = 0;
    }
#endif

    if (backendComInitialized_)
    {
        CoUninitialize();
        backendComInitialized_ = false;
    }

    deviceState_.initialized = false;
    deviceState_.started = false;
    deviceState_.mixFormatBlob.clear();
}

bool AudioEngine::startAudioStream()
{
#if defined(DAW_ENABLE_WASAPI) && (DAW_ENABLE_WASAPI == 1)
    if (deviceState_.backendType == BackendType::Wasapi && deviceState_.audioClientHandle != 0)
    {
        IAudioClient* audioClient = reinterpret_cast<IAudioClient*>(deviceState_.audioClientHandle);
        IAudioRenderClient* renderClient = reinterpret_cast<IAudioRenderClient*>(deviceState_.renderClientHandle);
        if (audioClient == nullptr || renderClient == nullptr)
        {
            setError(kErrorBackendInit, "WASAPI client is not ready.");
            return false;
        }

        UINT32 bufferFrames = 0;
        audioClient->GetBufferSize(&bufferFrames);

        BYTE* data = nullptr;
        if (SUCCEEDED(renderClient->GetBuffer(bufferFrames, &data)) && data != nullptr)
        {
            std::memset(data, 0, static_cast<std::size_t>(bufferFrames) * deviceState_.format.channelCount * (deviceState_.format.bitsPerSample / 8));
            renderClient->ReleaseBuffer(bufferFrames, 0);
        }

        const HRESULT hr = audioClient->Start();
        if (FAILED(hr))
        {
            setError(kErrorBackendInit, "Failed to start the WASAPI stream.");
            return false;
        }
    }
#endif

    deviceState_.started = true;
    return true;
}

void AudioEngine::stopAudioStream()
{
#if defined(DAW_ENABLE_WASAPI) && (DAW_ENABLE_WASAPI == 1)
    if (deviceState_.audioClientHandle != 0)
    {
        IAudioClient* audioClient = reinterpret_cast<IAudioClient*>(deviceState_.audioClientHandle);
        if (audioClient != nullptr)
        {
            audioClient->Stop();
            audioClient->Reset();
        }
    }
#endif

    deviceState_.started = false;
}

bool AudioEngine::waitForNextAudioBuffer(std::uint32_t& outFramesToRender)
{
    outFramesToRender = deviceState_.blockSize;

    if (deviceState_.backendType == BackendType::Dummy)
    {
        const auto sleepDuration = std::chrono::microseconds(
            static_cast<long long>(
                (static_cast<double>(deviceState_.blockSize) / static_cast<double>(std::max(1u, deviceState_.sampleRate))) *
                1'000'000.0));
        std::this_thread::sleep_for(sleepDuration);
        return true;
    }

#if defined(DAW_ENABLE_WASAPI) && (DAW_ENABLE_WASAPI == 1)
    HANDLE bufferEvent = reinterpret_cast<HANDLE>(deviceState_.eventHandle);
    if (bufferEvent == nullptr)
    {
        setError(kErrorAudioRuntime, "Missing WASAPI event handle.");
        return false;
    }

    const DWORD waitResult = WaitForSingleObject(bufferEvent, kBackendWaitTimeoutMs);
    if (waitResult == WAIT_TIMEOUT)
    {
        deviceState_.recovery.deviceLost = true;
        deviceState_.recovery.restartPending = true;
        deviceState_.recovery.lastFailure = "WASAPI event wait timed out.";
        setError(kErrorAudioRuntime, deviceState_.recovery.lastFailure);
        return false;
    }

    if (waitResult != WAIT_OBJECT_0)
    {
        deviceState_.recovery.deviceLost = true;
        deviceState_.recovery.restartPending = true;
        deviceState_.recovery.lastFailure = "WASAPI event wait failed.";
        setError(kErrorAudioRuntime, deviceState_.recovery.lastFailure);
        return false;
    }

    IAudioClient* audioClient = reinterpret_cast<IAudioClient*>(deviceState_.audioClientHandle);
    if (audioClient == nullptr)
    {
        setError(kErrorAudioRuntime, "IAudioClient is not available.");
        return false;
    }

    UINT32 padding = 0;
    if (FAILED(audioClient->GetCurrentPadding(&padding)))
    {
        setError(kErrorAudioRuntime, "Failed to query current WASAPI padding.");
        return false;
    }

    const UINT32 availableFrames = deviceState_.deviceBufferFrames > padding
        ? deviceState_.deviceBufferFrames - padding
        : 0;

    outFramesToRender = std::min<std::uint32_t>(
        std::max<std::uint32_t>(1, availableFrames),
        std::max<std::uint32_t>(1, deviceState_.blockSize));
    return true;
#else
    return true;
#endif
}

bool AudioEngine::writeBufferToBackend(const AudioBuffer& buffer)
{
    if (deviceState_.backendType == BackendType::Dummy)
    {
        return true;
    }

#if defined(DAW_ENABLE_WASAPI) && (DAW_ENABLE_WASAPI == 1)
    IAudioClient* audioClient = reinterpret_cast<IAudioClient*>(deviceState_.audioClientHandle);
    IAudioRenderClient* renderClient = reinterpret_cast<IAudioRenderClient*>(deviceState_.renderClientHandle);
    if (audioClient == nullptr || renderClient == nullptr)
    {
        setError(kErrorAudioRuntime, "WASAPI render path is not available.");
        return false;
    }

    UINT32 padding = 0;
    if (FAILED(audioClient->GetCurrentPadding(&padding)))
    {
        setError(kErrorAudioRuntime, "Failed to query padding before writing audio.");
        return false;
    }

    const UINT32 availableFrames = deviceState_.deviceBufferFrames > padding
        ? deviceState_.deviceBufferFrames - padding
        : 0;
    const UINT32 framesToWrite = std::min<UINT32>(buffer.frameCount, std::max<UINT32>(1, availableFrames));

    BYTE* rawBuffer = nullptr;
    const HRESULT hr = renderClient->GetBuffer(framesToWrite, &rawBuffer);
    if (FAILED(hr) || rawBuffer == nullptr)
    {
        setError(kErrorAudioRuntime, "Failed to lock the WASAPI render buffer.");
        return false;
    }

    const std::uint32_t channelCount = std::max<std::uint32_t>(1, deviceState_.format.channelCount);
    const std::uint32_t bitsPerSample = std::max<std::uint32_t>(8, deviceState_.format.bitsPerSample);

    if (deviceState_.format.floatingPoint && bitsPerSample == 32)
    {
        float* out = reinterpret_cast<float*>(rawBuffer);
        for (UINT32 frame = 0; frame < framesToWrite; ++frame)
        {
            const float left = static_cast<float>(clampSample(buffer.left[frame]));
            const float right = static_cast<float>(clampSample(buffer.right[frame]));
            out[(frame * channelCount) + 0] = left;
            if (channelCount > 1)
            {
                out[(frame * channelCount) + 1] = right;
            }
            for (std::uint32_t channel = 2; channel < channelCount; ++channel)
            {
                out[(frame * channelCount) + channel] = 0.0f;
            }
        }
    }
    else if (bitsPerSample == 16)
    {
        auto* out = reinterpret_cast<std::int16_t*>(rawBuffer);
        for (UINT32 frame = 0; frame < framesToWrite; ++frame)
        {
            const std::int16_t left = static_cast<std::int16_t>(clampSample(buffer.left[frame]) * 32767.0);
            const std::int16_t right = static_cast<std::int16_t>(clampSample(buffer.right[frame]) * 32767.0);
            out[(frame * channelCount) + 0] = left;
            if (channelCount > 1)
            {
                out[(frame * channelCount) + 1] = right;
            }
            for (std::uint32_t channel = 2; channel < channelCount; ++channel)
            {
                out[(frame * channelCount) + channel] = 0;
            }
        }
    }
    else
    {
        std::memset(rawBuffer, 0, static_cast<std::size_t>(framesToWrite) * channelCount * (bitsPerSample / 8));
    }

    renderClient->ReleaseBuffer(framesToWrite, 0);
    return true;
#else
    return false;
#endif
}

void AudioEngine::audioThreadMain()
{
    HRESULT comHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    audioThreadState_.comInitialized = SUCCEEDED(comHr) || comHr == S_FALSE;

#if defined(DAW_ENABLE_WASAPI) && (DAW_ENABLE_WASAPI == 1)
    DWORD taskIndex = 0;
    HANDLE mmcssHandle = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
    audioThreadState_.mmcssRegistered = mmcssHandle != nullptr;
#endif

    if (!startAudioStream())
    {
        running_.store(false);
        audioThreadState_.stopRequested.store(true);
        return;
    }

    audioThreadState_.running.store(true);
    state_.store(EngineState::Running);
    updateStatusFromState();
    publishSnapshot();

    while (!audioThreadState_.stopRequested.load() && running_.load())
    {
        std::uint32_t framesToRender = deviceState_.blockSize;
        if (!waitForNextAudioBuffer(framesToRender))
        {
            if (audioThreadState_.stopRequested.load())
            {
                break;
            }

            if (!recoverAudioDevice())
            {
                break;
            }
            continue;
        }

        if (framesToRender == 0)
        {
            continue;
        }

        realtimeBuffer_.resize(framesToRender);
        anticipativeBuffer_.resize(framesToRender);

        RenderContext renderContext{};
        renderContext.callback.sampleRate = deviceState_.sampleRate;
        renderContext.callback.blockSize = framesToRender;
        renderContext.callback.deviceBufferFrames = deviceState_.deviceBufferFrames;
        renderContext.callback.callbackIndex = metrics_.callbackCount;
        renderContext.callback.transportSampleStart = transportInfo_.samplePosition;
        renderContext.callback.blockStartTimeSeconds = transportInfo_.timelineSeconds;
        renderContext.isOffline = false;
        renderContext.isLivePath = true;
        renderContext.supportsSampleAccurateAutomation = config_.enableSampleAccurateAutomation;

        const auto blockStart = Clock::now();

        processPendingCommands();
        if (!running_.load() || audioThreadState_.stopRequested.load())
        {
            break;
        }

        if (graphSwapPending_)
        {
            swapCompiledGraphAtSafePoint();
        }

        applyAutomationForBlock(renderContext);
        processLiveBlock(renderContext, realtimeBuffer_);
        processAnticipativeWork(renderContext);
        mergeLiveAndAnticipativeResults(renderContext, realtimeBuffer_);
        alignBranchesForMix(realtimeBuffer_);
        queueClipReadAhead(renderContext);

        if (!writeBufferToBackend(realtimeBuffer_))
        {
            if (!recoverAudioDevice())
            {
                break;
            }
            continue;
        }

        if (transportInfo_.state == TransportState::Playing)
        {
            transportInfo_.samplePosition += framesToRender;

            if (transportInfo_.loopEnabled && transportInfo_.loopEndSample > transportInfo_.loopStartSample)
            {
                const std::uint64_t loopLength = transportInfo_.loopEndSample - transportInfo_.loopStartSample;
                if (transportInfo_.samplePosition >= transportInfo_.loopEndSample && loopLength > 0)
                {
                    const std::uint64_t wrappedOffset =
                        (transportInfo_.samplePosition - transportInfo_.loopStartSample) % loopLength;
                    transportInfo_.samplePosition = transportInfo_.loopStartSample + wrappedOffset;
                }
            }

            transportInfo_.timelineSeconds =
                static_cast<double>(transportInfo_.samplePosition) /
                static_cast<double>(std::max(1u, deviceState_.sampleRate));
        }

        const auto blockEnd = Clock::now();
        const double elapsedUs = static_cast<double>(
            std::chrono::duration_cast<std::chrono::microseconds>(blockEnd - blockStart).count());
        const double deadlineUs =
            (static_cast<double>(framesToRender) / static_cast<double>(std::max(1u, deviceState_.sampleRate))) * 1'000'000.0;

        ++metrics_.callbackCount;
        metrics_.averageBlockTimeUs =
            metrics_.callbackCount == 1
            ? elapsedUs
            : ((metrics_.averageBlockTimeUs * static_cast<double>(metrics_.callbackCount - 1)) + elapsedUs) /
              static_cast<double>(metrics_.callbackCount);
        metrics_.peakBlockTimeUs = std::max(metrics_.peakBlockTimeUs, elapsedUs);
        metrics_.cpuLoadApprox = std::min(1.0, elapsedUs / std::max(1.0, deadlineUs));

        if (elapsedUs > deadlineUs)
        {
            ++metrics_.deadlineMisses;
            ++metrics_.xruns;
        }

        audioThreadState_.lastCallbackIndex = metrics_.callbackCount;
        audioThreadState_.lastBlockDurationUs = elapsedUs;
        publishSnapshot();
    }

    stopAudioStream();
    audioThreadState_.running.store(false);

    if (state_.load() != EngineState::Error)
    {
        state_.store(EngineState::Stopped);
    }

    updateStatusFromState();
    publishSnapshot();

#if defined(DAW_ENABLE_WASAPI) && (DAW_ENABLE_WASAPI == 1)
    if (audioThreadState_.mmcssRegistered)
    {
        AvRevertMmThreadCharacteristics(mmcssHandle);
    }
#endif

    if (audioThreadState_.comInitialized)
    {
        CoUninitialize();
        audioThreadState_.comInitialized = false;
    }
}

void AudioEngine::maintenanceThreadMain()
{
    while (telemetryRunning_.load())
    {
        if (!running_.load())
        {
            if (audioThread_.joinable() && !audioThreadState_.running.load())
            {
                audioThread_.join();
            }

            for (auto& thread : helperThreads_)
            {
                if (thread.joinable())
                {
                    thread.join();
                }
            }
            helperThreads_.clear();

            if (diskThread_.joinable())
            {
                diskThread_.join();
            }

            processPendingCommands();

            if (!running_.load() && graphSwapPending_)
            {
                swapCompiledGraphAtSafePoint();
            }
        }

        serviceSandboxWatchdogs();
        serviceClipCacheEviction();
        maybeAutosaveProject();
        std::this_thread::sleep_for(std::chrono::milliseconds(kMaintenanceTickMs));
    }
}

void AudioEngine::scheduleAnticipativeBlock(std::uint64_t liveBlockIndex)
{
    if (!config_.enableAnticipativeProcessing)
    {
        return;
    }

    const std::uint64_t targetBlock = liveBlockIndex + static_cast<std::uint64_t>(std::max(1, config_.anticipativePrefetchBlocks));

    {
        std::lock_guard<std::mutex> lock(anticipativeMutex_);
        if (targetBlock <= requestedAnticipativeBlock_)
        {
            return;
        }

        requestedAnticipativeBlock_ = targetBlock;
        anticipativeWorkPending_ = true;
    }

    anticipativeCv_.notify_one();
}

void AudioEngine::computeAnticipativeBlock(std::uint64_t targetBlockIndex)
{
    RenderContext renderContext{};
    renderContext.isOffline = false;
    renderContext.isLivePath = false;
    renderContext.isAnticipativePass = true;
    renderContext.supportsSampleAccurateAutomation = config_.enableSampleAccurateAutomation;
    renderContext.futureBlockOffset = static_cast<std::uint32_t>(std::max(1, config_.anticipativePrefetchBlocks));
    renderContext.callback.sampleRate = deviceState_.sampleRate;
    renderContext.callback.blockSize = deviceState_.blockSize;
    renderContext.callback.callbackIndex = targetBlockIndex;
    renderContext.callback.transportSampleStart =
        transportInfo_.samplePosition + (targetBlockIndex * static_cast<std::uint64_t>(deviceState_.blockSize));

    AudioBuffer blockBuffer{};
    blockBuffer.resize(deviceState_.blockSize);
    blockBuffer.clear();

    renderGraphBlock(renderContext, blockBuffer, true);
    alignBranchesForMix(blockBuffer);

    {
        std::lock_guard<std::mutex> lock(anticipativeMutex_);
        anticipativeResult_.blockIndex = targetBlockIndex;
        anticipativeResult_.graphVersion = compiledGraph_.sourceVersion;
        anticipativeResult_.valid = true;
        anticipativeResult_.buffer = std::move(blockBuffer);
        completedAnticipativeBlock_ = targetBlockIndex;
    }
}

bool AudioEngine::consumeAnticipativeBlock(std::uint64_t blockIndex, AudioBuffer& outBuffer)
{
    std::lock_guard<std::mutex> lock(anticipativeMutex_);
    if (!anticipativeResult_.valid || anticipativeResult_.blockIndex != blockIndex)
    {
        return false;
    }

    outBuffer = anticipativeResult_.buffer;
    anticipativeResult_.valid = false;
    return true;
}

bool AudioEngine::renderGraphBlock(RenderContext& renderContext, AudioBuffer& outputBuffer, bool includeAnticipativeNodes)
{
    outputBuffer.resize(renderContext.callback.blockSize);
    outputBuffer.clear();

    if (!compiledGraph_.valid)
    {
        return false;
    }

    std::unordered_map<std::uint32_t, AudioBuffer> nodeOutputs;
    const GraphSnapshot& graph = editableGraph_;
    const double sampleRate = static_cast<double>(std::max(1u, renderContext.callback.sampleRate));
    const double blockStartSeconds =
        static_cast<double>(renderContext.callback.transportSampleStart) / sampleRate;

    for (const auto& stage : compiledGraph_.stages)
    {
        for (const std::uint32_t nodeId : stage.nodeIds)
        {
            const CompiledNode* compiledNode = findCompiledNode(nodeId);
            if (compiledNode == nullptr)
            {
                continue;
            }

            const bool processThisPass = includeAnticipativeNodes
                ? compiledNode->anticipativeLane
                : compiledNode->liveLane;

            if (!processThisPass)
            {
                continue;
            }

            const auto graphNodeOpt = findGraphNode(nodeId, graph);
            if (!graphNodeOpt.has_value())
            {
                continue;
            }

            const GraphNode graphNode = graphNodeOpt.value();
            AudioBuffer nodeBuffer{};
            nodeBuffer.resize(outputBuffer.frameCount);
            nodeBuffer.clear();

            for (const std::uint32_t upstreamId : compiledNode->upstreamNodeIds)
            {
                const auto upstreamIt = nodeOutputs.find(upstreamId);
                if (upstreamIt != nodeOutputs.end())
                {
                    mixBuffer(nodeBuffer, upstreamIt->second);
                }
            }

            if (graphNode.type == NodeType::Input && !includeAnticipativeNodes)
            {
                for (std::uint32_t frame = 0; frame < nodeBuffer.frameCount; ++frame)
                {
                    const double phase = static_cast<double>(
                        renderContext.callback.transportSampleStart + frame) /
                        static_cast<double>(std::max(1u, renderContext.callback.sampleRate));
                    const double inputSignal = transportInfo_.monitoringEnabled ? (0.05 * std::sin(kTwoPi * 220.0 * phase)) : 0.0;
                    nodeBuffer.left[frame] += inputSignal;
                    nodeBuffer.right[frame] += inputSignal;
                }
            }

            if (graphNode.type == NodeType::Track || graphNode.type == NodeType::Instrument)
            {
                renderTrackClips(graphNode.trackId, renderContext, nodeBuffer);
            }

            const double nodeGain = graphNode.baseGain;

            for (std::uint32_t frame = 0; frame < nodeBuffer.frameCount; ++frame)
            {
                const double automationValue = getNodeAutomationValue(nodeId, frame);
                const double totalGain = nodeGain * automationValue;
                nodeBuffer.left[frame] *= totalGain;
                nodeBuffer.right[frame] *= totalGain;
            }

            if (graphNode.type == NodeType::Bus)
            {
                const BusState* bus = findBusState(graphNode.busId);
                if (bus == nullptr || bus->muted || (hasSoloedBuses() && !bus->solo))
                {
                    nodeBuffer.clear();
                }
                else
                {
                    const double busGain = evaluateAutomationTarget(
                        "Bus:" + std::to_string(bus->busId) + ":gain",
                        blockStartSeconds,
                        1.0);
                    const double busPan = evaluateAutomationTarget(
                        "Bus:" + std::to_string(bus->busId) + ":pan",
                        blockStartSeconds,
                        bus->pan);
                    const auto [leftPan, rightPan] = stereoPanGains(busPan);
                    for (std::uint32_t frame = 0; frame < nodeBuffer.frameCount; ++frame)
                    {
                        nodeBuffer.left[frame] *= leftPan * busGain;
                        nodeBuffer.right[frame] *= rightPan * busGain;
                    }
                }
            }
            else if (graphNode.type == NodeType::Output || graphNode.type == NodeType::RenderSink)
            {
                const double masterGain = evaluateAutomationTarget("Master:gain", blockStartSeconds, 1.0);
                for (std::uint32_t frame = 0; frame < nodeBuffer.frameCount; ++frame)
                {
                    nodeBuffer.left[frame] *= masterGain;
                    nodeBuffer.right[frame] *= masterGain;
                }
            }

            if (graphNode.type == NodeType::Plugin)
            {
                bool bypassPlugin = graphNode.bypassed;

                std::lock_guard<std::mutex> pluginLock(pluginMutex_);
                const auto pluginIt = std::find_if(
                    loadedPlugins_.begin(),
                    loadedPlugins_.end(),
                    [&](const PluginInstance& plugin) { return plugin.ownerNodeId == graphNode.id; });

                if (pluginIt != loadedPlugins_.end())
                {
                    bypassPlugin = bypassPlugin || pluginIt->autoBypassed;

                    if (!bypassPlugin)
                    {
                        const double precisionGain = pluginIt->descriptor.supportsDoublePrecision ? 1.08 : 1.03;
                        for (std::uint32_t frame = 0; frame < nodeBuffer.frameCount; ++frame)
                        {
                            nodeBuffer.left[frame] = std::tanh(nodeBuffer.left[frame] * precisionGain);
                            nodeBuffer.right[frame] = std::tanh(nodeBuffer.right[frame] * precisionGain);
                        }
                    }
                }
                else
                {
                    for (std::uint32_t frame = 0; frame < nodeBuffer.frameCount; ++frame)
                    {
                        nodeBuffer.left[frame] = std::tanh(nodeBuffer.left[frame] * 1.02);
                        nodeBuffer.right[frame] = std::tanh(nodeBuffer.right[frame] * 1.02);
                    }
                }
            }

            applyNodeDelayCompensation(nodeId, nodeBuffer);
            nodeOutputs[nodeId] = std::move(nodeBuffer);
        }
    }

    for (const auto& compiledNode : compiledGraph_.executionPlan)
    {
        if (compiledNode.type != NodeType::Output && compiledNode.type != NodeType::RenderSink)
        {
            continue;
        }

        const auto nodeIt = nodeOutputs.find(compiledNode.nodeId);
        if (nodeIt != nodeOutputs.end())
        {
            mixBuffer(outputBuffer, nodeIt->second);
        }
    }

    if (outputBuffer.frameCount == 0 && !nodeOutputs.empty())
    {
        outputBuffer.resize(renderContext.callback.blockSize);
        outputBuffer.clear();
        for (const auto& [_, buffer] : nodeOutputs)
        {
            mixBuffer(outputBuffer, buffer);
        }
    }

    return true;
}

void AudioEngine::renderTrackClips(std::uint32_t trackId, const RenderContext& renderContext, AudioBuffer& destination)
{
    const auto trackIt = std::find_if(
        projectState_.tracks.begin(),
        projectState_.tracks.end(),
        [&](const TrackState& track) { return track.trackId == trackId; });

    if (trackIt == projectState_.tracks.end() || trackIt->muted || (hasSoloedTracks() && !trackIt->solo))
    {
        return;
    }

    const std::uint64_t blockStartSample = renderContext.callback.transportSampleStart;
    const double sampleRate = static_cast<double>(std::max(1u, renderContext.callback.sampleRate));
    const double blockStartSeconds = static_cast<double>(blockStartSample) / sampleRate;
    const ChannelSettingsState* channelSettings = findChannelSettings(trackId);

    ChannelSettingsState defaultChannelSettings{};
    defaultChannelSettings.trackId = trackId;
    defaultChannelSettings.name = trackIt->name;
    const ChannelSettingsState& settings = channelSettings == nullptr ? defaultChannelSettings : *channelSettings;
    const double trackGainAutomation = evaluateAutomationTarget(
        "Track:" + std::to_string(trackId) + ":gain",
        blockStartSeconds,
        1.0);
    const double trackPan = evaluateAutomationTarget(
        "Track:" + std::to_string(trackId) + ":pan",
        blockStartSeconds,
        trackIt->pan);
    const double channelGain = evaluateAutomationTarget(
        "Channel:" + std::to_string(trackId) + ":gain",
        blockStartSeconds,
        settings.gain);
    const double channelPan = evaluateAutomationTarget(
        "Channel:" + std::to_string(trackId) + ":pan",
        blockStartSeconds,
        settings.pan);
    const double cutoffHz = evaluateAutomationTarget(
        "Channel:" + std::to_string(trackId) + ":cutoff",
        blockStartSeconds,
        settings.filterCutoffHz);

    for (const std::uint32_t itemId : trackIt->clipIds)
    {
        const PlaylistItemState* item = findPlaylistItem(itemId);
        if (item == nullptr || item->muted || item->type == PlaylistItemType::AutomationClip)
        {
            continue;
        }

        const ClipState* sourceClip = item->sourceClipId == 0 ? nullptr : findClipState(item->sourceClipId);
        if (sourceClip == nullptr && item->type == PlaylistItemType::AudioClip)
        {
            continue;
        }

        const std::uint64_t clipStartSample = static_cast<std::uint64_t>(item->startTimeSeconds * sampleRate);
        const std::uint64_t clipEndSample = clipStartSample + static_cast<std::uint64_t>(item->durationSeconds * sampleRate);
        const double trimStartSeconds = item->trimStartSeconds > 0.0
            ? item->trimStartSeconds
            : (sourceClip == nullptr ? 0.0 : sourceClip->trimStartSeconds);
        const double trimEndSeconds = item->trimEndSeconds > 0.0
            ? item->trimEndSeconds
            : (sourceClip == nullptr ? 0.0 : sourceClip->trimEndSeconds);
        const double fadeInSeconds = item->fadeInSeconds > 0.0
            ? item->fadeInSeconds
            : (sourceClip == nullptr ? 0.0 : sourceClip->fadeInSeconds);
        const double fadeOutSeconds = item->fadeOutSeconds > 0.0
            ? item->fadeOutSeconds
            : (sourceClip == nullptr ? 0.0 : sourceClip->fadeOutSeconds);

        for (std::uint32_t frame = 0; frame < destination.frameCount; ++frame)
        {
            const std::uint64_t projectSample = blockStartSample + frame;
            if (projectSample < clipStartSample || projectSample >= clipEndSample)
            {
                continue;
            }

            const double localTimeSeconds = static_cast<double>(projectSample - clipStartSample) / sampleRate;
            const double fadeGain = computeFadeGain(
                localTimeSeconds,
                item->durationSeconds,
                fadeInSeconds,
                fadeOutSeconds);
            const double totalPan = std::clamp(trackPan + channelPan + item->pan, -1.0, 1.0);
            const auto [leftPan, rightPan] = stereoPanGains(totalPan);
            const double totalGain =
                std::clamp(item->gain, 0.0, 4.0) *
                std::clamp(trackGainAutomation, 0.0, 2.0) *
                std::clamp(channelGain, 0.0, 2.0) *
                fadeGain;
            double leftSample = 0.0;
            double rightSample = 0.0;

            if (item->type == PlaylistItemType::PatternClip)
            {
                const int patternNumber = item->patternNumber != 0
                    ? item->patternNumber
                    : (sourceClip == nullptr ? 1 : sourceClip->patternNumber);
                const PatternState* pattern = findPatternState(patternNumber);
                const PatternLaneState* patternLane =
                    pattern == nullptr ? nullptr : findPatternLane(*pattern, trackId);

                if (patternLane != nullptr)
                {
                    const double secondsPerBeat = 60.0 / std::max(1.0, transportInfo_.tempoBpm);
                    const double patternDurationSeconds =
                        static_cast<double>(std::max(1, pattern->lengthInBars)) * 4.0 * secondsPerBeat;
                    double patternPlaybackSeconds = localTimeSeconds;

                    if (item->timeStretchEnabled && item->durationSeconds > 0.0 && patternDurationSeconds > 0.0)
                    {
                        patternPlaybackSeconds =
                            (localTimeSeconds / item->durationSeconds) *
                            patternDurationSeconds *
                            std::max(0.125, item->stretchRatio);
                    }

                    if (!item->timeStretchEnabled)
                    {
                        patternPlaybackSeconds = localTimeSeconds * std::max(0.125, item->stretchRatio);
                    }

                    if (item->loopEnabled && patternDurationSeconds > 0.0)
                    {
                        patternPlaybackSeconds = std::fmod(patternPlaybackSeconds, patternDurationSeconds);
                        if (patternPlaybackSeconds < 0.0)
                        {
                            patternPlaybackSeconds += patternDurationSeconds;
                        }
                    }

                    const double cellDurationSeconds =
                        patternDurationSeconds / static_cast<double>(std::max(1, kDefaultPlaylistCellCount));

                    auto renderVoice =
                        [&](int laneIndex, int stepIndex, int lengthCells, int velocity, bool accent, bool slide)
                    {
                        const double noteStartSeconds = static_cast<double>(std::max(0, stepIndex)) * cellDurationSeconds;
                        const double noteBodyDurationSeconds =
                            static_cast<double>(std::max(1, lengthCells)) * cellDurationSeconds;
                        const double noteEndSeconds =
                            noteStartSeconds +
                            noteBodyDurationSeconds +
                            std::max(0.0, settings.releaseMs / 1000.0);

                        if (patternPlaybackSeconds < noteStartSeconds || patternPlaybackSeconds >= noteEndSeconds)
                        {
                            return;
                        }

                        const double noteElapsedSeconds = patternPlaybackSeconds - noteStartSeconds;
                        const double envelopeGain =
                            computeEnvelopeGain(noteElapsedSeconds, noteBodyDurationSeconds, settings);
                        if (envelopeGain <= 0.0)
                        {
                            return;
                        }

                        const double velocityGain =
                            std::clamp(static_cast<double>(velocity) / 127.0, 0.05, 1.0);
                        const double accentGain = accent ? 1.12 : 1.0;
                        const double pitchSemitones =
                            settings.pitchSemitones +
                            item->pitchSemitones +
                            (sourceClip == nullptr ? 0.0 : sourceClip->pitchSemitones);
                        const double midiNote =
                            static_cast<double>(36 + std::clamp(laneIndex, 0, kDefaultPianoLaneCount - 1)) +
                            pitchSemitones;
                        const double frequency =
                            440.0 * std::pow(2.0, (midiNote - 69.0) / 12.0);
                        const double cutoffNorm = std::clamp(cutoffHz / 16000.0, 0.08, 1.0);

                        double voiceLeft = 0.0;
                        double voiceRight = 0.0;
                        bool renderedFromSamplerSource = false;

                        if (settings.generatorType == GeneratorType::Sampler && !settings.sampleFilePath.empty())
                        {
                            const std::shared_ptr<const DecodedWavData> samplerData =
                                getSamplerSourceData(settings.sampleFilePath);
                            if (samplerData != nullptr && !samplerData->left.empty())
                            {
                                const double pitchRatio = std::pow(2.0, ((midiNote - 60.0) / 12.0));
                                double sampleIndex =
                                    noteElapsedSeconds *
                                    static_cast<double>(samplerData->sampleRate) *
                                    pitchRatio;
                                if (settings.reverse)
                                {
                                    sampleIndex = static_cast<double>(samplerData->left.size() - 1) - sampleIndex;
                                }
                                voiceLeft = linearSampleAt(samplerData->left, sampleIndex);
                                voiceRight = linearSampleAt(samplerData->right, sampleIndex);
                                renderedFromSamplerSource = true;
                            }
                        }

                        if (!renderedFromSamplerSource)
                        {
                            const double phase = kTwoPi * frequency * noteElapsedSeconds;

                            if (settings.generatorType == GeneratorType::Sampler)
                            {
                                const double transient = std::exp(-noteElapsedSeconds * 10.0);
                                const double body = std::sin(phase) * 0.68;
                                const double overtone = std::sin(phase * 2.0) * 0.18 * cutoffNorm;
                                const double low = std::sin(phase * 0.5) * 0.14;
                                const double wave = (body + overtone + low) * transient;
                                voiceLeft = wave;
                                voiceRight = wave;
                            }
                            else if (settings.generatorType == GeneratorType::PluginInstrument)
                            {
                                const double wave =
                                    (0.42 * std::sin(phase)) +
                                    (0.28 * std::asin(std::sin(phase * 1.01))) * cutoffNorm +
                                    (0.20 * std::sin(phase * 2.0)) +
                                    (slide ? 0.08 * std::sin(phase * 0.5) : 0.0);
                                voiceLeft = wave;
                                voiceRight = wave * (0.96 + (0.04 * cutoffNorm));
                            }
                            else
                            {
                                const double wave =
                                    (0.54 * std::sin(phase)) +
                                    (0.24 * std::sin(phase * 2.0) * cutoffNorm) +
                                    (0.14 * std::sin(phase * 3.0) * settings.resonance) +
                                    (0.12 * std::sin(phase * 0.5)) +
                                    (slide ? 0.05 * std::sin(phase * 1.5) : 0.0);
                                voiceLeft = wave;
                                voiceRight = wave;
                            }
                        }

                        const double voiceGain = envelopeGain * velocityGain * accentGain * 0.75;
                        leftSample += voiceLeft * voiceGain;
                        rightSample += voiceRight * voiceGain;
                    };

                    for (const auto& note : patternLane->notes)
                    {
                        renderVoice(
                            note.lane,
                            note.step,
                            note.length,
                            note.velocity,
                            note.accent,
                            note.slide);
                    }

                    for (std::size_t stepIndex = 0; stepIndex < patternLane->steps.size(); ++stepIndex)
                    {
                        const StepState& step = patternLane->steps[stepIndex];
                        if (!step.enabled)
                        {
                            continue;
                        }

                        const int rootLane = 8 + static_cast<int>((trackId + patternNumber) % 10);
                        renderVoice(
                            rootLane,
                            static_cast<int>(stepIndex),
                            1,
                            std::max(1, step.velocity),
                            step.velocity >= 112,
                            false);
                    }
                }
                else
                {
                    const double phase = kTwoPi * 110.0 * localTimeSeconds;
                    leftSample = std::sin(phase) * 0.35;
                    rightSample = leftSample;
                }
            }
            else if (sourceClip != nullptr && sourceClip->sourceType == ClipSourceType::AudioFile)
            {
                const std::shared_ptr<const DecodedWavData> audioData = getSamplerSourceData(sourceClip->filePath);
                if (audioData != nullptr && !audioData->left.empty())
                {
                    const double rawSourceDurationSeconds =
                        static_cast<double>(audioData->left.size()) /
                        static_cast<double>(std::max(1u, audioData->sampleRate));
                    const double sourcePlayableDurationSeconds =
                        std::max(0.001, rawSourceDurationSeconds - trimStartSeconds - trimEndSeconds);
                    double sourcePlaybackSeconds = localTimeSeconds;

                    if (item->timeStretchEnabled && item->durationSeconds > 0.0)
                    {
                        sourcePlaybackSeconds =
                            (localTimeSeconds / item->durationSeconds) *
                            sourcePlayableDurationSeconds;
                    }
                    else
                    {
                        sourcePlaybackSeconds = localTimeSeconds * std::max(0.125, item->stretchRatio);
                    }

                    const double pitchRatio = std::pow(2.0, item->pitchSemitones / 12.0);
                    sourcePlaybackSeconds *= std::max(0.125, pitchRatio);

                    if (!item->loopEnabled && sourcePlaybackSeconds >= sourcePlayableDurationSeconds)
                    {
                        continue;
                    }

                    if (item->loopEnabled && sourcePlayableDurationSeconds > 0.0)
                    {
                        sourcePlaybackSeconds = std::fmod(sourcePlaybackSeconds, sourcePlayableDurationSeconds);
                        if (sourcePlaybackSeconds < 0.0)
                        {
                            sourcePlaybackSeconds += sourcePlayableDurationSeconds;
                        }
                    }

                    const double sourceSampleIndex =
                        (trimStartSeconds + sourcePlaybackSeconds) *
                        static_cast<double>(audioData->sampleRate);
                    leftSample = linearSampleAt(audioData->left, sourceSampleIndex);
                    rightSample = linearSampleAt(audioData->right, sourceSampleIndex);
                }
            }

            destination.left[frame] += leftSample * leftPan * totalGain;
            destination.right[frame] += rightSample * rightPan * totalGain;
        }
    }
}

void AudioEngine::applyNodeDelayCompensation(std::uint32_t nodeId, AudioBuffer& buffer)
{
    if (!config_.enablePdc)
    {
        return;
    }

    auto delayIt = delayLines_.find(nodeId);
    if (delayIt == delayLines_.end())
    {
        return;
    }

    DelayLine& delayLine = delayIt->second;
    if (delayLine.delaySamples == 0 || delayLine.left.empty())
    {
        return;
    }

    const std::uint32_t capacity = static_cast<std::uint32_t>(delayLine.left.size());
    for (std::uint32_t frame = 0; frame < buffer.frameCount; ++frame)
    {
        const std::uint32_t readIndex =
            (delayLine.writeIndex + capacity - delayLine.delaySamples) % capacity;

        const double delayedLeft = delayLine.left[readIndex];
        const double delayedRight = delayLine.right[readIndex];

        delayLine.left[delayLine.writeIndex] = buffer.left[frame];
        delayLine.right[delayLine.writeIndex] = buffer.right[frame];
        delayLine.writeIndex = (delayLine.writeIndex + 1) % capacity;

        buffer.left[frame] = delayedLeft;
        buffer.right[frame] = delayedRight;
    }
}

void AudioEngine::mixBuffer(AudioBuffer& destination, const AudioBuffer& source, double gain)
{
    if (destination.frameCount == 0)
    {
        destination.resize(source.frameCount);
    }

    const std::uint32_t frameCount = std::min(destination.frameCount, source.frameCount);
    for (std::uint32_t frame = 0; frame < frameCount; ++frame)
    {
        destination.left[frame] += source.left[frame] * gain;
        destination.right[frame] += source.right[frame] * gain;
    }
}

void AudioEngine::mixTrackToBus(AudioBuffer& destination, std::uint32_t trackId, const RenderContext& renderContext)
{
    renderTrackClips(trackId, renderContext, destination);
}

void AudioEngine::queueClipReadAhead(const RenderContext& renderContext)
{
    const double blockStartSeconds =
        static_cast<double>(renderContext.callback.transportSampleStart) /
        static_cast<double>(std::max(1u, renderContext.callback.sampleRate));
    const double lookAheadSeconds =
        blockStartSeconds +
        (static_cast<double>(renderContext.callback.blockSize * std::max(1, config_.anticipativePrefetchBlocks)) /
         static_cast<double>(std::max(1u, renderContext.callback.sampleRate)));

    for (const auto& item : projectState_.playlistItems)
    {
        if (item.type != PlaylistItemType::AudioClip || item.sourceClipId == 0)
        {
            continue;
        }

        const ClipState* clip = findClipState(item.sourceClipId);
        if (clip == nullptr || clip->sourceType != ClipSourceType::AudioFile || clip->filePath.empty())
        {
            continue;
        }

        if (item.startTimeSeconds > lookAheadSeconds ||
            (item.startTimeSeconds + item.durationSeconds) < blockStartSeconds)
        {
            continue;
        }

        bool alreadyCached = false;
        {
            std::lock_guard<std::mutex> cacheLock(clipCacheMutex_);
            alreadyCached = std::any_of(
                clipCache_.begin(),
                clipCache_.end(),
                [&](const ClipCacheEntry& entry)
                {
                    return entry.clipId == clip->clipId && entry.complete;
                });
        }

        if (alreadyCached)
        {
            continue;
        }

        {
            std::lock_guard<std::mutex> queueLock(diskQueueMutex_);
            const bool alreadyQueued = std::any_of(
                diskReadQueue_.begin(),
                diskReadQueue_.end(),
                [&](const DiskReadRequest& request)
                {
                    return request.clipId == clip->clipId;
                });

            if (!alreadyQueued)
            {
                diskReadQueue_.push_back(DiskReadRequest{
                    clip->clipId,
                    clip->filePath,
                    0,
                    static_cast<std::uint32_t>(item.durationSeconds * static_cast<double>(renderContext.callback.sampleRate))});
            }
        }

        diskQueueCv_.notify_one();
    }
}

void AudioEngine::diskWorkerMain()
{
    while (diskWorkerRunning_.load())
    {
        DiskReadRequest request{};

        {
            std::unique_lock<std::mutex> lock(diskQueueMutex_);
            diskQueueCv_.wait(
                lock,
                [this]()
                {
                    return !diskWorkerRunning_.load() || !diskReadQueue_.empty();
                });

            if (!diskWorkerRunning_.load() && diskReadQueue_.empty())
            {
                return;
            }

            request = diskReadQueue_.front();
            diskReadQueue_.pop_front();
        }

        loadClipIntoCache(request);
    }
}

bool AudioEngine::loadClipIntoCache(const DiskReadRequest& request)
{
    if (request.filePath.empty())
    {
        return false;
    }

    DecodedWavData decoded{};
    if (!decodeWavFile(request.filePath, decoded))
    {
        return false;
    }

    ClipCacheEntry entry{};
    entry.clipId = request.clipId;
    entry.filePath = request.filePath;
    entry.cachedOffsetFrames = 0;
    entry.sampleRate = decoded.sampleRate;
    entry.left = std::move(decoded.left);
    entry.right = std::move(decoded.right);
    entry.complete = true;
    entry.lastAccess = Clock::now();

    std::lock_guard<std::mutex> cacheLock(clipCacheMutex_);
    auto existing = std::find_if(
        clipCache_.begin(),
        clipCache_.end(),
        [&](const ClipCacheEntry& candidate) { return candidate.clipId == request.clipId; });

    if (existing == clipCache_.end())
    {
        clipCache_.push_back(std::move(entry));
    }
    else
    {
        *existing = std::move(entry);
    }

    metrics_.cachedClipCount = static_cast<std::uint32_t>(clipCache_.size());
    return true;
}

bool AudioEngine::writeWavFile(const std::string& path, std::uint32_t sampleRate, const AudioBuffer& buffer)
{
    std::filesystem::path outputPath = path;
    if (outputPath.has_parent_path())
    {
        std::error_code ec;
        std::filesystem::create_directories(outputPath.parent_path(), ec);
    }

    std::ofstream stream(path, std::ios::binary);
    if (!stream.is_open())
    {
        return false;
    }

    const std::uint16_t channelCount = 2;
    const std::uint16_t bitsPerSample = 32;
    const std::uint32_t byteRate = sampleRate * channelCount * (bitsPerSample / 8);
    const std::uint16_t blockAlign = channelCount * (bitsPerSample / 8);
    const std::uint32_t dataSize = buffer.frameCount * blockAlign;
    const std::uint32_t riffSize = 36 + dataSize;
    const std::uint16_t formatTag = WAVE_FORMAT_IEEE_FLOAT;

    stream.write("RIFF", 4);
    stream.write(reinterpret_cast<const char*>(&riffSize), sizeof(riffSize));
    stream.write("WAVE", 4);
    stream.write("fmt ", 4);

    const std::uint32_t fmtSize = 16;
    stream.write(reinterpret_cast<const char*>(&fmtSize), sizeof(fmtSize));
    stream.write(reinterpret_cast<const char*>(&formatTag), sizeof(formatTag));
    stream.write(reinterpret_cast<const char*>(&channelCount), sizeof(channelCount));
    stream.write(reinterpret_cast<const char*>(&sampleRate), sizeof(sampleRate));
    stream.write(reinterpret_cast<const char*>(&byteRate), sizeof(byteRate));
    stream.write(reinterpret_cast<const char*>(&blockAlign), sizeof(blockAlign));
    stream.write(reinterpret_cast<const char*>(&bitsPerSample), sizeof(bitsPerSample));
    stream.write("data", 4);
    stream.write(reinterpret_cast<const char*>(&dataSize), sizeof(dataSize));

    for (std::uint32_t frame = 0; frame < buffer.frameCount; ++frame)
    {
        const float left = static_cast<float>(clampSample(buffer.left[frame]));
        const float right = static_cast<float>(clampSample(buffer.right[frame]));
        stream.write(reinterpret_cast<const char*>(&left), sizeof(left));
        stream.write(reinterpret_cast<const char*>(&right), sizeof(right));
    }

    return stream.good();
}

bool AudioEngine::spawnPluginSandbox(PluginInstance& instance)
{
    if (!config_.enablePluginSandbox || config_.safeMode)
    {
        return false;
    }

    char currentExePath[MAX_PATH]{};
    GetModuleFileNameA(nullptr, currentExePath, MAX_PATH);

    std::string pluginHostPath = currentExePath;
    std::filesystem::path siblingHost = std::filesystem::path(currentExePath).parent_path() / "DAWCloudPluginHost.exe";
    if (std::filesystem::exists(siblingHost))
    {
        pluginHostPath = siblingHost.string();
    }

    const std::string mappingName =
        "DAWCloudSandbox_" +
        std::to_string(instance.instanceId) +
        "_" +
        std::to_string(GetTickCount64());

    HANDLE mappingHandle = CreateFileMappingA(
        INVALID_HANDLE_VALUE,
        nullptr,
        PAGE_READWRITE,
        0,
        sizeof(SandboxSharedState),
        mappingName.c_str());

    if (mappingHandle == nullptr)
    {
        return false;
    }

    void* mappingView = MapViewOfFile(mappingHandle, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SandboxSharedState));
    if (mappingView == nullptr)
    {
        CloseHandle(mappingHandle);
        return false;
    }

    auto* sharedState = reinterpret_cast<SandboxSharedState*>(mappingView);
    *sharedState = SandboxSharedState{};
    UnmapViewOfFile(mappingView);

    std::string commandLine =
        quoteArgument(pluginHostPath) +
        " --plugin-host --mapping=" + quoteArgument(mappingName) +
        " --plugin=" + quoteArgument(instance.descriptor.name);

    STARTUPINFOA startupInfo{};
    startupInfo.cb = sizeof(startupInfo);
    PROCESS_INFORMATION processInfo{};

    const BOOL created = CreateProcessA(
        nullptr,
        commandLine.data(),
        nullptr,
        nullptr,
        FALSE,
        CREATE_NO_WINDOW,
        nullptr,
        nullptr,
        &startupInfo,
        &processInfo);

    if (!created)
    {
        CloseHandle(mappingHandle);
        return false;
    }

    instance.sandbox.enabled = true;
    instance.sandbox.alive = true;
    instance.sandbox.executablePath = pluginHostPath;
    instance.sandbox.sharedMemoryName = mappingName;
    instance.sandbox.mappingHandle = reinterpret_cast<std::uintptr_t>(mappingHandle);
    instance.sandbox.processHandle = reinterpret_cast<std::uintptr_t>(processInfo.hProcess);
    instance.sandbox.threadHandle = reinterpret_cast<std::uintptr_t>(processInfo.hThread);
    instance.sandbox.lastHeartbeatTime = Clock::now();
    return true;
}

void AudioEngine::shutdownPluginSandbox(PluginInstance& instance)
{
    if (!instance.sandbox.enabled)
    {
        return;
    }

    if (instance.sandbox.mappingHandle != 0)
    {
        HANDLE mappingHandle = reinterpret_cast<HANDLE>(instance.sandbox.mappingHandle);
        void* view = MapViewOfFile(mappingHandle, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SandboxSharedState));
        if (view != nullptr)
        {
            auto* shared = reinterpret_cast<SandboxSharedState*>(view);
            shared->stopRequested = 1;
            UnmapViewOfFile(view);
        }
    }

    if (instance.sandbox.processHandle != 0)
    {
        HANDLE processHandle = reinterpret_cast<HANDLE>(instance.sandbox.processHandle);
        WaitForSingleObject(processHandle, 1000);
        safeCloseHandle(instance.sandbox.processHandle);
    }

    safeCloseHandle(instance.sandbox.threadHandle);
    safeCloseHandle(instance.sandbox.mappingHandle);
    instance.sandbox.enabled = false;
    instance.sandbox.alive = false;
}

void AudioEngine::serviceSandboxWatchdogs()
{
    std::lock_guard<std::mutex> pluginLock(pluginMutex_);
    for (auto& plugin : loadedPlugins_)
    {
        if (!plugin.sandbox.enabled || plugin.sandbox.mappingHandle == 0)
        {
            continue;
        }

        HANDLE processHandle = reinterpret_cast<HANDLE>(plugin.sandbox.processHandle);
        if (processHandle != nullptr && WaitForSingleObject(processHandle, 0) == WAIT_OBJECT_0)
        {
            plugin.active = false;
            plugin.autoBypassed = true;
            plugin.sandbox.alive = false;
            plugin.sandbox.crashRecovered = true;
            continue;
        }

        HANDLE mappingHandle = reinterpret_cast<HANDLE>(plugin.sandbox.mappingHandle);
        void* view = MapViewOfFile(mappingHandle, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(SandboxSharedState));
        if (view == nullptr)
        {
            continue;
        }

        auto* shared = reinterpret_cast<SandboxSharedState*>(view);
        const std::int32_t heartbeat = shared->heartbeat;
        if (heartbeat != plugin.sandbox.lastHeartbeat)
        {
            plugin.sandbox.lastHeartbeat = heartbeat;
            plugin.sandbox.watchdogMisses = 0;
            plugin.sandbox.lastHeartbeatTime = Clock::now();
        }
        else
        {
            ++plugin.sandbox.watchdogMisses;
        }

        if (plugin.sandbox.watchdogMisses >= kSandboxWatchdogThreshold)
        {
            plugin.active = false;
            plugin.autoBypassed = true;
            plugin.sandbox.alive = false;
            plugin.sandbox.crashRecovered = true;
            shared->stopRequested = 1;
        }

        UnmapViewOfFile(view);
    }
}

void AudioEngine::serviceClipCacheEviction()
{
    std::lock_guard<std::mutex> cacheLock(clipCacheMutex_);
    if (clipCache_.size() <= 8)
    {
        return;
    }

    const auto now = Clock::now();
    clipCache_.erase(
        std::remove_if(
            clipCache_.begin(),
            clipCache_.end(),
            [&](const ClipCacheEntry& entry)
            {
                const auto age = std::chrono::duration_cast<std::chrono::seconds>(now - entry.lastAccess).count();
                return age > 30;
            }),
        clipCache_.end());

    metrics_.cachedClipCount = static_cast<std::uint32_t>(clipCache_.size());
}

void AudioEngine::pushUndoState(const std::string& description, const ProjectState& beforeState)
{
    undoStack_.push_back(ProjectAction{description, beforeState, projectState_});
    redoStack_.clear();
}

std::uint32_t AudioEngine::nextTrackId() const
{
    std::uint32_t maxId = 0;
    for (const auto& track : projectState_.tracks)
    {
        maxId = std::max(maxId, track.trackId);
    }
    return maxId + 1;
}

std::uint32_t AudioEngine::nextBusId() const
{
    std::uint32_t maxId = 0;
    for (const auto& bus : projectState_.buses)
    {
        maxId = std::max(maxId, bus.busId);
    }
    return maxId + 1;
}

std::uint32_t AudioEngine::nextClipId() const
{
    std::uint32_t maxId = 0;
    for (const auto& clip : projectState_.clips)
    {
        maxId = std::max(maxId, clip.clipId);
    }
    return maxId + 1;
}

std::uint32_t AudioEngine::nextPlaylistItemId() const
{
    std::uint32_t maxId = 0;
    for (const auto& item : projectState_.playlistItems)
    {
        if (item.type == PlaylistItemType::AutomationClip || item.itemId >= 4000)
        {
            continue;
        }
        maxId = std::max(maxId, item.itemId);
    }
    return std::max<std::uint32_t>(1, maxId + 1);
}

std::uint32_t AudioEngine::nextPluginInstanceId() const
{
    std::uint32_t maxId = 0;
    std::lock_guard<std::mutex> pluginLock(pluginMutex_);
    for (const auto& plugin : loadedPlugins_)
    {
        maxId = std::max(maxId, plugin.instanceId);
    }
    return maxId + 1;
}

std::uint32_t AudioEngine::nextAutomationClipId() const
{
    std::uint32_t maxId = 4000;
    for (const auto& automationClip : projectState_.automationClips)
    {
        maxId = std::max(maxId, automationClip.clipId);
    }
    return maxId + 1;
}

void AudioEngine::processPendingCommands()
{
    EngineCommand command{};
    while (consumeNextCommand(command))
    {
        switch (command.type)
        {
        case CommandType::NewProject:
            newProject(command.textValue);
            break;

        case CommandType::StartEngine:
            start();
            break;

        case CommandType::StopEngine:
            stop();
            break;

        case CommandType::RecoverAudioDevice:
            recoverAudioDevice();
            break;

        case CommandType::PlayTransport:
            play();
            break;

        case CommandType::PauseTransport:
            pause();
            break;

        case CommandType::StopTransport:
            stopTransport();
            break;

        case CommandType::RebuildGraph:
            requestGraphRebuild();
            break;

        case CommandType::RecompileGraph:
            compileGraph();
            break;

        case CommandType::SetTempo:
            setTempo(command.doubleValue);
            break;

        case CommandType::SetTimelinePosition:
            setTimelinePosition(command.doubleValue);
            break;

        case CommandType::SetSamplePosition:
            setSamplePosition(command.uintValue);
            break;

        case CommandType::ConfigureTransportLoop:
            configureTransportLoop(command.uintValue, command.secondaryUintValue, command.boolValue);
            break;

        case CommandType::ToggleAnticipativeProcessing:
            config_.enableAnticipativeProcessing = !config_.enableAnticipativeProcessing;
            publishSnapshot();
            break;

        case CommandType::ToggleAutomation:
            config_.enableSampleAccurateAutomation = !config_.enableSampleAccurateAutomation;
            publishSnapshot();
            break;

        case CommandType::TogglePdc:
            config_.enablePdc = !config_.enablePdc;
            recalculateLatencyModel();
            publishSnapshot();
            break;

        case CommandType::RenderOffline:
        {
            OfflineRenderRequest request{};
            request.startTimeSeconds = 0.0;
            request.endTimeSeconds = 12.0;
            request.sampleRate = deviceState_.sampleRate;
            request.outputPath = command.textValue.empty() ? "offline_render.wav" : command.textValue;
            renderOffline(request);
            break;
        }

        case CommandType::FreezeTrack:
            freezeTrack(static_cast<std::uint32_t>(command.uintValue));
            break;

        case CommandType::AddTrack:
            addTrack(command.textValue);
            break;

        case CommandType::AddBus:
            addBus(command.textValue);
            break;

        case CommandType::AddPattern:
            addPattern(command.textValue);
            break;

        case CommandType::ClonePattern:
            clonePattern(static_cast<int>(command.uintValue));
            break;

        case CommandType::DeletePattern:
            deletePattern(static_cast<int>(command.uintValue));
            break;

        case CommandType::AddClipToTrack:
        {
            const double requestedDuration =
                command.secondaryTextValue.empty() ? -1.0 : std::stod(command.secondaryTextValue);
            addClipToTrack(
                static_cast<std::uint32_t>(command.uintValue),
                command.textValue,
                command.doubleValue,
                requestedDuration);
            break;
        }

        case CommandType::TogglePatternStep:
        {
            const std::vector<std::string> parts = splitString(command.textValue, '|');
            if (parts.size() >= 3)
            {
                togglePatternStep(
                    std::stoi(parts[0]),
                    static_cast<std::uint32_t>(std::stoul(parts[1])),
                    std::stoi(parts[2]));
            }
            break;
        }

        case CommandType::UpsertMidiNote:
        {
            const std::vector<std::string> parts = splitString(command.textValue, '|');
            if (parts.size() >= 10)
            {
                MidiNoteState note{};
                note.lane = std::stoi(parts[3]);
                note.step = std::stoi(parts[4]);
                note.length = std::stoi(parts[5]);
                note.velocity = std::stoi(parts[6]);
                note.accent = parseBoolToken(parts[7]);
                note.slide = parseBoolToken(parts[8]);
                note.selected = false;
                upsertMidiNote(
                    std::stoi(parts[0]),
                    static_cast<std::uint32_t>(std::stoul(parts[1])),
                    static_cast<std::size_t>(std::stoull(parts[2])),
                    note,
                    parseBoolToken(parts[9]));
            }
            break;
        }

        case CommandType::SetAutomationPoint:
        {
            const std::vector<std::string> parts = splitString(command.textValue, '|');
            if (parts.size() >= 4)
            {
                setAutomationPoint(
                    static_cast<std::uint32_t>(std::stoul(parts[0])),
                    std::stoi(parts[1]),
                    std::stoi(parts[2]),
                    std::stoi(parts[3]));
            }
            break;
        }

        case CommandType::MoveClip:
            moveClip(
                static_cast<std::uint32_t>(command.uintValue),
                static_cast<std::uint32_t>(command.secondaryUintValue),
                command.doubleValue,
                command.textValue.empty() ? -1.0 : std::strtod(command.textValue.c_str(), nullptr));
            break;

        case CommandType::SaveProject:
            saveProject(command.textValue.empty() ? config_.sessionPath : command.textValue);
            break;

        case CommandType::LoadProject:
            loadProject(command.textValue.empty() ? config_.sessionPath : command.textValue);
            break;

        case CommandType::UndoEdit:
            undoLastEdit();
            break;

        case CommandType::RedoEdit:
            redoLastEdit();
            break;

        case CommandType::None:
        default:
            break;
        }
    }
}

