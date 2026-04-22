bool AudioEngine::newProject(const std::string& name)
{
    clearError();

    config_.projectName = name.empty() ? "Untitled Project" : name;

    editableGraph_ = {};
    pendingGraph_ = {};
    compiledGraph_ = {};
    graphSwapPending_ = false;

    automationLanes_.clear();
    latencyStates_.clear();
    delayLines_.clear();

    {
        std::lock_guard<std::mutex> cacheLock(clipCacheMutex_);
        clipCache_.clear();
    }
    metrics_.cachedClipCount = 0;

    {
        std::lock_guard<std::mutex> diskLock(diskQueueMutex_);
        diskReadQueue_.clear();
    }

    undoStack_.clear();
    redoStack_.clear();
    lastOfflineRenderRequest_.reset();

    if (!initializeProjectState())
    {
        setError(kErrorProjectIo, "Failed to create a new project state.");
        return false;
    }

    transportInfo_.state = TransportState::Stopped;
    transportInfo_.timelineSeconds = 0.0;
    transportInfo_.samplePosition = 0;

    if (!buildInitialGraph() || !compileGraph())
    {
        setError(kErrorGraphBuild, "Failed to initialize graph for new project.");
        return false;
    }

    updateStatusFromState();
    publishSnapshot();
    return true;
}

bool AudioEngine::writeProjectFile(const std::string& path, bool updateSessionPath, bool clearDirtyState)
{
    const std::string resolvedPath = path.empty()
        ? (updateSessionPath ? config_.sessionPath : projectState_.autosavePath)
        : path;
    const std::string sessionPathForFile = updateSessionPath ? resolvedPath : projectState_.sessionPath;
    const std::string autosavePathForFile = updateSessionPath
        ? makeAutosavePathForSession(resolvedPath)
        : (projectState_.autosavePath.empty() ? makeAutosavePathForSession(projectState_.sessionPath) : projectState_.autosavePath);
    const std::string recoveryPathForFile = updateSessionPath
        ? autosavePathForFile
        : (projectState_.recoveryPath.empty() ? autosavePathForFile : projectState_.recoveryPath);
    std::filesystem::path sessionPath = resolvedPath;

    if (sessionPath.has_parent_path())
    {
        std::error_code ec;
        std::filesystem::create_directories(sessionPath.parent_path(), ec);
    }

    std::ofstream stream(resolvedPath, std::ios::binary);
    if (!stream.is_open())
    {
        setError(kErrorProjectIo, "Failed to open session file for writing.");
        return false;
    }

    stream << "PROJECT|" << projectState_.projectName << '|' << sessionPathForFile << '|' << projectState_.revision << '\n';
    stream
        << "PROJECTMETA|" << (projectState_.autosaveEnabled ? 1 : 0) << '|'
        << autosavePathForFile << '|'
        << recoveryPathForFile << '|'
        << projectState_.lastSavedRevision << '|'
        << projectState_.lastAutosavedRevision << '|'
        << (projectState_.recoveryAvailable ? 1 : 0) << '\n';

    for (const auto& bus : projectState_.buses)
    {
        stream
            << "BUS|" << bus.busId << '|'
            << bus.name << '|'
            << bus.gain << '|'
            << bus.pan << '|'
            << (bus.muted ? 1 : 0) << '|'
            << (bus.solo ? 1 : 0) << '|'
            << bus.outputBusId << '\n';
    }

    for (const auto& track : projectState_.tracks)
    {
        stream
            << "TRACK|" << track.trackId << '|'
            << track.busId << '|'
            << track.name << '|'
            << (track.armed ? 1 : 0) << '|'
            << (track.muted ? 1 : 0) << '|'
            << (track.solo ? 1 : 0) << '|'
            << (track.recordEnabled ? 1 : 0) << '|'
            << track.gain << '|'
            << track.pan << '|'
            << track.routeTargetBusId << '|'
            << track.sendBusId << '|'
            << track.sendAmount << '\n';
    }

    for (const auto& clip : projectState_.clips)
    {
        stream
            << "CLIP|" << clip.clipId << '|'
            << clip.trackId << '|'
            << clip.name << '|'
            << static_cast<int>(clip.sourceType) << '|'
            << clip.filePath << '|'
            << clip.startTimeSeconds << '|'
            << clip.durationSeconds << '|'
            << clip.gain << '|'
            << (clip.muted ? 1 : 0) << '|'
            << clip.patternNumber << '|'
            << clip.trimStartSeconds << '|'
            << clip.trimEndSeconds << '|'
            << clip.fadeInSeconds << '|'
            << clip.fadeOutSeconds << '|'
            << clip.pan << '|'
            << clip.pitchSemitones << '|'
            << clip.stretchRatio << '|'
            << (clip.loopEnabled ? 1 : 0) << '|'
            << (clip.timeStretchEnabled ? 1 : 0) << '\n';
    }

    for (const auto& item : projectState_.playlistItems)
    {
        stream
            << "PLAYLIST|" << item.itemId << '|'
            << item.trackId << '|'
            << static_cast<int>(item.type) << '|'
            << item.sourceClipId << '|'
            << item.automationClipId << '|'
            << item.patternNumber << '|'
            << item.label << '|'
            << item.startTimeSeconds << '|'
            << item.durationSeconds << '|'
            << item.trimStartSeconds << '|'
            << item.trimEndSeconds << '|'
            << item.fadeInSeconds << '|'
            << item.fadeOutSeconds << '|'
            << item.gain << '|'
            << item.pan << '|'
            << item.pitchSemitones << '|'
            << item.stretchRatio << '|'
            << (item.muted ? 1 : 0) << '|'
            << (item.loopEnabled ? 1 : 0) << '|'
            << (item.timeStretchEnabled ? 1 : 0) << '\n';
    }

    for (const auto& marker : projectState_.markers)
    {
        stream << "MARKER|" << marker.markerId << '|' << marker.name << '|' << marker.timeSeconds << '\n';
    }

    for (const auto& pattern : projectState_.patterns)
    {
        stream
            << "PATTERN|" << pattern.patternNumber << '|'
            << pattern.name << '|'
            << pattern.lengthInBars << '|'
            << pattern.accentAmount << '\n';

        for (const auto& lane : pattern.lanes)
        {
            stream
                << "PATTERNLANE|" << pattern.patternNumber << '|'
                << lane.trackId << '|'
                << lane.swing << '|'
                << lane.shuffle << '\n';

            for (std::size_t stepIndex = 0; stepIndex < lane.steps.size(); ++stepIndex)
            {
                const StepState& step = lane.steps[stepIndex];
                stream
                    << "STEP|" << pattern.patternNumber << '|'
                    << lane.trackId << '|'
                    << stepIndex << '|'
                    << (step.enabled ? 1 : 0) << '|'
                    << step.velocity << '\n';
            }

            for (const auto& note : lane.notes)
            {
                stream
                    << "NOTE|" << pattern.patternNumber << '|'
                    << lane.trackId << '|'
                    << note.lane << '|'
                    << note.step << '|'
                    << note.length << '|'
                    << note.velocity << '|'
                    << (note.accent ? 1 : 0) << '|'
                    << (note.slide ? 1 : 0) << '\n';
            }
        }
    }

    for (const auto& settings : projectState_.channelSettings)
    {
        stream
            << "CHANNEL|" << settings.trackId << '|'
            << settings.name << '|'
            << static_cast<int>(settings.generatorType) << '|'
            << settings.sampleFilePath << '|'
            << settings.instrumentPluginName << '|'
            << settings.gain << '|'
            << settings.pan << '|'
            << settings.pitchSemitones << '|'
            << settings.attackMs << '|'
            << settings.decayMs << '|'
            << settings.sustainLevel << '|'
            << settings.releaseMs << '|'
            << settings.filterCutoffHz << '|'
            << settings.resonance << '|'
            << settings.mixerInsert << '|'
            << settings.routeTarget << '|'
            << (settings.reverse ? 1 : 0) << '|'
            << (settings.timeStretch ? 1 : 0) << '|'
            << joinString(settings.pluginRack, ',') << '|'
            << joinString(settings.presets, ',') << '\n';
    }

    for (const auto& automationClip : projectState_.automationClips)
    {
        stream
            << "AUTOCLIP|" << automationClip.clipId << '|'
            << automationClip.target << '|'
            << automationClip.trackId << '|'
            << automationClip.lane << '|'
            << automationClip.startCell << '|'
            << automationClip.lengthCells << '\n';

        for (const auto& point : automationClip.points)
        {
            stream
                << "AUTOPOINT|" << automationClip.clipId << '|'
                << point.cell << '|'
                << point.value << '|'
                << point.curve << '\n';
        }
    }

    if (updateSessionPath)
    {
        projectState_.sessionPath = resolvedPath;
        projectState_.autosavePath = makeAutosavePathForSession(resolvedPath);
        projectState_.recoveryPath = projectState_.autosavePath;
        config_.sessionPath = resolvedPath;
    }

    if (clearDirtyState)
    {
        projectState_.dirty = false;
        projectState_.lastSavedRevision = projectState_.revision;
        projectState_.recoveryAvailable = false;
    }
    else
    {
        projectState_.lastAutosavedRevision = projectState_.revision;
        projectState_.recoveryPath = resolvedPath;
        projectState_.recoveryAvailable = true;
    }

    publishSnapshot();
    return true;
}

