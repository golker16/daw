#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iterator>

namespace
{
    bool isSampleFilePath(const std::filesystem::path& path)
    {
        std::string extension = path.extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char value)
        {
            return static_cast<char>(std::tolower(value));
        });

        return extension == ".wav" ||
            extension == ".mp3" ||
            extension == ".flac" ||
            extension == ".aif" ||
            extension == ".aiff" ||
            extension == ".ogg";
    }

    std::string displayNameForPath(const std::filesystem::path& path)
    {
        std::string name = path.filename().string();
        if (name.empty())
        {
            name = path.root_name().string();
        }
        if (name.empty())
        {
            name = path.string();
        }
        return name;
    }
}

bool UI::isBrowserGroupExpanded(const std::string& id) const
{
    return std::find(expandedBrowserGroupIds_.begin(), expandedBrowserGroupIds_.end(), id) != expandedBrowserGroupIds_.end();
}

void UI::setBrowserGroupExpanded(const std::string& id, bool expanded)
{
    auto entryIt = std::find(expandedBrowserGroupIds_.begin(), expandedBrowserGroupIds_.end(), id);
    if (expanded)
    {
        if (entryIt == expandedBrowserGroupIds_.end())
        {
            expandedBrowserGroupIds_.push_back(id);
        }
    }
    else if (entryIt != expandedBrowserGroupIds_.end())
    {
        expandedBrowserGroupIds_.erase(entryIt);
    }
}

void UI::toggleBrowserGroup(const std::string& id)
{
    setBrowserGroupExpanded(id, !isBrowserGroupExpanded(id));
    rebuildBrowserEntries();
    updateBrowserScrollBar();
    invalidateSurface(browserPanel_);
}

void UI::addSampleLibraryRoot(const std::string& path)
{
    if (path.empty())
    {
        return;
    }

    std::error_code ec;
    const std::filesystem::path absolutePath = std::filesystem::absolute(std::filesystem::path(path), ec);
    const std::string normalizedPath = ec ? path : absolutePath.lexically_normal().string();
    if (normalizedPath.empty())
    {
        return;
    }

    if (std::find(sampleLibraryRoots_.begin(), sampleLibraryRoots_.end(), normalizedPath) == sampleLibraryRoots_.end())
    {
        sampleLibraryRoots_.push_back(normalizedPath);
    }

    setBrowserGroupExpanded("sample:" + normalizedPath, true);
}

void UI::handleBrowserDrop(WPARAM dropHandle)
{
    HDROP droppedFiles = reinterpret_cast<HDROP>(dropHandle);
    if (droppedFiles != nullptr)
    {
        DragFinish(droppedFiles);
    }
}

int UI::playlistTrackLaneCount() const
{
    return std::max(1, static_cast<int>(visibleState_.project.tracks.size()));
}

int UI::playlistTotalCellCount() const
{
    int maxCell = 64;

    for (const auto& block : workspaceModel_.playlistBlocks)
    {
        maxCell = std::max(maxCell, block.startCell + block.lengthCells + 8);
    }

    for (const auto& marker : workspaceModel_.markers)
    {
        maxCell = std::max(maxCell, marker.timelineCell + 8);
    }

    const int rounded = ((maxCell + 15) / 16) * 16;
    return std::max(64, rounded);
}

int UI::playlistColumnWidth(int availableWidth) const
{
    static constexpr int kWidths[] = {24, 32, 48, 72, 104};
    const int width = kWidths[std::min<std::size_t>(workspace_.playlistZoomIndex, 4)];
    return std::min(std::max(20, width), std::max(20, availableWidth));
}

void UI::clampBrowserScroll()
{
    if (browserPanel_ == nullptr)
    {
        browserScrollY_ = 0;
        return;
    }

    RECT clientRect{};
    GetClientRect(browserPanel_, &clientRect);
    const int visibleHeight = std::max(1, static_cast<int>(clientRect.bottom - clientRect.top) - 4);
    const int contentHeight = static_cast<int>(workspaceModel_.browserEntries.size()) * kBrowserRowHeight;
    const int maxScroll = std::max(0, contentHeight - visibleHeight);
    browserScrollY_ = clampValue(browserScrollY_, 0, maxScroll);
}

void UI::clampPlaylistScroll()
{
    if (playlistPanel_ == nullptr)
    {
        playlistScrollX_ = 0;
        playlistScrollY_ = 0;
        return;
    }

    RECT clientRect{};
    GetClientRect(playlistPanel_, &clientRect);
    const PlaylistLayoutMetrics metrics = makePlaylistLayoutMetrics(clientRect);
    const int contentWidth = playlistTotalCellCount() * playlistColumnWidth(metrics.visibleGridWidth);
    const int contentHeight = playlistContentHeightForViewport(playlistTrackLaneCount(), metrics.visibleGridHeight);
    const int maxScrollX = std::max(0, contentWidth - metrics.visibleGridWidth);
    const int maxScrollY = std::max(0, contentHeight - metrics.visibleGridHeight);
    playlistScrollX_ = clampValue(playlistScrollX_, 0, maxScrollX);
    playlistScrollY_ = clampValue(playlistScrollY_, 0, maxScrollY);
}

void UI::updateBrowserScrollBar()
{
    if (browserPanel_ == nullptr)
    {
        return;
    }

    clampBrowserScroll();

    RECT clientRect{};
    GetClientRect(browserPanel_, &clientRect);
    const int visibleHeight = std::max(1, static_cast<int>(clientRect.bottom - clientRect.top) - 4);
    const int contentHeight = std::max(visibleHeight, static_cast<int>(workspaceModel_.browserEntries.size()) * kBrowserRowHeight);

    SCROLLINFO info{};
    info.cbSize = sizeof(info);
    info.fMask = SIF_PAGE | SIF_POS | SIF_RANGE;
    info.nMin = 0;
    info.nMax = std::max(0, contentHeight - 1);
    info.nPage = static_cast<UINT>(visibleHeight);
    info.nPos = browserScrollY_;
    SetScrollInfo(browserPanel_, SB_VERT, &info, TRUE);
}

void UI::updatePlaylistScrollBars()
{
    if (playlistPanel_ == nullptr)
    {
        return;
    }

    clampPlaylistScroll();
    ShowScrollBar(playlistPanel_, SB_BOTH, FALSE);
}

void UI::scrollBrowserTo(int y)
{
    browserScrollY_ = y;
    updateBrowserScrollBar();
    invalidateSurface(browserPanel_);
}

void UI::scrollPlaylistTo(int x, int y)
{
    playlistScrollX_ = x;
    playlistScrollY_ = y;
    updatePlaylistScrollBars();
    invalidateSurface(playlistPanel_);
}