void AudioEngine::maybeAutosaveProject()
{
    if (!projectState_.autosaveEnabled || !projectState_.dirty)
    {
        return;
    }

    if (projectState_.autosavePath.empty())
    {
        projectState_.autosavePath = makeAutosavePathForSession(projectState_.sessionPath);
    }

    if (projectState_.lastAutosavedRevision == projectState_.revision)
    {
        return;
    }

    writeProjectFile(projectState_.autosavePath, false, false);
}

bool AudioEngine::saveProject(const std::string& path)
{
    const std::string resolvedPath = path.empty() ? config_.sessionPath : path;
    return writeProjectFile(resolvedPath, true, true);
}

bool AudioEngine::loadProject(const std::string& path)
{
    const std::string resolvedPath = path.empty() ? config_.sessionPath : path;
    std::ifstream stream(resolvedPath, std::ios::binary);
    if (!stream.is_open())
    {
        setError(kErrorProjectIo, "Failed to open session file for reading.");
        return false;
    }

    ProjectState loadedProject{};
    loadedProject.sessionPath = resolvedPath;
    loadedProject.autosaveEnabled = true;
    loadedProject.autosavePath = makeAutosavePathForSession(resolvedPath);
    loadedProject.recoveryPath = loadedProject.autosavePath;

    try
    {
        std::string line;
        while (std::getline(stream, line))
        {
            if (line.empty())
            {
                continue;
            }

            const std::vector<std::string> parts = splitString(line, '|');
            if (parts.empty())
            {
                continue;
            }

            if (parts[0] == "PROJECT" && parts.size() >= 4)
            {
                loadedProject.projectName = parts[1];
                loadedProject.sessionPath = parts[2];
                loadedProject.revision = static_cast<std::uint64_t>(std::stoull(parts[3]));
            }
            else if (parts[0] == "PROJECTMETA" && parts.size() >= 7)
            {
                loadedProject.autosaveEnabled = parseBoolToken(parts[1]);
                loadedProject.autosavePath = parts[2];
                loadedProject.recoveryPath = parts[3];
                loadedProject.lastSavedRevision = static_cast<std::uint64_t>(std::stoull(parts[4]));
                loadedProject.lastAutosavedRevision = static_cast<std::uint64_t>(std::stoull(parts[5]));
                loadedProject.recoveryAvailable = parseBoolToken(parts[6]);
            }
            else if (parts[0] == "BUS" && parts.size() >= 3)
            {
                BusState bus{};
                bus.busId = static_cast<std::uint32_t>(std::stoul(parts[1]));
                bus.name = parts[2];
                if (parts.size() >= 8)
                {
                    bus.gain = std::stod(parts[3]);
                    bus.pan = std::stod(parts[4]);
                    bus.muted = parseBoolToken(parts[5]);
                    bus.solo = parseBoolToken(parts[6]);
                    bus.outputBusId = static_cast<std::uint32_t>(std::stoul(parts[7]));
                }
                loadedProject.buses.push_back(std::move(bus));
            }
            else if (parts[0] == "TRACK" && parts.size() >= 7)
            {
                TrackState track{};
                track.trackId = static_cast<std::uint32_t>(std::stoul(parts[1]));
                track.busId = static_cast<std::uint32_t>(std::stoul(parts[2]));
                track.name = parts[3];
                track.armed = parseBoolToken(parts[4]);
                track.muted = parseBoolToken(parts[5]);
                track.solo = parseBoolToken(parts[6]);
                if (parts.size() >= 13)
                {
                    track.recordEnabled = parseBoolToken(parts[7]);
                    track.gain = std::stod(parts[8]);
                    track.pan = std::stod(parts[9]);
                    track.routeTargetBusId = static_cast<std::uint32_t>(std::stoul(parts[10]));
                    track.sendBusId = static_cast<std::uint32_t>(std::stoul(parts[11]));
                    track.sendAmount = std::stod(parts[12]);
                }
                loadedProject.tracks.push_back(std::move(track));
            }
            else if (parts[0] == "CLIP" && parts.size() >= 10)
            {
                ClipState clip{};
                clip.clipId = static_cast<std::uint32_t>(std::stoul(parts[1]));
                clip.trackId = static_cast<std::uint32_t>(std::stoul(parts[2]));
                clip.name = parts[3];
                clip.sourceType = static_cast<ClipSourceType>(std::stoi(parts[4]));
                clip.filePath = parts[5];
                clip.startTimeSeconds = std::stod(parts[6]);
                clip.durationSeconds = std::stod(parts[7]);
                clip.gain = std::stod(parts[8]);
                clip.muted = parseBoolToken(parts[9]);
                if (parts.size() >= 19)
                {
                    clip.patternNumber = std::stoi(parts[10]);
                    clip.trimStartSeconds = std::stod(parts[11]);
                    clip.trimEndSeconds = std::stod(parts[12]);
                    clip.fadeInSeconds = std::stod(parts[13]);
                    clip.fadeOutSeconds = std::stod(parts[14]);
                    clip.pan = std::stod(parts[15]);
                    clip.pitchSemitones = std::stod(parts[16]);
                    clip.stretchRatio = std::stod(parts[17]);
                    clip.loopEnabled = parseBoolToken(parts[18]);
                    clip.timeStretchEnabled = parts.size() >= 20 ? parseBoolToken(parts[19]) : false;
                }
                loadedProject.clips.push_back(std::move(clip));
            }
            else if (parts[0] == "PLAYLIST" && parts.size() >= 20)
            {
                PlaylistItemState item{};
                item.itemId = static_cast<std::uint32_t>(std::stoul(parts[1]));
                item.trackId = static_cast<std::uint32_t>(std::stoul(parts[2]));
                item.type = static_cast<PlaylistItemType>(std::stoi(parts[3]));
                item.sourceClipId = static_cast<std::uint32_t>(std::stoul(parts[4]));
                item.automationClipId = static_cast<std::uint32_t>(std::stoul(parts[5]));
                item.patternNumber = std::stoi(parts[6]);
                item.label = parts[7];
                item.startTimeSeconds = std::stod(parts[8]);
                item.durationSeconds = std::stod(parts[9]);
                item.trimStartSeconds = std::stod(parts[10]);
                item.trimEndSeconds = std::stod(parts[11]);
                item.fadeInSeconds = std::stod(parts[12]);
                item.fadeOutSeconds = std::stod(parts[13]);
                item.gain = std::stod(parts[14]);
                item.pan = std::stod(parts[15]);
                item.pitchSemitones = std::stod(parts[16]);
                item.stretchRatio = std::stod(parts[17]);
                item.muted = parseBoolToken(parts[18]);
                item.loopEnabled = parseBoolToken(parts[19]);
                item.timeStretchEnabled = parts.size() >= 21 ? parseBoolToken(parts[20]) : false;
                loadedProject.playlistItems.push_back(std::move(item));
            }
            else if (parts[0] == "MARKER" && parts.size() >= 4)
            {
                loadedProject.markers.push_back(MarkerState{
                    static_cast<std::uint32_t>(std::stoul(parts[1])),
                    parts[2],
                    std::stod(parts[3])});
            }
            else if (parts[0] == "PATTERN" && parts.size() >= 5)
            {
                PatternState pattern{};
                pattern.patternNumber = std::stoi(parts[1]);
                pattern.name = parts[2];
                pattern.lengthInBars = std::stoi(parts[3]);
                pattern.accentAmount = std::stoi(parts[4]);
                loadedProject.patterns.push_back(std::move(pattern));
            }
            else if (parts[0] == "PATTERNLANE" && parts.size() >= 5)
            {
                const int patternNumber = std::stoi(parts[1]);
                auto patternIt = std::find_if(
                    loadedProject.patterns.begin(),
                    loadedProject.patterns.end(),
                    [&](const PatternState& pattern) { return pattern.patternNumber == patternNumber; });
                if (patternIt == loadedProject.patterns.end())
                {
                    PatternState pattern{};
                    pattern.patternNumber = patternNumber;
                    pattern.name = "Pattern " + std::to_string(patternNumber);
                    pattern.lengthInBars = 2;
                    pattern.accentAmount = 0;
                    loadedProject.patterns.push_back(std::move(pattern));
                    patternIt = loadedProject.patterns.end() - 1;
                }

                PatternLaneState lane{};
                lane.trackId = static_cast<std::uint32_t>(std::stoul(parts[2]));
                lane.swing = std::stoi(parts[3]);
                lane.shuffle = std::stoi(parts[4]);
                patternIt->lanes.push_back(std::move(lane));
            }
            else if (parts[0] == "STEP" && parts.size() >= 6)
            {
                const int patternNumber = std::stoi(parts[1]);
                const std::uint32_t trackId = static_cast<std::uint32_t>(std::stoul(parts[2]));
                auto patternIt = std::find_if(
                    loadedProject.patterns.begin(),
                    loadedProject.patterns.end(),
                    [&](const PatternState& pattern) { return pattern.patternNumber == patternNumber; });
                if (patternIt != loadedProject.patterns.end())
                {
                    auto laneIt = std::find_if(
                        patternIt->lanes.begin(),
                        patternIt->lanes.end(),
                        [&](const PatternLaneState& lane) { return lane.trackId == trackId; });
                    if (laneIt != patternIt->lanes.end())
                    {
                        const std::size_t stepIndex = static_cast<std::size_t>(std::stoul(parts[3]));
                        if (laneIt->steps.size() <= stepIndex)
                        {
                            laneIt->steps.resize(stepIndex + 1);
                        }
                        laneIt->steps[stepIndex] = StepState{parseBoolToken(parts[4]), std::stoi(parts[5])};
                    }
                }
            }
            else if (parts[0] == "NOTE" && parts.size() >= 9)
            {
                const int patternNumber = std::stoi(parts[1]);
                const std::uint32_t trackId = static_cast<std::uint32_t>(std::stoul(parts[2]));
                auto patternIt = std::find_if(
                    loadedProject.patterns.begin(),
                    loadedProject.patterns.end(),
                    [&](const PatternState& pattern) { return pattern.patternNumber == patternNumber; });
                if (patternIt != loadedProject.patterns.end())
                {
                    auto laneIt = std::find_if(
                        patternIt->lanes.begin(),
                        patternIt->lanes.end(),
                        [&](const PatternLaneState& lane) { return lane.trackId == trackId; });
                    if (laneIt != patternIt->lanes.end())
                    {
                        laneIt->notes.push_back(MidiNoteState{
                            std::stoi(parts[3]),
                            std::stoi(parts[4]),
                            std::stoi(parts[5]),
                            std::stoi(parts[6]),
                            parseBoolToken(parts[7]),
                            parseBoolToken(parts[8]),
                            false});
                    }
                }
            }
            else if (parts[0] == "CHANNEL" && parts.size() >= 15)
            {
                ChannelSettingsState settings{};
                settings.trackId = static_cast<std::uint32_t>(std::stoul(parts[1]));
                settings.name = parts[2];

                if (parts.size() >= 20)
                {
                    settings.generatorType = static_cast<GeneratorType>(std::stoi(parts[3]));
                    settings.sampleFilePath = parts[4];
                    settings.instrumentPluginName = parts[5];
                    settings.gain = std::stod(parts[6]);
                    settings.pan = std::stod(parts[7]);
                    settings.pitchSemitones = std::stod(parts[8]);
                    settings.attackMs = std::stod(parts[9]);
                    settings.decayMs = std::stod(parts[10]);
                    settings.sustainLevel = std::stod(parts[11]);
                    settings.releaseMs = std::stod(parts[12]);
                    settings.filterCutoffHz = std::stod(parts[13]);
                    settings.resonance = std::stod(parts[14]);
                    settings.mixerInsert = std::stoi(parts[15]);
                    settings.routeTarget = std::stoi(parts[16]);
                    settings.reverse = parseBoolToken(parts[17]);
                    settings.timeStretch = parseBoolToken(parts[18]);
                    settings.pluginRack = parts[19].empty() ? std::vector<std::string>{} : splitString(parts[19], ',');
                    settings.presets = parts.size() >= 21 && !parts[20].empty() ? splitString(parts[20], ',') : std::vector<std::string>{};
                }
                else
                {
                    settings.gain = std::stod(parts[3]);
                    settings.pan = std::stod(parts[4]);
                    settings.pitchSemitones = std::stod(parts[5]);
                    settings.attackMs = std::stod(parts[6]);
                    settings.releaseMs = std::stod(parts[7]);
                    settings.filterCutoffHz = std::stod(parts[8]);
                    settings.resonance = std::stod(parts[9]);
                    settings.mixerInsert = std::stoi(parts[10]);
                    settings.routeTarget = std::stoi(parts[11]);
                    settings.reverse = parseBoolToken(parts[12]);
                    settings.timeStretch = parseBoolToken(parts[13]);
                    settings.pluginRack = parts[14].empty() ? std::vector<std::string>{} : splitString(parts[14], ',');
                    settings.presets = parts.size() >= 16 && !parts[15].empty() ? splitString(parts[15], ',') : std::vector<std::string>{};
                }

                loadedProject.channelSettings.push_back(std::move(settings));
            }
            else if (parts[0] == "AUTOCLIP" && parts.size() >= 6)
            {
                AutomationClipState clip{};
                clip.clipId = static_cast<std::uint32_t>(std::stoul(parts[1]));
                clip.target = parts[2];
                if (parts.size() >= 7)
                {
                    clip.trackId = static_cast<std::uint32_t>(std::stoul(parts[3]));
                    clip.lane = std::stoi(parts[4]);
                    clip.startCell = std::stoi(parts[5]);
                    clip.lengthCells = std::stoi(parts[6]);
                }
                else
                {
                    clip.lane = std::stoi(parts[3]);
                    clip.startCell = std::stoi(parts[4]);
                    clip.lengthCells = std::stoi(parts[5]);
                }
                loadedProject.automationClips.push_back(std::move(clip));
            }
            else if (parts[0] == "AUTOPOINT" && parts.size() >= 4)
            {
                const std::uint32_t clipId = static_cast<std::uint32_t>(std::stoul(parts[1]));
                auto automationIt = std::find_if(
                    loadedProject.automationClips.begin(),
                    loadedProject.automationClips.end(),
                    [&](const AutomationClipState& clip) { return clip.clipId == clipId; });
                if (automationIt != loadedProject.automationClips.end())
                {
                    AutomationPointState point{};
                    point.cell = std::stoi(parts[2]);
                    point.value = std::stoi(parts[3]);
                    point.curve = parts.size() >= 5 ? std::stod(parts[4]) : 0.0;
                    automationIt->points.push_back(point);
                }
            }
        }
    }
    catch (const std::exception&)
    {
        setError(kErrorProjectIo, "Session file is malformed or contains unsupported values.");
        return false;
    }

    if (loadedProject.projectName.empty())
    {
        loadedProject.projectName = std::filesystem::path(resolvedPath).stem().string();
    }

    if (loadedProject.buses.empty())
    {
        BusState mainBus{};
        mainBus.busId = 1;
        mainBus.name = "Main Bus";
        loadedProject.buses.push_back(std::move(mainBus));
    }

    if (loadedProject.tracks.empty())
    {
        TrackState track{};
        track.trackId = 1;
        track.busId = loadedProject.buses.front().busId;
        track.name = "Track 1";
        loadedProject.tracks.push_back(std::move(track));
    }

    if (loadedProject.autosavePath.empty())
    {
        loadedProject.autosavePath = makeAutosavePathForSession(resolvedPath);
    }
    if (loadedProject.recoveryPath.empty())
    {
        loadedProject.recoveryPath = loadedProject.autosavePath;
    }

    for (auto& bus : loadedProject.buses)
    {
        bus.inputTrackIds.clear();
    }

    for (auto& track : loadedProject.tracks)
    {
        if (track.busId == 0)
        {
            track.busId = loadedProject.buses.front().busId;
        }

        track.clipIds.clear();

        auto busIt = std::find_if(
            loadedProject.buses.begin(),
            loadedProject.buses.end(),
            [&](const BusState& bus) { return bus.busId == track.busId; });

        if (busIt != loadedProject.buses.end())
        {
            busIt->inputTrackIds.push_back(track.trackId);
        }
    }

    projectState_ = std::move(loadedProject);
    synchronizeProjectMusicalState();
    projectState_.dirty = false;
    projectState_.revision = std::max<std::uint64_t>(1, projectState_.revision + 1);
    projectState_.lastSavedRevision = std::max(projectState_.lastSavedRevision, projectState_.revision);
    projectState_.recoveryAvailable =
        projectState_.autosaveEnabled &&
        !projectState_.autosavePath.empty() &&
        std::filesystem::exists(projectState_.autosavePath);
    undoStack_.clear();
    redoStack_.clear();

    config_.projectName = projectState_.projectName;
    config_.sessionPath = projectState_.sessionPath;

    transportInfo_.state = TransportState::Stopped;
    transportInfo_.timelineSeconds = 0.0;
    transportInfo_.samplePosition = 0;

    requestGraphRebuild();
    publishSnapshot();
    return true;
}

bool AudioEngine::undoLastEdit()
{
    if (undoStack_.empty())
    {
        return false;
    }

    const ProjectAction action = undoStack_.back();
    undoStack_.pop_back();
    redoStack_.push_back(action);
    projectState_ = action.before;
    projectState_.dirty = true;
    ++projectState_.revision;
    synchronizeProjectMusicalState();
    requestGraphRebuild();
    publishSnapshot();
    return true;
}

bool AudioEngine::redoLastEdit()
{
    if (redoStack_.empty())
    {
        return false;
    }

    const ProjectAction action = redoStack_.back();
    redoStack_.pop_back();
    undoStack_.push_back(action);
    projectState_ = action.after;
    projectState_.dirty = true;
    ++projectState_.revision;
    synchronizeProjectMusicalState();
    requestGraphRebuild();
    publishSnapshot();
    return true;
}

bool AudioEngine::startTelemetry()
{
    if (telemetryRunning_.load())
    {
        return true;
    }

    telemetryRunning_.store(true);
    audioThreadState_.maintenanceRunning.store(true);
    maintenanceThread_ = std::thread(&AudioEngine::maintenanceThreadMain, this);
    return true;
}

void AudioEngine::stopTelemetry()
{
    telemetryRunning_.store(false);
    audioThreadState_.maintenanceRunning.store(false);

    if (maintenanceThread_.joinable())
    {
        maintenanceThread_.join();
    }
}