void UI::rebuildPlaylistVisuals(const RECT& rect)
{
    interactionState_.playlistClipVisuals.clear();
    const PlaylistLayoutMetrics metrics = makePlaylistLayoutMetrics(rect);
    const int laneHeight = kPlaylistLaneHeight;
    const int columnWidth = playlistColumnWidth(metrics.visibleGridWidth);
    const int clipTopBase = metrics.gridRect.top + 6 - playlistScrollY_;
    const int clipLeftBase = metrics.gridRect.left - playlistScrollX_;

    for (const auto& block : workspaceModel_.playlistBlocks)
    {
        UiClipVisual visual{};
        visual.clipId = block.clipId;
        visual.trackId = block.trackId;
        visual.label = block.label;
        visual.clipType = block.clipType;
        visual.rect.x = clipLeftBase + (block.startCell * columnWidth);
        visual.rect.y = clipTopBase + (block.lane * laneHeight);
        visual.rect.width = std::max(42, block.lengthCells * columnWidth - 6);
        visual.rect.height = laneHeight - 10;
        visual.selected = block.selected || (block.clipId == interactionState_.selectedClipId);
        visual.automation = false;
        visual.resizeLeftHot = false;
        visual.resizeRightHot = false;

        if (visual.rect.x + visual.rect.width < metrics.gridRect.left ||
            visual.rect.x > metrics.gridRect.right ||
            visual.rect.y + visual.rect.height < metrics.gridRect.top ||
            visual.rect.y > metrics.gridRect.bottom)
        {
            continue;
        }

        interactionState_.playlistClipVisuals.push_back(visual);
    }
}

void UI::rebuildPianoVisuals(const RECT& rect)
{
    interactionState_.pianoNoteVisuals.clear();
    const int keyWidth = 72;
    const int laneHeight = 22;
    const int usableWidth = std::max<int>(1, static_cast<int>(rect.right) - keyWidth);
    const int stepWidth = std::max<int>(18, usableWidth / kPlaylistCellCount);

    if (workspaceModel_.patternLanes.empty())
    {
        return;
    }

    const PatternLaneState& lane = workspaceModel_.patternLanes[static_cast<std::size_t>(workspaceModel_.activeChannelIndex)];
    for (std::size_t index = 0; index < lane.notes.size(); ++index)
    {
        const PianoNoteState& source = lane.notes[index];
        UiNoteVisual note{};
        note.lane = source.lane;
        note.step = source.step;
        note.rect.x = keyWidth + 20 + (source.step * stepWidth);
        note.rect.y = 24 + ((kPianoLaneCount - 1 - source.lane) * laneHeight);
        note.rect.width = std::max(18, source.length * stepWidth - 4);
        note.rect.height = laneHeight - 4;
        note.selected = source.selected || (interactionState_.selectedNoteIndex == index);
        note.resizeRightHot = false;
        note.rect.x = clampValue(note.rect.x, keyWidth + 2, rect.right - 60);
        interactionState_.pianoNoteVisuals.push_back(note);
    }
}

int UI::clampValue(int value, int minValue, int maxValue) const
{
    return std::max(minValue, std::min(value, maxValue));
}

void UI::setStaticText(HWND control, const std::string& text) const
{
    if (control != nullptr)
    {
        SetWindowTextA(control, text.c_str());
    }
}

void UI::syncWorkspaceModel()
{
    workspace_.browserTabIndex = std::min<std::size_t>(workspace_.browserTabIndex, kBrowserTabCount - 1);
    ensurePatternBank();
    rebuildPatternLanes();
    rebuildPlaylistBlocks();
    rebuildMixerStrips();
    rebuildChannelSettings();
    rebuildAutomationLanes();
    rebuildBrowserEntries();

    if (!workspaceModel_.patterns.empty())
    {
        workspaceModel_.selectedPatternIndex =
            clampValue(workspace_.activePattern - 1, 0, static_cast<int>(workspaceModel_.patterns.size() - 1));
        workspace_.activePattern = workspaceModel_.patterns[static_cast<std::size_t>(workspaceModel_.selectedPatternIndex)].patternNumber;
    }
    else
    {
        workspaceModel_.selectedPatternIndex = 0;
        workspace_.activePattern = 1;
    }

    if (!workspaceModel_.patternLanes.empty())
    {
        workspaceModel_.activeChannelIndex =
            clampValue(static_cast<int>(selectedTrackIndex_), 0, static_cast<int>(workspaceModel_.patternLanes.size() - 1));
    }
    else
    {
        workspaceModel_.activeChannelIndex = 0;
    }

    if (!workspaceModel_.browserEntries.empty())
    {
        workspaceModel_.selectedBrowserIndex =
            clampValue(workspaceModel_.selectedBrowserIndex, 0, static_cast<int>(workspaceModel_.browserEntries.size() - 1));
    }
    else
    {
        workspaceModel_.selectedBrowserIndex = 0;
    }
}