void AudioEngine::postCommand(const EngineCommand& command)
{
    if (!commandQueue_.push(command))
    {
        ++metrics_.commandQueueDrops;
    }
}

bool AudioEngine::consumeNextCommand(EngineCommand& outCommand)
{
    return commandQueue_.pop(outCommand);
}

void AudioEngine::setError(int errorCode, const std::string& message)
{
    lastErrorCode_ = errorCode;
    lastErrorMessage_ = message;
    state_.store(EngineState::Error);
    updateStatusFromState();
    publishSnapshot();
}

void AudioEngine::clearError()
{
    lastErrorCode_ = 0;
    lastErrorMessage_.clear();
}

void AudioEngine::resetMetrics()
{
    metrics_ = {};
}

void AudioEngine::updateStatusFromState()
{
    if (state_.load() != EngineState::Error && !initialized_.load())
    {
        state_.store(EngineState::Uninitialized);
    }
}

void AudioEngine::publishSnapshot()
{
    std::lock_guard<std::mutex> snapshotLock(snapshotWriteMutex_);
    const int currentIndex = activeSnapshotIndex_.load(std::memory_order_acquire);
    const int nextIndex = 1 - currentIndex;

    EngineSnapshot snapshot{};
    snapshot.engineState = state_.load();
    snapshot.config = config_;
    snapshot.metrics = metrics_;
    snapshot.transport = transportInfo_;
    snapshot.device = deviceState_;
    snapshot.project.state = projectState_;
    snapshot.project.undoDepth = static_cast<std::uint32_t>(undoStack_.size());
    snapshot.project.redoDepth = static_cast<std::uint32_t>(redoStack_.size());
    snapshot.project.graphVersion = metrics_.activeGraphVersion;
    snapshot.lastErrorCode = lastErrorCode_;
    snapshot.lastErrorMessage = lastErrorMessage_;

    {
        std::lock_guard<std::mutex> cacheLock(clipCacheMutex_);
        snapshot.metrics.cachedClipCount = static_cast<std::uint32_t>(clipCache_.size());
    }

    std::ostringstream status;
    status
        << "Audio Engine: " << toString(snapshot.engineState)
        << " | Backend: " << snapshot.device.backendName
        << " | Device: " << snapshot.device.deviceName
        << " | SR: " << snapshot.device.sampleRate
        << " | Block: " << snapshot.device.blockSize
        << " | Graph v" << snapshot.metrics.activeGraphVersion
        << " | Project: " << snapshot.project.state.projectName;
    snapshot.statusText = status.str();

    uiSnapshots_[nextIndex] = std::move(snapshot);
    activeSnapshotIndex_.store(nextIndex, std::memory_order_release);
}

bool AudioEngine::initializeProjectState()
{
    projectState_ = {};
    projectState_.projectName = config_.projectName;
    projectState_.sessionPath = config_.sessionPath;
    projectState_.revision = 1;
    projectState_.dirty = false;
    projectState_.autosaveEnabled = true;
    projectState_.autosavePath = makeAutosavePathForSession(projectState_.sessionPath);
    projectState_.recoveryPath = projectState_.autosavePath;
    projectState_.lastSavedRevision = projectState_.revision;

    BusState mainBus{};
    mainBus.busId = 1;
    mainBus.name = "Main Bus";
    mainBus.gain = 1.0;
    mainBus.pan = 0.0;

    TrackState track{};
    track.trackId = 1;
    track.busId = mainBus.busId;
    track.name = "Track 1";
    track.gain = 0.92;
    track.pan = 0.0;
    track.recordEnabled = false;

    ClipState clip{};
    clip.clipId = 1;
    clip.trackId = track.trackId;
    clip.name = "Demo Clip";
    clip.sourceType = ClipSourceType::GeneratedTone;
    clip.patternNumber = 1;
    clip.startTimeSeconds = 0.0;
    clip.durationSeconds = 8.0;
    clip.gain = 0.7;
    clip.loopEnabled = true;
    clip.timeStretchEnabled = true;

    PlaylistItemState item{};
    item.itemId = 1;
    item.trackId = track.trackId;
    item.type = PlaylistItemType::PatternClip;
    item.sourceClipId = clip.clipId;
    item.patternNumber = clip.patternNumber;
    item.label = "Pattern 1";
    item.startTimeSeconds = 0.0;
    item.durationSeconds = 8.0;
    item.gain = 0.84;
    item.loopEnabled = true;
    item.timeStretchEnabled = true;

    track.clipIds.push_back(item.itemId);
    mainBus.inputTrackIds.push_back(track.trackId);

    projectState_.buses.push_back(mainBus);
    projectState_.tracks.push_back(track);
    projectState_.clips.push_back(clip);
    projectState_.playlistItems.push_back(item);
    projectState_.markers.push_back(MarkerState{1, "Start", 0.0});
    synchronizeProjectMusicalState();
    return true;
}

void AudioEngine::synchronizeProjectMusicalState()
{
    if (projectState_.autosavePath.empty())
    {
        projectState_.autosavePath = makeAutosavePathForSession(projectState_.sessionPath);
    }
    if (projectState_.recoveryPath.empty())
    {
        projectState_.recoveryPath = projectState_.autosavePath;
    }

    if (projectState_.playlistItems.empty())
    {
        for (const auto& clip : projectState_.clips)
        {
            PlaylistItemState item{};
            item.itemId = nextPlaylistItemId();
            item.trackId = clip.trackId;
            item.type = clip.sourceType == ClipSourceType::AudioFile
                ? PlaylistItemType::AudioClip
                : PlaylistItemType::PatternClip;
            item.sourceClipId = clip.clipId;
            item.patternNumber = clip.patternNumber > 0 ? clip.patternNumber : extractPatternNumberFromClipName(clip.name);
            item.label = clip.name;
            item.startTimeSeconds = clip.startTimeSeconds;
            item.durationSeconds = clip.durationSeconds;
            item.trimStartSeconds = clip.trimStartSeconds;
            item.trimEndSeconds = clip.trimEndSeconds;
            item.fadeInSeconds = clip.fadeInSeconds;
            item.fadeOutSeconds = clip.fadeOutSeconds;
            item.gain = clip.gain;
            item.pan = clip.pan;
            item.pitchSemitones = clip.pitchSemitones;
            item.stretchRatio = clip.stretchRatio <= 0.0 ? 1.0 : clip.stretchRatio;
            item.muted = clip.muted;
            item.loopEnabled = clip.loopEnabled;
            item.timeStretchEnabled = clip.timeStretchEnabled;
            projectState_.playlistItems.push_back(std::move(item));
        }
    }

    for (auto& item : projectState_.playlistItems)
    {
        item.durationSeconds = std::max(0.125, item.durationSeconds);
        item.trimStartSeconds = std::max(0.0, item.trimStartSeconds);
        item.trimEndSeconds = std::max(0.0, item.trimEndSeconds);
        item.fadeInSeconds = std::max(0.0, item.fadeInSeconds);
        item.fadeOutSeconds = std::max(0.0, item.fadeOutSeconds);
        item.gain = std::clamp(item.gain, 0.0, 4.0);
        item.pan = std::clamp(item.pan, -1.0, 1.0);
        item.stretchRatio = item.stretchRatio <= 0.0 ? 1.0 : item.stretchRatio;

        if ((item.type == PlaylistItemType::PatternClip || item.type == PlaylistItemType::AudioClip) && item.sourceClipId != 0)
        {
            if (const ClipState* sourceClip = findClipState(item.sourceClipId); sourceClip != nullptr)
            {
                if (item.label.empty())
                {
                    item.label = sourceClip->name;
                }
                if (item.patternNumber == 0)
                {
                    item.patternNumber = sourceClip->patternNumber > 0
                        ? sourceClip->patternNumber
                        : extractPatternNumberFromClipName(sourceClip->name);
                }
            }
        }
    }

    for (auto& automationClip : projectState_.automationClips)
    {
        automationClip.trackId = automationClip.trackId == 0 && !projectState_.tracks.empty()
            ? projectState_.tracks.front().trackId
            : automationClip.trackId;

        auto playlistAutomationIt = std::find_if(
            projectState_.playlistItems.begin(),
            projectState_.playlistItems.end(),
            [&](const PlaylistItemState& item)
            {
                return item.type == PlaylistItemType::AutomationClip &&
                    item.automationClipId == automationClip.clipId;
            });

        if (playlistAutomationIt == projectState_.playlistItems.end())
        {
            PlaylistItemState item{};
            item.itemId = automationClip.clipId;
            item.trackId = automationClip.trackId;
            item.type = PlaylistItemType::AutomationClip;
            item.automationClipId = automationClip.clipId;
            item.label = automationClip.target;
            item.startTimeSeconds = static_cast<double>(std::max(0, automationClip.startCell)) * kPlaylistCellSeconds;
            item.durationSeconds = static_cast<double>(std::max(1, automationClip.lengthCells)) * kPlaylistCellSeconds;
            item.gain = 1.0;
            projectState_.playlistItems.push_back(std::move(item));
        }
        else
        {
            playlistAutomationIt->itemId = automationClip.clipId;
            playlistAutomationIt->trackId = automationClip.trackId;
            playlistAutomationIt->label = automationClip.target;
            playlistAutomationIt->startTimeSeconds =
                static_cast<double>(std::max(0, automationClip.startCell)) * kPlaylistCellSeconds;
            playlistAutomationIt->durationSeconds =
                static_cast<double>(std::max(1, automationClip.lengthCells)) * kPlaylistCellSeconds;
        }
    }

    for (auto& clip : projectState_.clips)
    {
        clip.patternNumber = clip.patternNumber > 0 ? clip.patternNumber : extractPatternNumberFromClipName(clip.name);
        clip.trimStartSeconds = std::max(0.0, clip.trimStartSeconds);
        clip.trimEndSeconds = std::max(0.0, clip.trimEndSeconds);
        clip.fadeInSeconds = std::max(0.0, clip.fadeInSeconds);
        clip.fadeOutSeconds = std::max(0.0, clip.fadeOutSeconds);
        clip.gain = std::clamp(clip.gain, 0.0, 4.0);
        clip.pan = std::clamp(clip.pan, -1.0, 1.0);
        clip.stretchRatio = clip.stretchRatio <= 0.0 ? 1.0 : clip.stretchRatio;
    }

    if (projectState_.patterns.empty())
    {
        for (int patternNumber = 1; patternNumber <= kDefaultPatternCount; ++patternNumber)
        {
            projectState_.patterns.push_back(makeDefaultPatternState(patternNumber));
        }
    }

    for (auto& pattern : projectState_.patterns)
    {
        std::vector<PatternLaneState> synchronizedLanes;
        synchronizedLanes.reserve(projectState_.tracks.size());

        for (std::size_t trackIndex = 0; trackIndex < projectState_.tracks.size(); ++trackIndex)
        {
            const TrackState& track = projectState_.tracks[trackIndex];
            PatternLaneState* existingLane = findPatternLane(pattern, track.trackId);
            if (existingLane != nullptr)
            {
                if (existingLane->steps.size() != static_cast<std::size_t>(kDefaultStepCount))
                {
                    existingLane->steps.resize(static_cast<std::size_t>(kDefaultStepCount));
                }
                synchronizedLanes.push_back(*existingLane);
            }
            else
            {
                synchronizedLanes.push_back(makeDefaultPatternLane(track.trackId, pattern.patternNumber, trackIndex));
            }
        }

        pattern.lanes = std::move(synchronizedLanes);
    }

    std::vector<ChannelSettingsState> synchronizedSettings;
    synchronizedSettings.reserve(projectState_.tracks.size());
    for (std::size_t trackIndex = 0; trackIndex < projectState_.tracks.size(); ++trackIndex)
    {
        const TrackState& track = projectState_.tracks[trackIndex];
        ChannelSettingsState* settings = findChannelSettings(track.trackId);
        if (settings != nullptr)
        {
            settings->name = track.name;
            synchronizedSettings.push_back(*settings);
        }
        else
        {
            synchronizedSettings.push_back(makeDefaultChannelSettings(track.trackId, trackIndex));
        }
    }
    projectState_.channelSettings = std::move(synchronizedSettings);

    for (auto& track : projectState_.tracks)
    {
        track.gain = std::clamp(track.gain, 0.0, 4.0);
        track.pan = std::clamp(track.pan, -1.0, 1.0);
        track.sendAmount = std::clamp(track.sendAmount, 0.0, 1.0);
        track.clipIds.clear();
        for (const auto& item : projectState_.playlistItems)
        {
            if (item.trackId == track.trackId && item.type != PlaylistItemType::AutomationClip)
            {
                track.clipIds.push_back(item.itemId);
            }
        }
    }

    if (projectState_.automationClips.empty())
    {
        projectState_.automationClips.push_back(AutomationClipState{
            nextAutomationClipId(),
            "Bus:1:gain",
            {{0, 84, 0.0}, {8, 78, 0.0}, {16, 90, 0.0}, {24, 82, 0.0}},
            projectState_.tracks.empty() ? 0u : projectState_.tracks.front().trackId,
            static_cast<int>(projectState_.tracks.size()),
            0,
            8});

        const AutomationClipState& automationClip = projectState_.automationClips.back();
        projectState_.playlistItems.push_back(PlaylistItemState{
            automationClip.clipId,
            automationClip.trackId,
            PlaylistItemType::AutomationClip,
            0,
            automationClip.clipId,
            0,
            automationClip.target,
            static_cast<double>(std::max(0, automationClip.startCell)) * kPlaylistCellSeconds,
            static_cast<double>(std::max(1, automationClip.lengthCells)) * kPlaylistCellSeconds,
            0.0,
            0.0,
            0.0,
            0.0,
            1.0,
            0.0,
            0.0,
            1.0,
            false,
            false,
            false});
    }
}