void UI::rebuildBrowserEntries()
{
    workspaceModel_.browserEntries.clear();

    auto addEntry =
        [&](std::string id,
            std::string category,
            std::string label,
            std::string subtitle,
            std::string payloadPath = {},
            bool favorite = false,
            int indentLevel = 0,
            bool group = false,
            bool expanded = false,
            bool draggable = false,
            bool folder = false)
    {
        workspaceModel_.browserEntries.push_back(
            BrowserEntry{
                std::move(id),
                std::move(category),
                std::move(label),
                std::move(subtitle),
                std::move(payloadPath),
                favorite,
                indentLevel,
                group,
                expanded,
                draggable,
                folder});
    };

    const bool patternsExpanded = isBrowserGroupExpanded("project:patterns");
    const bool audioExpanded = isBrowserGroupExpanded("project:audio");

    addEntry(
        "project:patterns",
        "Patterns",
        "Patterns",
        std::to_string(workspaceModel_.patterns.size()) + " pattern(s)",
        {},
        false,
        0,
        true,
        patternsExpanded);

    if (patternsExpanded)
    {
        if (workspaceModel_.patterns.empty())
        {
            addEntry("project:patterns:empty", "Pattern", "<empty>", "No patterns in the project yet.", {}, false, 1);
        }
        else
        {
            for (const auto& pattern : workspaceModel_.patterns)
            {
                int laneCountWithSteps = 0;
                for (const auto& lane : pattern.lanes)
                {
                    if (std::any_of(
                            lane.steps.begin(),
                            lane.steps.end(),
                            [](const ChannelStepState& step) { return step.enabled; }))
                    {
                        ++laneCountWithSteps;
                    }
                }

                addEntry(
                    "project:pattern:" + std::to_string(pattern.patternNumber),
                    "Pattern",
                    pattern.name,
                    std::to_string(pattern.lanes.size()) + " channels | " +
                        std::to_string(laneCountWithSteps) + " active lanes | " +
                        std::to_string(pattern.lengthInBars) + " bars",
                    {},
                    pattern.patternNumber == workspace_.activePattern,
                    1,
                    false,
                    false,
                    false);
            }
        }
    }

    std::vector<const PlaylistBlockState*> audioBlocks;
    for (const auto& block : workspaceModel_.playlistBlocks)
    {
        if (block.clipType == "Audio")
        {
            audioBlocks.push_back(&block);
        }
    }

    addEntry(
        "project:audio",
        "Audio",
        "Audio Clips",
        std::to_string(audioBlocks.size()) + " audio clip(s)",
        {},
        false,
        0,
        true,
        audioExpanded);

    if (audioExpanded)
    {
        if (audioBlocks.empty())
        {
            addEntry("project:audio:empty", "Audio", "<empty>", "No audio clips in the playlist yet.", {}, false, 1);
        }
        else
        {
            for (const PlaylistBlockState* block : audioBlocks)
            {
                std::string trackName = "Track " + std::to_string(block->lane + 1);
                if (block->lane >= 0 && block->lane < static_cast<int>(visibleState_.project.tracks.size()))
                {
                    trackName = visibleState_.project.tracks[static_cast<std::size_t>(block->lane)].name;
                }

                addEntry(
                    "project:audio:" + std::to_string(block->clipId),
                    "Audio",
                    block->label,
                    trackName + " | Start " + std::to_string(block->startCell + 1) +
                        " | Length " + std::to_string(block->lengthCells) + " cells",
                    {},
                    block->selected,
                    1,
                    false,
                    false,
                    false);
            }
        }
    }

    if (workspaceModel_.browserEntries.empty())
    {
        workspaceModel_.selectedBrowserIndex = 0;
        return;
    }

    const int maxIndex = static_cast<int>(workspaceModel_.browserEntries.size() - 1);
    int preferredIndex = -1;
    int firstLeafIndex = -1;
    for (std::size_t index = 0; index < workspaceModel_.browserEntries.size(); ++index)
    {
        const BrowserEntry& entry = workspaceModel_.browserEntries[index];
        if (!entry.group && firstLeafIndex < 0)
        {
            firstLeafIndex = static_cast<int>(index);
        }
        if (!entry.group && entry.favorite)
        {
            preferredIndex = static_cast<int>(index);
            break;
        }
    }

    workspaceModel_.selectedBrowserIndex = clampValue(workspaceModel_.selectedBrowserIndex, 0, maxIndex);
    if (workspaceModel_.browserEntries[static_cast<std::size_t>(workspaceModel_.selectedBrowserIndex)].group)
    {
        workspaceModel_.selectedBrowserIndex =
            preferredIndex >= 0 ? preferredIndex :
            firstLeafIndex >= 0 ? firstLeafIndex :
            workspaceModel_.selectedBrowserIndex;
    }
}

void UI::ensurePatternBank()
{
    workspaceModel_.patterns.clear();
    for (const auto& sourcePattern : visibleState_.project.patterns)
    {
        PatternState pattern{};
        pattern.patternNumber = sourcePattern.patternNumber;
        pattern.name = sourcePattern.name;
        pattern.lengthInBars = sourcePattern.lengthInBars;
        pattern.accentAmount = sourcePattern.accentAmount;

        for (const auto& sourceLane : sourcePattern.lanes)
        {
            PatternLaneState lane{};
            lane.trackId = sourceLane.trackId;
            lane.steps.reserve(sourceLane.steps.size());
            for (const auto& step : sourceLane.steps)
            {
                lane.steps.push_back(ChannelStepState{step.enabled, step.velocity});
            }

            for (const auto& note : sourceLane.notes)
            {
                lane.notes.push_back(PianoNoteState{
                    note.lane,
                    note.step,
                    note.length,
                    note.velocity,
                    note.accent,
                    note.slide,
                    note.selected});
            }

            lane.swing = sourceLane.swing;
            lane.shuffle = sourceLane.shuffle;

            const auto trackIt = std::find_if(
                visibleState_.project.tracks.begin(),
                visibleState_.project.tracks.end(),
                [&](const VisibleTrack& track) { return track.trackId == sourceLane.trackId; });
            lane.name = trackIt == visibleState_.project.tracks.end()
                ? ("Channel " + std::to_string(sourceLane.trackId))
                : trackIt->name;

            pattern.lanes.push_back(std::move(lane));
        }

        workspaceModel_.patterns.push_back(std::move(pattern));
    }

    if (workspaceModel_.patterns.empty())
    {
        workspaceModel_.patterns.push_back(makePatternState(1));
    }
}

void UI::rebuildPatternLanes()
{
    workspaceModel_.patternLanes.clear();

    if (workspaceModel_.patterns.empty())
    {
        workspaceModel_.patterns.push_back(makePatternState(1));
    }

    workspaceModel_.selectedPatternIndex =
        clampValue(workspace_.activePattern - 1, 0, static_cast<int>(workspaceModel_.patterns.size() - 1));
    workspaceModel_.patternLanes =
        workspaceModel_.patterns[static_cast<std::size_t>(workspaceModel_.selectedPatternIndex)].lanes;
}

void UI::rebuildPlaylistBlocks()
{
    workspaceModel_.playlistBlocks.clear();
    workspaceModel_.markers.clear();

    for (const auto& item : visibleState_.project.playlistItems)
    {
        if (item.type == AudioEngine::PlaylistItemType::AutomationClip)
        {
            continue;
        }

        auto trackIt = std::find_if(
            visibleState_.project.tracks.begin(),
            visibleState_.project.tracks.end(),
            [&](const VisibleTrack& track) { return track.trackId == item.trackId; });
        if (trackIt == visibleState_.project.tracks.end())
        {
            continue;
        }

        PlaylistBlockState block{};
        block.clipId = item.itemId;
        block.trackId = item.trackId;
        block.lane = static_cast<int>(std::distance(visibleState_.project.tracks.begin(), trackIt));
        block.startCell = std::max(0, static_cast<int>(item.startTimeSeconds * 2.0 + 0.5));
        block.lengthCells = std::max(2, static_cast<int>(item.durationSeconds * 2.0 + 0.5));
        block.label = item.label.empty() ? ("Clip " + std::to_string(item.itemId)) : item.label;
        block.clipType = item.type == AudioEngine::PlaylistItemType::AudioClip ? "Audio" : "Pattern";
        block.patternNumber = item.patternNumber;
        block.muted = item.muted;
        block.selected = (item.itemId == interactionState_.selectedClipId);
        workspaceModel_.playlistBlocks.push_back(std::move(block));
    }

    for (const auto& marker : visibleState_.project.markers)
    {
        workspaceModel_.markers.push_back(
            PlaylistMarkerState{
                marker.name,
                std::max(0, static_cast<int>(marker.timeSeconds * 2.0 + 0.5))});
    }
}