AudioEngine::PatternState* AudioEngine::findPatternState(int patternNumber)
{
    auto it = std::find_if(
        projectState_.patterns.begin(),
        projectState_.patterns.end(),
        [&](const PatternState& pattern) { return pattern.patternNumber == patternNumber; });
    return it == projectState_.patterns.end() ? nullptr : &(*it);
}

const AudioEngine::PatternState* AudioEngine::findPatternState(int patternNumber) const
{
    auto it = std::find_if(
        projectState_.patterns.begin(),
        projectState_.patterns.end(),
        [&](const PatternState& pattern) { return pattern.patternNumber == patternNumber; });
    return it == projectState_.patterns.end() ? nullptr : &(*it);
}

AudioEngine::PatternLaneState* AudioEngine::findPatternLane(PatternState& pattern, std::uint32_t trackId)
{
    auto it = std::find_if(
        pattern.lanes.begin(),
        pattern.lanes.end(),
        [&](const PatternLaneState& lane) { return lane.trackId == trackId; });
    return it == pattern.lanes.end() ? nullptr : &(*it);
}

const AudioEngine::PatternLaneState* AudioEngine::findPatternLane(const PatternState& pattern, std::uint32_t trackId) const
{
    auto it = std::find_if(
        pattern.lanes.begin(),
        pattern.lanes.end(),
        [&](const PatternLaneState& lane) { return lane.trackId == trackId; });
    return it == pattern.lanes.end() ? nullptr : &(*it);
}

AudioEngine::ClipState* AudioEngine::findClipState(std::uint32_t clipId)
{
    auto it = std::find_if(
        projectState_.clips.begin(),
        projectState_.clips.end(),
        [&](const ClipState& clip) { return clip.clipId == clipId; });
    return it == projectState_.clips.end() ? nullptr : &(*it);
}

const AudioEngine::ClipState* AudioEngine::findClipState(std::uint32_t clipId) const
{
    auto it = std::find_if(
        projectState_.clips.begin(),
        projectState_.clips.end(),
        [&](const ClipState& clip) { return clip.clipId == clipId; });
    return it == projectState_.clips.end() ? nullptr : &(*it);
}

AudioEngine::PlaylistItemState* AudioEngine::findPlaylistItem(std::uint32_t itemId)
{
    auto it = std::find_if(
        projectState_.playlistItems.begin(),
        projectState_.playlistItems.end(),
        [&](const PlaylistItemState& item) { return item.itemId == itemId; });
    return it == projectState_.playlistItems.end() ? nullptr : &(*it);
}

const AudioEngine::PlaylistItemState* AudioEngine::findPlaylistItem(std::uint32_t itemId) const
{
    auto it = std::find_if(
        projectState_.playlistItems.begin(),
        projectState_.playlistItems.end(),
        [&](const PlaylistItemState& item) { return item.itemId == itemId; });
    return it == projectState_.playlistItems.end() ? nullptr : &(*it);
}

AudioEngine::BusState* AudioEngine::findBusState(std::uint32_t busId)
{
    auto it = std::find_if(
        projectState_.buses.begin(),
        projectState_.buses.end(),
        [&](const BusState& bus) { return bus.busId == busId; });
    return it == projectState_.buses.end() ? nullptr : &(*it);
}

const AudioEngine::BusState* AudioEngine::findBusState(std::uint32_t busId) const
{
    auto it = std::find_if(
        projectState_.buses.begin(),
        projectState_.buses.end(),
        [&](const BusState& bus) { return bus.busId == busId; });
    return it == projectState_.buses.end() ? nullptr : &(*it);
}

AudioEngine::ChannelSettingsState* AudioEngine::findChannelSettings(std::uint32_t trackId)
{
    auto it = std::find_if(
        projectState_.channelSettings.begin(),
        projectState_.channelSettings.end(),
        [&](const ChannelSettingsState& settings) { return settings.trackId == trackId; });
    return it == projectState_.channelSettings.end() ? nullptr : &(*it);
}

const AudioEngine::ChannelSettingsState* AudioEngine::findChannelSettings(std::uint32_t trackId) const
{
    auto it = std::find_if(
        projectState_.channelSettings.begin(),
        projectState_.channelSettings.end(),
        [&](const ChannelSettingsState& settings) { return settings.trackId == trackId; });
    return it == projectState_.channelSettings.end() ? nullptr : &(*it);
}

AudioEngine::AutomationClipState* AudioEngine::findAutomationClip(std::uint32_t clipId)
{
    auto it = std::find_if(
        projectState_.automationClips.begin(),
        projectState_.automationClips.end(),
        [&](const AutomationClipState& clip) { return clip.clipId == clipId; });
    return it == projectState_.automationClips.end() ? nullptr : &(*it);
}

double AudioEngine::evaluateAutomationTarget(const std::string& target, double timelineSeconds, double defaultValue) const
{
    if (target.empty())
    {
        return defaultValue;
    }

    const std::string desiredTarget = toLowerCopy(target);
    const double timelineCell = timelineSeconds / kPlaylistCellSeconds;

    auto convertValue =
        [&](double normalizedValue, const std::string& normalizedTarget) -> double
    {
        const double clamped = std::clamp(normalizedValue, 0.0, 1.0);
        if (normalizedTarget.find(":pan") != std::string::npos)
        {
            return (clamped * 2.0) - 1.0;
        }
        if (normalizedTarget.find(":cutoff") != std::string::npos)
        {
            return 200.0 + (clamped * 15800.0);
        }
        if (normalizedTarget.find(":gain") != std::string::npos || normalizedTarget == "master volume")
        {
            return clamped;
        }
        return clamped;
    };

    for (const auto& automationClip : projectState_.automationClips)
    {
        const std::string clipTarget = toLowerCopy(automationClip.target);
        const bool targetMatches =
            clipTarget == desiredTarget ||
            (desiredTarget == "master:gain" && (clipTarget == "master volume" || clipTarget == "bus:1:gain")) ||
            (desiredTarget == "bus:1:gain" && clipTarget == "master volume");

        if (!targetMatches || automationClip.points.empty())
        {
            continue;
        }

        const double clipStartCell = static_cast<double>(std::max(0, automationClip.startCell));
        const double clipEndCell = clipStartCell + static_cast<double>(std::max(1, automationClip.lengthCells));
        if (timelineCell < clipStartCell || timelineCell > clipEndCell)
        {
            continue;
        }

        const AutomationPointState* previousPoint = &automationClip.points.front();
        const AutomationPointState* nextPoint = previousPoint;

        for (const auto& point : automationClip.points)
        {
            if (point.cell <= timelineCell)
            {
                previousPoint = &point;
            }
            if (point.cell >= timelineCell)
            {
                nextPoint = &point;
                break;
            }
        }

        if (nextPoint == nullptr)
        {
            nextPoint = previousPoint;
        }

        if (previousPoint == nullptr || nextPoint == nullptr)
        {
            continue;
        }

        if (previousPoint == nextPoint || nextPoint->cell == previousPoint->cell)
        {
            return convertValue(static_cast<double>(previousPoint->value) / 100.0, desiredTarget);
        }

        const double t =
            (timelineCell - static_cast<double>(previousPoint->cell)) /
            static_cast<double>(std::max(1, nextPoint->cell - previousPoint->cell));
        const double interpolatedValue =
            static_cast<double>(previousPoint->value) +
            ((static_cast<double>(nextPoint->value) - static_cast<double>(previousPoint->value)) * t);
        return convertValue(interpolatedValue / 100.0, desiredTarget);
    }

    return defaultValue;
}

bool AudioEngine::hasSoloedTracks() const
{
    return std::any_of(
        projectState_.tracks.begin(),
        projectState_.tracks.end(),
        [](const TrackState& track) { return track.solo; });
}

bool AudioEngine::hasSoloedBuses() const
{
    return std::any_of(
        projectState_.buses.begin(),
        projectState_.buses.end(),
        [](const BusState& bus) { return bus.solo; });
}

AudioEngine::PatternLaneState AudioEngine::makeDefaultPatternLane(std::uint32_t trackId, int patternNumber, std::size_t laneIndex) const
{
    PatternLaneState lane{};
    lane.trackId = trackId;
    lane.steps.resize(static_cast<std::size_t>(kDefaultStepCount));

    for (int stepIndex = 0; stepIndex < kDefaultStepCount; ++stepIndex)
    {
        const int patternOffset = patternNumber - 1;
        const bool onQuarter = ((stepIndex + patternOffset) % 4) == 0;
        const bool grooveHit =
            ((stepIndex + static_cast<int>(laneIndex) + patternOffset) % (patternNumber + 3)) == 0;
        lane.steps[static_cast<std::size_t>(stepIndex)].enabled = onQuarter || grooveHit;
        lane.steps[static_cast<std::size_t>(stepIndex)].velocity =
            68 + ((stepIndex * (patternNumber + 6) + static_cast<int>(laneIndex) * 11) % 56);
    }

    lane.swing = static_cast<int>((laneIndex * 9 + static_cast<std::size_t>(patternNumber) * 5) % 48);
    lane.shuffle = static_cast<int>((laneIndex * 5 + static_cast<std::size_t>(patternNumber) * 7) % 32);

    const int rootLane = 12 + static_cast<int>((laneIndex * 3) % 7);
    lane.notes.push_back(MidiNoteState{rootLane, 0, 3, 98, true, false, false});
    lane.notes.push_back(MidiNoteState{rootLane + 4, 4, 2, 84, false, false, false});
    lane.notes.push_back(MidiNoteState{rootLane + 7, 8, 4, 102, false, false, false});
    lane.notes.push_back(MidiNoteState{rootLane + 12, 14, 2, 92, false, (laneIndex % 2) == 0, false});
    return lane;
}

AudioEngine::PatternState AudioEngine::makeDefaultPatternState(int patternNumber) const
{
    PatternState pattern{};
    pattern.patternNumber = patternNumber;
    pattern.name = "Pattern " + std::to_string(patternNumber);
    pattern.lengthInBars = 2 + ((patternNumber - 1) % 3);
    pattern.accentAmount = 8 * patternNumber;

    for (std::size_t trackIndex = 0; trackIndex < projectState_.tracks.size(); ++trackIndex)
    {
        pattern.lanes.push_back(
            makeDefaultPatternLane(
                projectState_.tracks[trackIndex].trackId,
                patternNumber,
                trackIndex + static_cast<std::size_t>(patternNumber - 1)));
    }

    return pattern;
}