void UI::rebuildMixerStrips()
{
    workspaceModel_.mixerStrips.clear();

    int insertSlot = 1;
    for (const auto& bus : visibleState_.project.buses)
    {
        MixerStripState strip{};
        strip.busId = bus.busId;
        strip.name = bus.name;
        strip.insertSlot = insertSlot;
        strip.volumeDb = 20.0 * std::log10(std::max(0.0001, bus.gain));
        strip.pan = bus.pan;
        strip.peakLevel = normalizedMeterFill(strip.volumeDb);
        strip.solo = bus.solo;
        strip.muted = bus.muted;
        strip.routeTarget = bus.outputBusId == 0 ? -1 : static_cast<int>(bus.outputBusId);
        strip.sendAmount = 0.0;
        strip.effects = {"EQ", "Comp", insertSlot % 2 == 0 ? "Saturator" : "Delay"};
        workspaceModel_.mixerStrips.push_back(std::move(strip));
        ++insertSlot;
    }

    if (workspaceModel_.mixerStrips.empty())
    {
        workspaceModel_.mixerStrips.push_back(MixerStripState{1, "Kick Bus", 1, -3.0, 0.0, 78, false, false, -1, 0.10, {"EQ", "Clipper"}});
        workspaceModel_.mixerStrips.push_back(MixerStripState{2, "Music Bus", 2, -4.5, -0.08, 66, false, false, 0, 0.26, {"EQ", "Compressor", "Stereo"}});
    }

    const double masterGain =
        visibleState_.project.buses.empty()
            ? 0.87
            : visibleState_.project.buses.front().gain;
    const double masterPan =
        visibleState_.project.buses.empty()
            ? 0.0
            : visibleState_.project.buses.front().pan;
    const bool masterSolo =
        !visibleState_.project.buses.empty() && visibleState_.project.buses.front().solo;
    const bool masterMuted =
        !visibleState_.project.buses.empty() && visibleState_.project.buses.front().muted;
    workspaceModel_.mixerStrips.push_back(MixerStripState{
        0,
        "Master",
        0,
        20.0 * std::log10(std::max(0.0001, masterGain)),
        masterPan,
        normalizedMeterFill(20.0 * std::log10(std::max(0.0001, masterGain))),
        masterSolo,
        masterMuted,
        -1,
        0.0,
        {"Glue", "Limiter"}});
}

void UI::rebuildChannelSettings()
{
    workspaceModel_.channelSettings.clear();
    for (const auto& sourceSettings : visibleState_.project.channelSettings)
    {
        ChannelSettingsState settings{};
        settings.trackId = sourceSettings.trackId;
        settings.name = sourceSettings.name;
        settings.gain = sourceSettings.gain;
        settings.pan = sourceSettings.pan;
        settings.pitchSemitones = sourceSettings.pitchSemitones;
        settings.attackMs = sourceSettings.attackMs;
        settings.releaseMs = sourceSettings.releaseMs;
        settings.filterCutoffHz = sourceSettings.filterCutoffHz;
        settings.resonance = sourceSettings.resonance;
        settings.mixerInsert = sourceSettings.mixerInsert;
        settings.routeTarget = sourceSettings.routeTarget;
        settings.reverse = sourceSettings.reverse;
        settings.timeStretch = sourceSettings.timeStretch;
        settings.pluginRack = sourceSettings.pluginRack;
        if (sourceSettings.generatorType == AudioEngine::GeneratorType::Sampler)
        {
            settings.pluginRack.insert(settings.pluginRack.begin(), "Sampler");
        }
        else if (sourceSettings.generatorType == AudioEngine::GeneratorType::TestSynth)
        {
            settings.pluginRack.insert(settings.pluginRack.begin(), "TestSynth");
        }
        else
        {
            settings.pluginRack.insert(settings.pluginRack.begin(), "PluginInst");
        }
        settings.presets = sourceSettings.presets;
        workspaceModel_.channelSettings.push_back(std::move(settings));
    }

    if (workspaceModel_.channelSettings.empty())
    {
        workspaceModel_.channelSettings.push_back(ChannelSettingsState{0, "Init Sampler", 0.8, 0.0, 0.0, 12.0, 180.0, 8400.0, 0.2, 1, 0, false, true, {"Sampler", "EQ"}, {"Init", "Bright", "Wide"}});
    }
}

void UI::rebuildAutomationLanes()
{
    workspaceModel_.automationLanes.clear();
    const int automationLaneBase = static_cast<int>(workspaceModel_.patternLanes.size());
    int automationIndex = 0;
    for (const auto& sourceAutomation : visibleState_.project.automationClips)
    {
        AutomationLaneState automation{};
        automation.target = sourceAutomation.target;
        automation.clipId = sourceAutomation.clipId;
        automation.lane = automationLaneBase + automationIndex;
        automation.startCell = sourceAutomation.startCell;
        automation.lengthCells = sourceAutomation.lengthCells;
        for (const auto& point : sourceAutomation.points)
        {
            automation.values.push_back(point.value);
        }
        workspaceModel_.automationLanes.push_back(std::move(automation));
        ++automationIndex;
    }
}

UI::PatternState UI::makePatternState(int patternNumber) const
{
    PatternState pattern{};
    pattern.patternNumber = patternNumber;
    pattern.name = "Pattern " + std::to_string(patternNumber);
    pattern.lengthInBars = 2 + ((patternNumber - 1) % 3);
    pattern.accentAmount = 8 * patternNumber;

    std::size_t laneIndex = 0;
    for (const auto& track : visibleState_.project.tracks)
    {
        PatternLaneState lane{};
        lane.trackId = track.trackId;
        lane.name = track.name;
        lane.steps.resize(kStepCount);
        for (int step = 0; step < kStepCount; ++step)
        {
            const int patternOffset = patternNumber - 1;
            const bool onQuarter = ((step + patternOffset) % 4 == 0);
            const bool grooveHit = ((step + static_cast<int>(laneIndex) + patternOffset) % (patternNumber + 3) == 0);
            lane.steps[static_cast<std::size_t>(step)].enabled = onQuarter || grooveHit;
            lane.steps[static_cast<std::size_t>(step)].velocity =
                68 + ((step * (patternNumber + 6) + static_cast<int>(laneIndex) * 11) % 56);
        }
        lane.swing = static_cast<int>((laneIndex * 9 + patternNumber * 5) % 48);
        lane.shuffle = static_cast<int>((laneIndex * 5 + patternNumber * 7) % 32);
        ensurePatternLaneNoteContent(lane, laneIndex + static_cast<std::size_t>(patternNumber - 1));
        pattern.lanes.push_back(std::move(lane));
        ++laneIndex;
    }

    if (pattern.lanes.empty())
    {
        PatternLaneState lane{};
        lane.name = "Init Sampler";
        lane.steps.resize(kStepCount);
        for (int step = 0; step < kStepCount; ++step)
        {
            lane.steps[static_cast<std::size_t>(step)].enabled = ((step + patternNumber - 1) % 4 == 0);
            lane.steps[static_cast<std::size_t>(step)].velocity = 88 + (patternNumber * 2);
        }
        ensurePatternLaneNoteContent(lane, static_cast<std::size_t>(patternNumber - 1));
        pattern.lanes.push_back(std::move(lane));
    }

    return pattern;
}