AudioEngine::ChannelSettingsState AudioEngine::makeDefaultChannelSettings(std::uint32_t trackId, std::size_t laneIndex) const
{
    ChannelSettingsState settings{};
    settings.trackId = trackId;

    auto trackIt = std::find_if(
        projectState_.tracks.begin(),
        projectState_.tracks.end(),
        [&](const TrackState& track) { return track.trackId == trackId; });
    settings.name = trackIt == projectState_.tracks.end() ? ("Channel " + std::to_string(trackId)) : trackIt->name;
    settings.generatorType = laneIndex % 3 == 2 ? GeneratorType::PluginInstrument : (laneIndex % 2 == 0 ? GeneratorType::Sampler : GeneratorType::TestSynth);
    settings.gain = 0.78 + (0.03 * static_cast<double>(laneIndex % 3));
    settings.pan = laneIndex % 3 == 0 ? -0.08 : (laneIndex % 4 == 0 ? 0.10 : 0.0);
    settings.pitchSemitones = static_cast<double>((laneIndex % 5) - 2);
    settings.attackMs = 10.0 + static_cast<double>(laneIndex * 2);
    settings.decayMs = 64.0 + static_cast<double>(laneIndex * 8);
    settings.sustainLevel = 0.72 + (0.04 * static_cast<double>(laneIndex % 3));
    settings.releaseMs = 160.0 + static_cast<double>(laneIndex * 18);
    settings.filterCutoffHz = 7600.0 + static_cast<double>((laneIndex % 4) * 900);
    settings.resonance = 0.18 + (0.04 * static_cast<double>(laneIndex % 3));
    settings.mixerInsert = static_cast<int>(laneIndex + 1);
    settings.routeTarget = 0;
    settings.reverse = false;
    settings.timeStretch = true;
    settings.sampleFilePath.clear();
    settings.instrumentPluginName = settings.generatorType == GeneratorType::PluginInstrument ? "Future VSTi Rack" : "";
    settings.pluginRack = {"Sampler", laneIndex % 2 == 0 ? "Transient" : "EQ"};
    settings.presets = {"Init", "Tight", "Wide"};
    return settings;
}

AudioEngine::GraphSnapshot AudioEngine::buildGraphFromProjectState() const
{
    GraphSnapshot graph{};
    graph.version = std::max<std::uint64_t>(1, projectState_.revision);

    graph.nodes.push_back(GraphNode{
        kInputNodeId,
        NodeType::Input,
        "Hardware Input",
        0,
        0,
        0,
        0,
        1.0,
        true,
        false,
        false,
        true});

    for (const auto& bus : projectState_.buses)
    {
        graph.nodes.push_back(GraphNode{
            busNodeId(bus.busId),
            NodeType::Bus,
            bus.name,
            0,
            0,
            bus.busId,
            0,
            bus.gain,
            false,
            true,
            bus.muted,
            true});
    }

    for (const auto& track : projectState_.tracks)
    {
        const bool liveTrack = track.armed;
        graph.nodes.push_back(GraphNode{
            trackNodeId(track.trackId),
            NodeType::Track,
            track.name,
            0,
            track.trackId,
            track.busId,
            0,
            track.gain,
            liveTrack,
            !liveTrack,
            track.muted,
            true});

        graph.nodes.push_back(GraphNode{
            pluginNodeId(track.trackId),
            NodeType::Plugin,
            track.name + " FX",
            32,
            track.trackId,
            track.busId,
            0,
            1.0,
            liveTrack,
            !liveTrack,
            false,
            true});

        if (track.sendBusId != 0 && track.sendAmount > 0.0 && findBusState(track.sendBusId) != nullptr)
        {
            graph.nodes.push_back(GraphNode{
                sendNodeId(track.trackId),
                NodeType::Send,
                track.name + " Send",
                0,
                track.trackId,
                track.sendBusId,
                0,
                std::clamp(track.sendAmount, 0.0, 1.0),
                liveTrack,
                !liveTrack,
                false,
                true});
        }

        graph.edges.push_back(GraphEdge{kInputNodeId, trackNodeId(track.trackId)});
        graph.edges.push_back(GraphEdge{trackNodeId(track.trackId), pluginNodeId(track.trackId)});

        const std::uint32_t routeBusId =
            track.routeTargetBusId != 0 && findBusState(track.routeTargetBusId) != nullptr
                ? track.routeTargetBusId
                : track.busId;
        graph.edges.push_back(GraphEdge{pluginNodeId(track.trackId), busNodeId(routeBusId)});

        if (track.sendBusId != 0 && track.sendBusId != routeBusId && findBusState(track.sendBusId) != nullptr)
        {
            graph.edges.push_back(GraphEdge{pluginNodeId(track.trackId), sendNodeId(track.trackId)});
            graph.edges.push_back(GraphEdge{sendNodeId(track.trackId), busNodeId(track.sendBusId)});
        }
    }

    graph.nodes.push_back(GraphNode{
        kOutputNodeId,
        NodeType::Output,
        "Hardware Output",
        0,
        0,
        0,
        0,
        1.0,
        true,
        true,
        false,
        true});

    for (const auto& bus : projectState_.buses)
    {
        if (bus.outputBusId != 0 && bus.outputBusId != bus.busId && findBusState(bus.outputBusId) != nullptr)
        {
            graph.edges.push_back(GraphEdge{busNodeId(bus.busId), busNodeId(bus.outputBusId)});
        }
        else
        {
            graph.edges.push_back(GraphEdge{busNodeId(bus.busId), kOutputNodeId});
        }
    }

    return graph;
}

std::optional<AudioEngine::GraphNode> AudioEngine::findGraphNode(std::uint32_t nodeId, const GraphSnapshot& graph) const
{
    const auto it = std::find_if(
        graph.nodes.begin(),
        graph.nodes.end(),
        [&](const GraphNode& node) { return node.id == nodeId; });

    if (it == graph.nodes.end())
    {
        return std::nullopt;
    }

    return *it;
}

const AudioEngine::CompiledNode* AudioEngine::findCompiledNode(std::uint32_t nodeId) const
{
    const auto it = compiledGraph_.nodeLookup.find(nodeId);
    if (it == compiledGraph_.nodeLookup.end())
    {
        return nullptr;
    }

    return &compiledGraph_.executionPlan[it->second];
}

const AudioEngine::AutomationLane* AudioEngine::findAutomationLane(std::uint32_t nodeId) const
{
    const auto it = std::find_if(
        automationLanes_.begin(),
        automationLanes_.end(),
        [&](const AutomationLane& lane) { return lane.nodeId == nodeId; });

    return it == automationLanes_.end() ? nullptr : &(*it);
}

AudioEngine::AutomationLane* AudioEngine::findAutomationLane(std::uint32_t nodeId)
{
    const auto it = std::find_if(
        automationLanes_.begin(),
        automationLanes_.end(),
        [&](const AutomationLane& lane) { return lane.nodeId == nodeId; });

    return it == automationLanes_.end() ? nullptr : &(*it);
}

double AudioEngine::getNodeAutomationValue(std::uint32_t nodeId, std::uint32_t sampleOffset) const
{
    const AutomationLane* lane = findAutomationLane(nodeId);
    if (lane == nullptr)
    {
        return 1.0;
    }

    for (const auto& segment : lane->activeSegments)
    {
        if (segment.parameterId != kDefaultAutomationParameter)
        {
            continue;
        }

        if (sampleOffset < segment.startSample || sampleOffset > segment.endSample)
        {
            continue;
        }

        const std::uint32_t segmentLength = std::max<std::uint32_t>(1, segment.endSample - segment.startSample);
        const double position = static_cast<double>(sampleOffset - segment.startSample) /
            static_cast<double>(segmentLength);
        return interpolateValue(segment.startValue, segment.endValue, position, segment.mode);
    }

    if (!lane->parameterStates.empty())
    {
        return lane->parameterStates.front().currentValue;
    }

    return 1.0;
}