void UI::ensurePatternLaneNoteContent(PatternLaneState& lane, std::size_t laneIndex) const
{
    if (!lane.notes.empty())
    {
        return;
    }

    const int rootLane = 12 + static_cast<int>((laneIndex * 3) % 7);
    lane.notes.push_back(PianoNoteState{rootLane, 0, 3, 98, true, false, false});
    lane.notes.push_back(PianoNoteState{rootLane + 4, 4, 2, 84, false, false, false});
    lane.notes.push_back(PianoNoteState{rootLane + 7, 8, 4, 102, false, false, false});
    lane.notes.push_back(PianoNoteState{rootLane + 12, 14, 2, 92, false, (laneIndex % 2) == 0, false});
}

void UI::paintMainBackground(HDC dc) const
{
    if (hwnd_ == nullptr)
    {
        return;
    }

    RECT clientRect{};
    GetClientRect(hwnd_, &clientRect);

    fillRectColor(dc, clientRect, kUiShadow);

    const int menuStripHeight = 28;
    const int topPanelHeight = 104;
    const int topPanelY = menuStripHeight;
    const int workspaceTop = topPanelY + topPanelHeight + kGap;

    RECT menuRect{clientRect.left, clientRect.top, clientRect.right, menuStripHeight};
    RECT toolbarRect{clientRect.left, menuStripHeight, clientRect.right, topPanelY + topPanelHeight};
    RECT workspaceRect{clientRect.left, workspaceTop, clientRect.right, clientRect.bottom};

    fillRectColor(dc, menuRect, RGB(56, 64, 67));
    fillRectColor(dc, toolbarRect, kFlToolbarBase);
    fillRectColor(dc, workspaceRect, blendColor(kUiPetrol, kUiShadow, 1, 4));
    drawHorizontalLine(dc, clientRect.left, clientRect.right, menuStripHeight - 1, RGB(73, 82, 86));
    drawHorizontalLine(dc, clientRect.left, clientRect.right, topPanelY + topPanelHeight - 1, kFlToolbarBorder);

    if (workspace_.browserVisible)
    {
        const int browserDividerX = kOuterPadding + kBrowserWidth + (kGap / 2);
        drawVerticalLine(dc, browserDividerX, workspaceTop, clientRect.bottom - kOuterPadding, kUiLineSoft);
    }

    const int timePanelX = 22;
    const int timePanelY = topPanelY + 12;
    const int timePanelWidth = 210;
    const int displayHeight = 50;
    const int transportX = timePanelX + timePanelWidth + 18;
    const int transportY = topPanelY + 18;
    const int patternClusterWidth = 392;
    const int clientRight = static_cast<int>(clientRect.right);
    const int patternClusterLimit = std::max(kOuterPadding, clientRight - kOuterPadding - patternClusterWidth);
    const int patternClusterDesired = std::max(transportX + 344, clientRight - patternClusterWidth - 28);
    const int patternClusterX = std::min(patternClusterLimit, patternClusterDesired);

    const int totalCentiseconds = std::max(0, static_cast<int>(displayedTimelineSeconds() * 100.0 + 0.5));
    const int minutes = totalCentiseconds / 6000;
    const int seconds = (totalCentiseconds / 100) % 60;
    const int centiseconds = totalCentiseconds % 100;
    char timeBuffer[32]{};
    std::snprintf(timeBuffer, sizeof(timeBuffer), "%d:%02d:%02d", minutes, seconds, centiseconds);

    static HFONT timeFont = CreateFontA(
        34,
        0,
        0,
        0,
        FW_NORMAL,
        FALSE,
        FALSE,
        FALSE,
        ANSI_CHARSET,
        OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_SWISS,
        "Segoe UI");
    static HFONT smallFont = CreateFontA(
        11,
        0,
        0,
        0,
        FW_BOLD,
        FALSE,
        FALSE,
        FALSE,
        ANSI_CHARSET,
        OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_SWISS,
        "Segoe UI");
    HGDIOBJ oldFont = SelectObject(dc, timeFont);
    RECT timeValueRect{timePanelX, timePanelY + 2, timePanelX + timePanelWidth, timePanelY + displayHeight - 4};
    SetTextColor(dc, RGB(226, 233, 236));
    DrawTextA(dc, timeBuffer, -1, &timeValueRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    SelectObject(dc, smallFont);
    RECT timeModeRect{timePanelX + 136, timePanelY + 4, timePanelX + timePanelWidth, timePanelY + 16};
    SetTextColor(dc, RGB(178, 188, 194));
    DrawTextA(dc, "M:S:CS", -1, &timeModeRect, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, oldFont);

    RECT patternFieldRect{patternClusterX + 26, transportY + 6, patternClusterX + 136, transportY + 40};
    fillRectColor(dc, patternFieldRect, RGB(209, 222, 228));
    drawSurfaceFrame(dc, patternFieldRect, RGB(184, 196, 201));
}

HBRUSH UI::resolveLabelBrush(HWND control, HDC dc) const
{
    static HBRUSH toolbarBrush = CreateSolidBrush(kUiAnthracite);
    static HBRUSH infoBrush = CreateSolidBrush(blendColor(kUiGraphite, kUiAnthracite, 1, 3));
    static HBRUSH bodyBrush = CreateSolidBrush(blendColor(kUiPetrol, kUiShadow, 1, 4));
    static HBRUSH fieldBrush = CreateSolidBrush(RGB(208, 222, 227));

    SetBkMode(dc, TRANSPARENT);

    const int controlId = GetDlgCtrlID(control);
    COLORREF textColor = kUiTextSoft;
    HBRUSH brush = bodyBrush;

    switch (controlId)
    {
    case IdLabelTempo:
    case IdLabelPattern:
        textColor = RGB(79, 92, 99);
        brush = fieldBrush;
        break;

    case IdLabelSystem:
        textColor = kUiTextSoft;
        brush = toolbarBrush;
        break;

    case IdLabelSnap:
        textColor = kUiText;
        brush = toolbarBrush;
        break;

    case IdLabelStatus:
        textColor = isControlActive(IdButtonPlay) ? kUiLime : kUiText;
        brush = infoBrush;
        break;

    case IdLabelProjectSummary:
        textColor = kUiText;
        brush = infoBrush;
        break;

    case IdLabelDocument:
    case IdLabelSelection:
    case IdLabelContext:
        textColor = kUiTextSoft;
        brush = infoBrush;
        break;

    case IdLabelHints:
        textColor = kUiTextSoft;
        brush = bodyBrush;
        break;

    default:
        textColor = kUiTextSoft;
        brush = bodyBrush;
        break;
    }

    SetTextColor(dc, textColor);
    return brush;
}

bool UI::isControlActive(WORD controlId) const
{
    switch (controlId)
    {
    case IdButtonEngineStart:
        return visibleState_.engineState == AudioEngine::EngineState::Running;

    case IdButtonPlay:
        return visibleState_.transportState == AudioEngine::TransportState::Playing;

    case IdButtonRecord:
        return workspace_.recordArmed;

    case IdButtonTempoDisplay:
        return tempoDragActive_ || tempoEditHwnd_ != nullptr;

    case IdButtonChronometer:
        return workspace_.chronometerEnabled;

    case IdButtonPatSong:
        return !workspace_.songMode;

    case IdButtonSongMode:
        return workspace_.songMode;

    case IdButtonPlaylistToolPrev:
        return workspace_.playlistTool == EditorTool::Draw;

    case IdButtonPlaylistToolNext:
        return workspace_.playlistTool == EditorTool::Slice;

    case IdButtonBrowser:
        return workspace_.browserVisible;

    case IdButtonChannelRack:
        return workspace_.channelRackVisible;

    case IdButtonPianoRoll:
        return workspace_.pianoRollVisible;

    case IdButtonPlaylist:
        return workspace_.playlistVisible;

    case IdButtonMixer:
        return workspace_.mixerVisible;

    case IdButtonPlugin:
        return workspace_.pluginVisible;

    case IdCheckboxAutomation:
        return visibleState_.automationEnabled;

    case IdCheckboxPdc:
        return visibleState_.pdcEnabled;

    case IdCheckboxAnticipative:
        return visibleState_.anticipativeProcessingEnabled;

    default:
        return false;
    }
}

bool UI::drawThemedButton(const DRAWITEMSTRUCT& drawItem) const
{
    if (drawItem.CtlType != ODT_BUTTON)
    {
        return false;
    }

    const WORD controlId = static_cast<WORD>(drawItem.CtlID);
    const bool active = isControlActive(controlId);
    const bool pressed = (drawItem.itemState & ODS_SELECTED) != 0;
    const bool disabled = (drawItem.itemState & ODS_DISABLED) != 0;
    const bool focused = (drawItem.itemState & ODS_FOCUS) != 0;
    const bool isMenuStripButton =
        controlId >= IdButtonMenuFile && controlId <= IdButtonMenuHelp;
    const bool isQuickAccessButton =
        controlId == IdButtonPianoRoll ||
        controlId == IdButtonChannelRack ||
        controlId == IdButtonMixer;
    const bool isTransportButton =
        controlId == IdButtonPlay ||
        controlId == IdButtonStopTransport;
    const bool isModeButton =
        controlId == IdButtonPatSong ||
        controlId == IdButtonSongMode;
    const bool isPatternNavButton =
        controlId == IdButtonPatternPrev ||
        controlId == IdButtonPatternNext;
    const bool isPlaylistToolButton =
        controlId == IdButtonPlaylistToolPrev ||
        controlId == IdButtonPlaylistToolNext;
    const bool isLegacyToggle =
        controlId == IdCheckboxAutomation ||
        controlId == IdCheckboxPdc ||
        controlId == IdCheckboxAnticipative;

    RECT rect = drawItem.rcItem;
    char label[96]{};
    GetWindowTextA(drawItem.hwndItem, label, static_cast<int>(sizeof(label)));
    SetBkMode(drawItem.hDC, TRANSPARENT);
    static HFONT menuFont = CreateFontA(17, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");
    static HFONT moduleFont = CreateFontA(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");
    static HFONT smallBoldFont = CreateFontA(12, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");
    static HFONT lcdFont = CreateFontA(22, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");
    static HFONT lcdFineFont = CreateFontA(14, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");
    HGDIOBJ oldFont = nullptr;

    if (isMenuStripButton)
    {
        fillRectColor(drawItem.hDC, rect, pressed ? RGB(46, 53, 56) : RGB(56, 64, 67));
        oldFont = SelectObject(drawItem.hDC, menuFont);
        SetTextColor(drawItem.hDC, disabled ? RGB(126, 135, 140) : RGB(229, 236, 239));
        RECT textRect{rect.left + 4, rect.top, rect.right - 2, rect.bottom};
        DrawTextA(drawItem.hDC, label, -1, &textRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        SelectObject(drawItem.hDC, oldFont);
        return true;
    }

    if (isModeButton)
    {
        fillRectColor(drawItem.hDC, rect, kFlToolbarBase);
        const bool songButton = controlId == IdButtonSongMode;
        const COLORREF activeFill =
            songButton
                ? blendColor(kFlSongGreen, RGB(225, 249, 189), pressed ? 3 : 2, 6)
                : blendColor(kFlOrangeDeep, RGB(255, 190, 110), pressed ? 3 : 2, 6);
        const COLORREF fillColor =
            active
                ? activeFill
                : (pressed ? blendColor(kFlPanelDark, kUiShadow, 1, 2) : kFlPanelDark);
        const COLORREF borderColor =
            active
                ? (songButton ? RGB(118, 162, 70) : RGB(188, 118, 56))
                : kFlPanelDarker;
        HBRUSH brush = CreateSolidBrush(fillColor);
        HPEN pen = CreatePen(PS_SOLID, 1, borderColor);
        HGDIOBJ oldBrush = SelectObject(drawItem.hDC, brush);
        HGDIOBJ oldPen = SelectObject(drawItem.hDC, pen);
        RoundRect(drawItem.hDC, rect.left, rect.top, rect.right, rect.bottom, 12, 12);
        SelectObject(drawItem.hDC, oldPen);
        SelectObject(drawItem.hDC, oldBrush);
        DeleteObject(pen);
        DeleteObject(brush);
        oldFont = SelectObject(drawItem.hDC, smallBoldFont);
        SetTextColor(
            drawItem.hDC,
            active
                ? (songButton ? RGB(50, 73, 31) : RGB(66, 46, 27))
                : RGB(225, 232, 235));
        DrawTextA(drawItem.hDC, label, -1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(drawItem.hDC, oldFont);
        return true;
    }

    if (isTransportButton)
    {
        fillRectColor(drawItem.hDC, rect, kFlToolbarBase);
        COLORREF buttonFill = pressed ? kFlPanelDarker : kFlPanelDark;
        HBRUSH brush = CreateSolidBrush(buttonFill);
        HPEN pen = CreatePen(PS_SOLID, 1, kFlPanelDarker);
        HGDIOBJ oldBrush = SelectObject(drawItem.hDC, brush);
        HGDIOBJ oldPen = SelectObject(drawItem.hDC, pen);
        RoundRect(drawItem.hDC, rect.left, rect.top, rect.right, rect.bottom, 16, 16);
        SelectObject(drawItem.hDC, oldPen);
        SelectObject(drawItem.hDC, oldBrush);
        DeleteObject(pen);
        DeleteObject(brush);

        RECT iconRect{
            rect.left + ((rect.right - rect.left - kToolbarGlyphSize) / 2),
            rect.top + ((rect.bottom - rect.top - kToolbarGlyphSize) / 2),
            rect.left + ((rect.right - rect.left - kToolbarGlyphSize) / 2) + kToolbarGlyphSize,
            rect.top + ((rect.bottom - rect.top - kToolbarGlyphSize) / 2) + kToolbarGlyphSize};
        drawToolbarGlyph(drawItem.hDC, controlId, iconRect, RGB(226, 232, 235), active);
        return true;
    }

    if (controlId == IdButtonRecord)
    {
        fillRectColor(drawItem.hDC, rect, kFlToolbarBase);
        RECT outerRect{rect.left + 1, rect.top + 1, rect.right - 1, rect.bottom - 1};
        drawFilledCircle(drawItem.hDC, outerRect, RGB(74, 82, 86), kFlPanelDarker);
        RECT innerRect{rect.left + 7, rect.top + 7, rect.right - 7, rect.bottom - 7};
        drawFilledCircle(drawItem.hDC, innerRect, active ? RGB(234, 96, 78) : RGB(183, 88, 77), RGB(214, 130, 114));
        return true;
    }

    if (controlId == IdButtonChronometer)
    {
        fillRectColor(drawItem.hDC, rect, kFlToolbarBase);
        const bool pulsing =
            workspace_.chronometerEnabled &&
            visibleState_.transportState == AudioEngine::TransportState::Playing;
        const double beatPhase = std::fmod(std::max(0.0, displayedTimelineSeconds()) * (workspace_.tempoBpm / 60.0), 1.0);
        const int pulseStrength =
            pulsing && beatPhase < 0.15
                ? clampValue(static_cast<int>(((0.15 - beatPhase) / 0.15) * 10.0), 0, 10)
                : 0;
        const COLORREF fillColor =
            workspace_.chronometerEnabled
                ? blendColor(kFlOrangeDeep, RGB(255, 204, 137), pulseStrength, 10)
                : kFlClockPassive;

        HBRUSH brush = CreateSolidBrush(fillColor);
        HPEN pen = CreatePen(
            PS_SOLID,
            1,
            workspace_.chronometerEnabled ? RGB(179, 111, 51) : kFlPanelDarker);
        HGDIOBJ oldBrush = SelectObject(drawItem.hDC, brush);
        HGDIOBJ oldPen = SelectObject(drawItem.hDC, pen);
        RoundRect(drawItem.hDC, rect.left, rect.top, rect.right, rect.bottom, 8, 8);
        SelectObject(drawItem.hDC, oldPen);
        SelectObject(drawItem.hDC, oldBrush);
        DeleteObject(pen);
        DeleteObject(brush);

        RECT clockFace{
            rect.left + ((rect.right - rect.left - 14) / 2),
            rect.top + ((rect.bottom - rect.top - 14) / 2),
            rect.left + ((rect.right - rect.left - 14) / 2) + 14,
            rect.top + ((rect.bottom - rect.top - 14) / 2) + 14};
        drawFilledCircle(drawItem.hDC, clockFace, RGB(248, 236, 219), RGB(142, 88, 39));
        drawVerticalLine(drawItem.hDC, clockFace.left + 7, clockFace.top + 3, clockFace.top + 9, RGB(112, 73, 38));
        drawHorizontalLine(drawItem.hDC, clockFace.left + 7, clockFace.left + 11, clockFace.top + 9, RGB(112, 73, 38));
        return true;
    }

    if (controlId == IdButtonTempoDisplay)
    {
        fillRectColor(drawItem.hDC, rect, kFlToolbarBase);
        HBRUSH fieldBrush = CreateSolidBrush(kFlLcdFill);
        HPEN fieldPen = CreatePen(PS_SOLID, 1, active ? kFlOrangeDeep : kFlLcdBorder);
        HGDIOBJ oldBrush = SelectObject(drawItem.hDC, fieldBrush);
        HGDIOBJ oldPen = SelectObject(drawItem.hDC, fieldPen);
        RoundRect(drawItem.hDC, rect.left, rect.top, rect.right, rect.bottom, 8, 8);
        SelectObject(drawItem.hDC, oldPen);
        SelectObject(drawItem.hDC, oldBrush);
        DeleteObject(fieldPen);
        DeleteObject(fieldBrush);

        const double roundedTempo = roundTempoBpm(workspace_.tempoBpm);
        int baseTempo = static_cast<int>(std::floor(roundedTempo + 0.0001));
        int fineTempo = static_cast<int>(std::round((roundedTempo - static_cast<double>(baseTempo)) * 1000.0));
        if (fineTempo >= 1000)
        {
            ++baseTempo;
            fineTempo -= 1000;
        }

        RECT baseRect{rect.left + 8, rect.top + 3, rect.left + 48, rect.bottom - 3};
        RECT fineRect{rect.left + 46, rect.top + 8, rect.right - 8, rect.bottom - 6};
        drawVerticalLine(drawItem.hDC, rect.left + 46, rect.top + 6, rect.bottom - 6, RGB(196, 206, 209));

        HGDIOBJ oldLcdFont = SelectObject(drawItem.hDC, lcdFont);
        SetTextColor(drawItem.hDC, kFlLcdText);
        const std::string baseText = std::to_string(baseTempo);
        DrawTextA(drawItem.hDC, baseText.c_str(), -1, &baseRect, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

        SelectObject(drawItem.hDC, lcdFineFont);
        char fineBuffer[8]{};
        std::snprintf(fineBuffer, sizeof(fineBuffer), ".%03d", fineTempo);
        SetTextColor(drawItem.hDC, tempoDragFineAdjust_ ? kFlOrangeDeep : RGB(90, 101, 105));
        DrawTextA(drawItem.hDC, fineBuffer, -1, &fineRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        SelectObject(drawItem.hDC, oldLcdFont);
        return true;
    }

    if (controlId == IdButtonTempoDown || controlId == IdButtonTempoUp)
    {
        fillRectColor(drawItem.hDC, rect, kFlToolbarBase);
        return true;
    }

    if (isPatternNavButton)
    {
        fillRectColor(drawItem.hDC, rect, kFlToolbarBase);
        const COLORREF fillColor =
            controlId == IdButtonPatternPrev
                ? (patternPopupOpen_
                    ? blendColor(kFlOrangeDeep, RGB(255, 199, 130), 1, 4)
                    : (pressed ? blendColor(kFlPanelDark, kUiShadow, 1, 2) : kFlPanelDark))
                : (pressed ? blendColor(kFlPanelDark, kUiShadow, 1, 2) : kFlPanelDark);
        fillRectColor(drawItem.hDC, rect, fillColor);
        drawSurfaceFrame(
            drawItem.hDC,
            rect,
            controlId == IdButtonPatternPrev && patternPopupOpen_ ? RGB(160, 96, 42) : kFlPanelDarker);
        if (controlId == IdButtonPatternPrev)
        {
            POINT arrow[3]{};
            if (patternPopupOpen_)
            {
                arrow[0] = POINT{rect.left + 7, rect.top + 12};
                arrow[1] = POINT{rect.left + 17, rect.top + 12};
                arrow[2] = POINT{rect.left + 12, rect.bottom - 10};
            }
            else
            {
                arrow[0] = POINT{rect.left + 8, rect.top + 8};
                arrow[1] = POINT{rect.left + 8, rect.bottom - 8};
                arrow[2] = POINT{rect.right - 7, rect.top + ((rect.bottom - rect.top) / 2)};
            }
            const COLORREF arrowColor = patternPopupOpen_ ? RGB(95, 51, 15) : RGB(225, 232, 235);
            HBRUSH arrowBrush = CreateSolidBrush(arrowColor);
            HPEN arrowPen = CreatePen(PS_SOLID, 1, arrowColor);
            HGDIOBJ oldArrowBrush = SelectObject(drawItem.hDC, arrowBrush);
            HGDIOBJ oldArrowPen = SelectObject(drawItem.hDC, arrowPen);
            Polygon(drawItem.hDC, arrow, 3);
            SelectObject(drawItem.hDC, oldArrowPen);
            SelectObject(drawItem.hDC, oldArrowBrush);
            DeleteObject(arrowPen);
            DeleteObject(arrowBrush);
        }
        else
        {
            drawHorizontalLine(drawItem.hDC, rect.left + 7, rect.right - 7, rect.top + ((rect.bottom - rect.top) / 2), RGB(225, 232, 235));
            drawVerticalLine(drawItem.hDC, rect.left + ((rect.right - rect.left) / 2), rect.top + 8, rect.bottom - 8, RGB(225, 232, 235));
        }
        return true;
    }

    if (isQuickAccessButton)
    {
        const COLORREF fillColor = active ? RGB(66, 76, 82) : RGB(74, 84, 89);
        fillRectColor(drawItem.hDC, rect, fillColor);
        drawSurfaceFrame(drawItem.hDC, rect, RGB(57, 66, 71));
        oldFont = SelectObject(drawItem.hDC, moduleFont);
        SetTextColor(drawItem.hDC, disabled ? RGB(118, 126, 132) : RGB(227, 234, 237));
        RECT textRect{rect.left + 4, rect.top + 4, rect.right - 4, rect.bottom - 4};
        DrawTextA(drawItem.hDC, label, -1, &textRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        SelectObject(drawItem.hDC, oldFont);
        return true;
    }

    if (isPlaylistToolButton)
    {
        fillRectColor(drawItem.hDC, rect, kFlToolbarBase);
        const COLORREF fillColor =
            active
                ? blendColor(kUiPetrol, kUiLime, 1, 6)
                : (pressed ? blendColor(kFlPanelDark, kUiShadow, 1, 2) : kFlPanelDark);
        HBRUSH brush = CreateSolidBrush(fillColor);
        HPEN pen = CreatePen(PS_SOLID, 1, active ? blendColor(kUiLime, kUiLine, 1, 3) : kFlPanelDarker);
        HGDIOBJ oldBrush = SelectObject(drawItem.hDC, brush);
        HGDIOBJ oldPen = SelectObject(drawItem.hDC, pen);
        RoundRect(drawItem.hDC, rect.left, rect.top, rect.right, rect.bottom, 10, 10);
        SelectObject(drawItem.hDC, oldPen);
        SelectObject(drawItem.hDC, oldBrush);
        DeleteObject(pen);
        DeleteObject(brush);
        oldFont = SelectObject(drawItem.hDC, smallBoldFont);
        SetTextColor(drawItem.hDC, active ? RGB(223, 236, 226) : RGB(225, 232, 235));
        DrawTextA(drawItem.hDC, label, -1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(drawItem.hDC, oldFont);
        return true;
    }

    if (isLegacyToggle)
    {
        fillRectColor(drawItem.hDC, rect, RGB(61, 69, 74));
        drawSurfaceFrame(drawItem.hDC, rect, active ? RGB(166, 240, 106) : RGB(74, 85, 96));
        SetTextColor(drawItem.hDC, active ? RGB(166, 240, 106) : RGB(199, 205, 211));
        DrawTextA(drawItem.hDC, label, -1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        return true;
    }

    COLORREF outerColor = pressed ? kUiShadow : blendColor(kUiAnthracite, kUiShadow, 1, 4);
    COLORREF fillColor = active ? blendColor(kUiAnthracite, kUiPetrol, 1, 2) : kUiAnthracite;
    if (pressed)
    {
        fillColor = blendColor(fillColor, kUiShadow, 1, 3);
    }

    fillRectColor(drawItem.hDC, rect, outerColor);
    RECT innerRect{rect.left + 1, rect.top + 1, rect.right - 1, rect.bottom - 1};
    fillRectColor(drawItem.hDC, innerRect, fillColor);
    drawSurfaceFrame(drawItem.hDC, innerRect, active ? blendColor(kUiLime, kUiLine, 1, 3) : kUiLineSoft);

    const COLORREF textColor =
        disabled ? kUiTextDim :
        active ? kUiLime :
        kUiText;
    SetTextColor(drawItem.hDC, textColor);
    DrawTextA(drawItem.hDC, label, -1, &innerRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    if (focused)
    {
        RECT focusRect{rect.left + 3, rect.top + 3, rect.right - 3, rect.bottom - 3};
        DrawFocusRect(drawItem.hDC, &focusRect);
    }

    return true;
}

void UI::drawSurfaceHeader(HDC dc, const RECT& rect, const std::string& title, const std::string& subtitle) const
{
    RECT headerRect{rect.left, rect.top, rect.right, rect.top + kSurfaceHeaderHeight};
    fillRectColor(dc, headerRect, blendColor(kUiAnthracite, kUiPetrol, 1, 4));
    drawHorizontalLine(dc, rect.left, rect.right, headerRect.bottom - 1, kUiLineSoft);

    RECT titleRect{rect.left + 8, rect.top + 3, rect.right - 8, rect.top + kSurfaceHeaderHeight - 2};
    SetTextColor(dc, kUiText);
    DrawTextA(dc, title.c_str(), -1, &titleRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    if (!subtitle.empty())
    {
        RECT subtitleRect{rect.right - 280, rect.top + 3, rect.right - 8, rect.top + kSurfaceHeaderHeight - 2};
        SetTextColor(dc, kUiTextSoft);
        DrawTextA(dc, subtitle.c_str(), -1, &subtitleRect, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    }
}

void UI::drawSurfaceFrame(HDC dc, const RECT& rect, COLORREF borderColor) const
{
    HPEN pen = CreatePen(PS_SOLID, 1, borderColor);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
    Rectangle(dc, rect.left, rect.top, rect.right, rect.bottom);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(pen);
}

std::string UI::browserDropTargetLabel() const
{
    if (workspaceModel_.browserEntries.empty())
    {
        return "Project";
    }

    const BrowserEntry& entry = workspaceModel_.browserEntries[static_cast<std::size_t>(workspaceModel_.selectedBrowserIndex)];
    return entry.label;
}
