void UI::rebuildPlaylistVisuals(const RECT& rect)
{
    interactionState_.playlistClipVisuals.clear();
    const int timelineHeight = kSurfaceHeaderHeight + kPlaylistTimelineHeight;
    const int laneHeight = kPlaylistLaneHeight;
    const int leftInset = kPlaylistTrackHeaderWidth;
    const int columnWidth = std::max<int>(24, (static_cast<int>(rect.right) - leftInset) / kPlaylistCellCount);

    for (const auto& block : workspaceModel_.playlistBlocks)
    {
        UiClipVisual visual{};
        visual.clipId = block.clipId;
        visual.trackId = block.trackId;
        visual.label = block.label;
        visual.clipType = block.clipType;
        visual.rect.x = leftInset + (block.startCell * columnWidth);
        visual.rect.y = timelineHeight + 6 + (block.lane * laneHeight);
        visual.rect.width = std::max(42, block.lengthCells * columnWidth - 6);
        visual.rect.height = laneHeight - 10;
        visual.selected = block.selected || (block.clipId == interactionState_.selectedClipId);
        visual.automation = false;
        visual.resizeLeftHot = false;
        visual.resizeRightHot = false;
        visual.rect.x = clampValue(visual.rect.x, leftInset, rect.right - 50);
        visual.rect.width = std::min<int>(visual.rect.width, std::max<int>(30, static_cast<int>(rect.right) - visual.rect.x - 6));
        interactionState_.playlistClipVisuals.push_back(visual);
    }

    for (const auto& automation : workspaceModel_.automationLanes)
    {
        UiClipVisual visual{};
        visual.clipId = automation.clipId;
        visual.trackId = 0;
        visual.label = automation.target;
        visual.clipType = "Automation";
        visual.rect.x = leftInset + (automation.startCell * columnWidth);
        visual.rect.y = timelineHeight + 6 + (automation.lane * laneHeight);
        visual.rect.width = std::max(42, automation.lengthCells * columnWidth - 6);
        visual.rect.height = laneHeight - 10;
        visual.selected = automation.selected || (automation.clipId == interactionState_.selectedClipId);
        visual.automation = true;
        visual.resizeLeftHot = false;
        visual.resizeRightHot = false;
        visual.rect.x = clampValue(visual.rect.x, leftInset, rect.right - 50);
        visual.rect.width = std::min<int>(visual.rect.width, std::max<int>(30, static_cast<int>(rect.right) - visual.rect.x - 6));
        interactionState_.playlistClipVisuals.push_back(visual);
    }
}

void UI::rebuildPianoVisuals(const RECT& rect)
{
    interactionState_.pianoNoteVisuals.clear();
    const int keyWidth = 48;
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

    auto addEntry = [&](std::string category, std::string label, std::string subtitle, bool favorite = false, int indentLevel = 0, bool group = false, bool expanded = false)
    {
        workspaceModel_.browserEntries.push_back(
            BrowserEntry{
                std::move(category),
                std::move(label),
                std::move(subtitle),
                favorite,
                indentLevel,
                group,
                expanded});
    };

    switch (std::min<std::size_t>(workspace_.browserTabIndex, kBrowserTabCount - 1))
    {
    case 0:
        addEntry(
            "Project",
            visibleState_.project.projectName.empty() ? "Current project" : visibleState_.project.projectName,
            visibleState_.document.sessionPath.empty() ? "Playlist, patterns and channels" : visibleState_.document.sessionPath,
            false,
            0,
            true,
            true);
        addEntry(
            "Patterns",
            "Patterns",
            std::to_string(workspaceModel_.patterns.size()) + " pattern slots",
            false,
            0,
            true,
            true);
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
                "Pattern",
                pattern.name,
                std::to_string(pattern.lanes.size()) + " channels | " +
                    std::to_string(laneCountWithSteps) + " active step lanes | " +
                    std::to_string(pattern.lengthInBars) + " bars",
                pattern.patternNumber == workspace_.activePattern,
                1);
        }

        if (!visibleState_.project.tracks.empty())
        {
            addEntry(
                "Channel Rack",
                "Channels",
                std::to_string(visibleState_.project.tracks.size()) + " playlist tracks",
                false,
                0,
                true,
                true);

            const std::size_t visibleTrackCount = std::min<std::size_t>(visibleState_.project.tracks.size(), 6);
            for (std::size_t index = 0; index < visibleTrackCount; ++index)
            {
                const auto& track = visibleState_.project.tracks[index];
                addEntry(
                    "Track",
                    track.name,
                    std::to_string(track.clips.size()) + " clips | Bus " + std::to_string(track.busId),
                    track.trackId == visibleState_.selection.selectedTrackId,
                    1);
            }
        }
        break;

    case 1:
    {
        addEntry("Packs", "Packs", "Samples, one-shots and rendered audio", false, 0, true, true);
        bool addedAudio = false;
        for (const auto& block : workspaceModel_.playlistBlocks)
        {
            if (block.clipType != "Audio")
            {
                continue;
            }

            addedAudio = true;
            addEntry(
                "Audio",
                block.label,
                "Playlist lane " + std::to_string(block.lane + 1) + " | Start " +
                    std::to_string(block.startCell + 1) + " | Length " + std::to_string(block.lengthCells) + " cells",
                block.selected,
                1);
        }

        if (!addedAudio)
        {
            addEntry("Sample", "Lead Vox Chop", "Preview sample ready for playlist", true, 1);
            addEntry("Sample", "Impact Downlifter", "Transition FX clip", false, 1);
            addEntry("Sample", "Analog Bass One-Shot", "Sampler-ready source clip", false, 1);
        }

        addEntry("Presets", "Plugin database", "Presets, wrappers and channel states", false, 0, true, true);
        addEntry("Preset", "Sampler defaults", "Compact rack preset ready for drag-drop", false, 1);
        break;
    }

    default:
        addEntry("Current project", "Automation clips", "Playlist envelopes and linked controls", false, 0, true, true);
        for (const auto& automation : workspaceModel_.automationLanes)
        {
            addEntry(
                "Automation",
                automation.target,
                "Lane " + std::to_string(automation.lane + 1) + " | Start " +
                    std::to_string(automation.startCell + 1) + " | " +
                    std::to_string(automation.values.size()) + " points",
                automation.selected,
                1);
        }

        if (workspaceModel_.automationLanes.empty())
        {
            addEntry("Automation", "Master Volume", "Fallback envelope lane", true, 1);
            addEntry("Automation", "Filter Cutoff", "Sweep automation clip", false, 1);
        }
        break;
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
    const std::size_t expectedLaneCount = std::max<std::size_t>(1, visibleState_.project.tracks.empty() ? 1 : visibleState_.project.tracks.size());
    bool rebuildPatterns = workspaceModel_.patterns.empty();

    if (!rebuildPatterns)
    {
        for (const auto& pattern : workspaceModel_.patterns)
        {
            if (pattern.lanes.size() != expectedLaneCount)
            {
                rebuildPatterns = true;
                break;
            }
        }
    }

    if (!rebuildPatterns)
    {
        return;
    }

    workspaceModel_.patterns.clear();
    for (int patternNumber = 1; patternNumber <= 4; ++patternNumber)
    {
        workspaceModel_.patterns.push_back(makePatternState(patternNumber));
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

    int lane = 0;
    for (const auto& track : visibleState_.project.tracks)
    {
        for (const auto& clip : track.clips)
        {
            PlaylistBlockState block{};
            block.clipId = clip.clipId;
            block.trackId = track.trackId;
            block.lane = lane;
            block.startCell = clampValue(static_cast<int>(clip.startTimeSeconds * 2.0), 0, kPlaylistCellCount - 2);
            block.lengthCells = std::max(2, static_cast<int>(clip.durationSeconds * 2.0 + 0.5));
            block.label = clip.name.empty() ? ("Clip " + std::to_string(clip.clipId)) : clip.name;
            block.clipType = clip.sourceLabel;
            block.patternNumber = workspace_.activePattern;
            block.muted = clip.muted;
            block.selected = (clip.clipId == interactionState_.selectedClipId);
            workspaceModel_.playlistBlocks.push_back(std::move(block));
        }
        ++lane;
    }

    if (workspaceModel_.playlistBlocks.empty())
    {
        workspaceModel_.playlistBlocks.push_back(PlaylistBlockState{1, 0, 0, 0, 4, "Pattern 1 Intro", "Pattern", 1, false, false});
        workspaceModel_.playlistBlocks.push_back(PlaylistBlockState{2, 0, 1, 4, 6, "Pattern 2 Bass", "Pattern", 2, false, false});
        workspaceModel_.playlistBlocks.push_back(PlaylistBlockState{3, 0, 2, 10, 8, "Vocal Chop", "Audio", 0, false, false});
        workspaceModel_.playlistBlocks.push_back(PlaylistBlockState{4, 0, 0, 18, 6, "Pattern 3 Fill", "Pattern", 3, false, false});
    }

    workspaceModel_.markers = {
        {"Intro", 0},
        {"Drop", 8},
        {"Break", 16},
        {"Outro", 24}};
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
        strip.volumeDb = -4.5 + static_cast<double>(insertSlot % 5);
        strip.pan = (insertSlot % 3 == 0) ? -0.12 : ((insertSlot % 4 == 0) ? 0.15 : 0.0);
        strip.peakLevel = normalizedMeterFill(strip.volumeDb);
        strip.routeTarget = (insertSlot % 3 == 0) ? 0 : -1;
        strip.sendAmount = 0.12 * static_cast<double>(insertSlot % 4);
        strip.effects = {"EQ", "Comp", insertSlot % 2 == 0 ? "Saturator" : "Delay"};
        workspaceModel_.mixerStrips.push_back(std::move(strip));
        ++insertSlot;
    }

    if (workspaceModel_.mixerStrips.empty())
    {
        workspaceModel_.mixerStrips.push_back(MixerStripState{1, "Kick Bus", 1, -3.0, 0.0, 78, false, false, -1, 0.10, {"EQ", "Clipper"}});
        workspaceModel_.mixerStrips.push_back(MixerStripState{2, "Music Bus", 2, -4.5, -0.08, 66, false, false, 0, 0.26, {"EQ", "Compressor", "Stereo"}});
    }

    workspaceModel_.mixerStrips.push_back(MixerStripState{0, "Master", 0, -1.2, 0.0, 92, false, false, -1, 0.0, {"Glue", "Limiter"}});
}

void UI::rebuildChannelSettings()
{
    workspaceModel_.channelSettings.clear();

    std::size_t laneIndex = 0;
    for (const auto& lane : workspaceModel_.patternLanes)
    {
        ChannelSettingsState settings{};
        settings.trackId = lane.trackId;
        settings.name = lane.name;
        settings.gain = 0.72 + (static_cast<double>(laneIndex % 4) * 0.08);
        settings.pan = (laneIndex % 3 == 0) ? -0.12 : ((laneIndex % 4 == 0) ? 0.16 : 0.0);
        settings.pitchSemitones = static_cast<double>((static_cast<int>(laneIndex) % 5) - 2);
        settings.attackMs = 8.0 + static_cast<double>(laneIndex * 3);
        settings.releaseMs = 150.0 + static_cast<double>(laneIndex * 24);
        settings.filterCutoffHz = 6200.0 + static_cast<double>(laneIndex * 850);
        settings.resonance = 0.18 + static_cast<double>(laneIndex) * 0.03;
        settings.mixerInsert = static_cast<int>(laneIndex + 1);
        settings.routeTarget = (laneIndex % 2 == 0) ? 0 : -1;
        settings.reverse = (laneIndex % 5 == 0);
        settings.timeStretch = true;
        settings.pluginRack = {
            laneIndex % 2 == 0 ? "Sampler" : "Synth Rack",
            "Parametric EQ",
            laneIndex % 3 == 0 ? "Soft Clipper" : "Compressor"};
        settings.presets = {
            lane.name + " Init",
            lane.name + " Bright",
            lane.name + " Wide"};
        workspaceModel_.channelSettings.push_back(std::move(settings));
        ++laneIndex;
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
    workspaceModel_.automationLanes.push_back(AutomationLaneState{"Master Volume", {85, 78, 80, 92, 88, 74, 96, 83}, 4001, automationLaneBase + 0, 0, 8, false});
    workspaceModel_.automationLanes.push_back(AutomationLaneState{"Lead Filter", {24, 28, 36, 48, 58, 72, 81, 94}, 4002, automationLaneBase + 1, 8, 8, false});
    workspaceModel_.automationLanes.push_back(AutomationLaneState{"Delay Send", {8, 12, 18, 12, 22, 14, 26, 18}, 4003, automationLaneBase + 2, 18, 6, false});
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
    fillRectColor(dc, toolbarRect, RGB(93, 111, 113));
    fillRectColor(dc, workspaceRect, blendColor(kUiPetrol, kUiShadow, 1, 4));
    drawHorizontalLine(dc, clientRect.left, clientRect.right, menuStripHeight - 1, RGB(73, 82, 86));
    drawHorizontalLine(dc, clientRect.left, clientRect.right, topPanelY + topPanelHeight - 1, kUiLineSoft);

    if (workspace_.browserVisible)
    {
        const int browserDividerX = kOuterPadding + kBrowserWidth + (kGap / 2);
        drawVerticalLine(dc, browserDividerX, workspaceTop, clientRect.bottom - kOuterPadding, kUiLineSoft);
    }

    const int timePanelX = 22;
    const int timePanelY = topPanelY + 12;
    const int timePanelWidth = 210;
    const int displayHeight = 50;
    const int meterY = timePanelY + displayHeight + 8;
    const int meterHeight = 34;
    const int spectrumPanelX = timePanelX + timePanelWidth + 14;
    const int spectrumPanelWidth = 140;
    const int spectrumPanelHeight = displayHeight + meterHeight + 8;
    const int transportX = spectrumPanelX + spectrumPanelWidth + 28;
    const int transportY = topPanelY + 18;
    const int patternClusterWidth = 154;
    const int clientRight = static_cast<int>(clientRect.right);
    const int patternClusterLimit = clientRight - kOuterPadding - patternClusterWidth - 20;
    const int patternClusterDesired = std::max(transportX + 320, clientRight - 460);
    const int patternClusterX = std::min(patternClusterLimit, patternClusterDesired);

    RECT timeDisplayRect{timePanelX, timePanelY, timePanelX + timePanelWidth, timePanelY + displayHeight};
    fillRectColor(dc, timeDisplayRect, RGB(83, 101, 103));
    drawSurfaceFrame(dc, timeDisplayRect, RGB(73, 88, 91));

    const int totalCentiseconds = std::max(0, static_cast<int>(visibleState_.timelineSeconds * 100.0 + 0.5));
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
    RECT timeValueRect{timeDisplayRect.left + 14, timeDisplayRect.top + 2, timeDisplayRect.right - 10, timeDisplayRect.bottom - 4};
    SetTextColor(dc, RGB(230, 239, 241));
    DrawTextA(dc, timeBuffer, -1, &timeValueRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    SelectObject(dc, smallFont);
    RECT timeModeRect{timeDisplayRect.right - 54, timeDisplayRect.top + 3, timeDisplayRect.right - 8, timeDisplayRect.top + 16};
    SetTextColor(dc, RGB(210, 219, 223));
    DrawTextA(dc, "M:S:CS", -1, &timeModeRect, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, oldFont);

    RECT meterRect{timePanelX, meterY, timePanelX + timePanelWidth, meterY + meterHeight};
    fillRectColor(dc, meterRect, RGB(62, 71, 76));
    drawSurfaceFrame(dc, meterRect, RGB(58, 66, 70));

    const double meterBase =
        visibleState_.transportState == AudioEngine::TransportState::Playing
            ? (0.40 + (0.26 * (0.5 + (0.5 * std::sin((visibleState_.timelineSeconds * 4.0) + 0.35)))))
            : (0.34 + (visibleState_.cpuLoadApprox * 0.18));
    const int meterFillWidth = clampValue(static_cast<int>((timePanelWidth - 14) * meterBase), 30, timePanelWidth - 20);
    RECT meterFillTop{meterRect.left + 8, meterRect.top + 6, meterRect.left + 8 + meterFillWidth, meterRect.top + 16};
    RECT meterFillBottom{meterRect.left + 8, meterRect.top + 18, meterRect.left + 8 + meterFillWidth, meterRect.top + 28};
    fillRectColor(dc, meterFillTop, RGB(232, 228, 216));
    fillRectColor(dc, meterFillBottom, RGB(232, 228, 216));
    drawHorizontalLine(dc, meterFillTop.left, meterFillTop.right, meterRect.top + 17, RGB(201, 196, 183));

    RECT spectrumRect{spectrumPanelX, timePanelY, spectrumPanelX + spectrumPanelWidth, timePanelY + spectrumPanelHeight};
    fillRectColor(dc, spectrumRect, RGB(74, 86, 91));
    drawSurfaceFrame(dc, spectrumRect, RGB(73, 88, 91));

    RECT spectrumInnerRect{spectrumRect.left + 6, spectrumRect.top + 6, spectrumRect.right - 6, spectrumRect.bottom - 6};
    fillRectColor(dc, spectrumInnerRect, RGB(62, 71, 75));
    drawHorizontalLine(dc, spectrumInnerRect.left, spectrumInnerRect.right, spectrumInnerRect.bottom - 22, RGB(87, 99, 103));

    POINT spectrumPoints[16]{};
    const int spectrumWidth = spectrumInnerRect.right - spectrumInnerRect.left;
    const int spectrumHeight = spectrumInnerRect.bottom - spectrumInnerRect.top;
    const double motionBase =
        visibleState_.transportState == AudioEngine::TransportState::Playing
            ? visibleState_.timelineSeconds
            : (static_cast<double>(visibleState_.callbackCount % 512) * 0.013);
    spectrumPoints[0] = POINT{spectrumInnerRect.left, spectrumInnerRect.bottom};
    for (int index = 0; index < 14; ++index)
    {
        const double phase = motionBase + static_cast<double>(index) * 0.41;
        const double composite =
            (0.45 + (0.28 * std::sin(phase * 1.3)) + (0.20 * std::sin((phase * 2.7) + 1.1))) *
            (0.62 + (0.22 * std::sin((phase * 0.7) + 0.4)));
        const int amplitude = clampValue(static_cast<int>(composite * static_cast<double>(spectrumHeight - 18)), 8, spectrumHeight - 16);
        const int x = spectrumInnerRect.left + ((spectrumWidth - 1) * index) / 13;
        const int y = spectrumInnerRect.bottom - 8 - amplitude;
        spectrumPoints[index + 1] = POINT{x, y};
    }
    spectrumPoints[15] = POINT{spectrumInnerRect.right, spectrumInnerRect.bottom};

    HBRUSH spectrumBrush = CreateSolidBrush(RGB(216, 222, 223));
    HPEN spectrumPen = CreatePen(PS_SOLID, 1, RGB(216, 222, 223));
    HGDIOBJ oldSpectrumBrush = SelectObject(dc, spectrumBrush);
    HGDIOBJ oldSpectrumPen = SelectObject(dc, spectrumPen);
    Polygon(dc, spectrumPoints, 16);
    SelectObject(dc, oldSpectrumPen);
    SelectObject(dc, oldSpectrumBrush);
    DeleteObject(spectrumPen);
    DeleteObject(spectrumBrush);

    RECT tempoFieldRect{transportX + 198, transportY + 4, transportX + 288, transportY + 42};
    fillRectColor(dc, tempoFieldRect, RGB(208, 222, 227));
    drawSurfaceFrame(dc, tempoFieldRect, RGB(184, 196, 201));

    RECT patternFieldRect{patternClusterX + 34, transportY + 2, patternClusterX + 90, transportY + 42};
    fillRectColor(dc, patternFieldRect, RGB(209, 222, 228));
    drawSurfaceFrame(dc, patternFieldRect, RGB(184, 196, 201));

    const int modeBarLeft = transportX - 2;
    const int modeBarTop = transportY + 68;
    const int modeBarWidth = std::max(160, std::min(332, patternClusterX - transportX - 40));
    const int modeKnobPosition = workspace_.songMode ? static_cast<int>(modeBarWidth * 0.67) : static_cast<int>(modeBarWidth * 0.15);
    RECT modeLineRect{modeBarLeft + 18, modeBarTop + 7, modeBarLeft + modeBarWidth, modeBarTop + 9};
    fillRectColor(dc, modeLineRect, RGB(123, 138, 143));
    RECT modeFillRect{modeLineRect.left, modeLineRect.top, modeLineRect.left + modeKnobPosition, modeLineRect.bottom};
    fillRectColor(dc, modeFillRect, workspace_.songMode ? RGB(98, 109, 113) : RGB(240, 162, 75));

    RECT knobRect{
        modeLineRect.left + modeKnobPosition - 9,
        modeBarTop,
        modeLineRect.left + modeKnobPosition + 9,
        modeBarTop + 18};
    drawFilledCircle(dc, knobRect, RGB(59, 68, 73), RGB(46, 53, 58));
    RECT knobHighlight{knobRect.left + 6, knobRect.top + 4, knobRect.left + 10, knobRect.top + 12};
    fillRectColor(dc, knobHighlight, RGB(209, 138, 62));
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

    case IdButtonPatSong:
        return workspace_.songMode;

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
    const bool isPatternNavButton =
        controlId == IdButtonPatternPrev ||
        controlId == IdButtonPatternNext;
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

    if (controlId == IdButtonPatSong)
    {
        HBRUSH shellBrush = CreateSolidBrush(RGB(61, 69, 74));
        HPEN shellPen = CreatePen(PS_SOLID, 1, RGB(44, 51, 55));
        HGDIOBJ oldBrush = SelectObject(drawItem.hDC, shellBrush);
        HGDIOBJ oldPen = SelectObject(drawItem.hDC, shellPen);
        RoundRect(drawItem.hDC, rect.left, rect.top, rect.right, rect.bottom, 10, 10);
        SelectObject(drawItem.hDC, oldPen);
        SelectObject(drawItem.hDC, oldBrush);
        DeleteObject(shellPen);
        DeleteObject(shellBrush);

        RECT patRect{rect.left + 2, rect.top + 2, rect.right - 2, rect.top + ((rect.bottom - rect.top) / 2)};
        RECT songRect{rect.left + 2, patRect.bottom, rect.right - 2, rect.bottom - 2};
        const bool patternActive = !workspace_.songMode;
        fillRectColor(drawItem.hDC, patRect, patternActive ? RGB(240, 162, 75) : RGB(39, 48, 57));
        fillRectColor(drawItem.hDC, songRect, workspace_.songMode ? RGB(240, 162, 75) : RGB(39, 48, 57));
        drawHorizontalLine(drawItem.hDC, patRect.left, patRect.right, patRect.bottom, RGB(46, 53, 58));

        oldFont = SelectObject(drawItem.hDC, smallBoldFont);
        SetTextColor(drawItem.hDC, patternActive ? RGB(59, 49, 41) : RGB(138, 150, 156));
        DrawTextA(drawItem.hDC, "PAT", -1, &patRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SetTextColor(drawItem.hDC, workspace_.songMode ? RGB(59, 49, 41) : RGB(138, 150, 156));
        DrawTextA(drawItem.hDC, "SONG", -1, &songRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(drawItem.hDC, oldFont);
        return true;
    }

    if (isTransportButton)
    {
        COLORREF buttonFill = pressed ? RGB(48, 56, 61) : RGB(58, 66, 72);
        HBRUSH brush = CreateSolidBrush(buttonFill);
        HPEN pen = CreatePen(PS_SOLID, 1, RGB(42, 49, 54));
        HGDIOBJ oldBrush = SelectObject(drawItem.hDC, brush);
        HGDIOBJ oldPen = SelectObject(drawItem.hDC, pen);
        RoundRect(drawItem.hDC, rect.left, rect.top, rect.right, rect.bottom, 14, 14);
        SelectObject(drawItem.hDC, oldPen);
        SelectObject(drawItem.hDC, oldBrush);
        DeleteObject(pen);
        DeleteObject(brush);

        RECT iconRect{
            rect.left + ((rect.right - rect.left - kToolbarGlyphSize) / 2),
            rect.top + ((rect.bottom - rect.top - kToolbarGlyphSize) / 2),
            rect.left + ((rect.right - rect.left - kToolbarGlyphSize) / 2) + kToolbarGlyphSize,
            rect.top + ((rect.bottom - rect.top - kToolbarGlyphSize) / 2) + kToolbarGlyphSize};
        drawToolbarGlyph(drawItem.hDC, controlId, iconRect, RGB(216, 224, 227), active);
        return true;
    }

    if (controlId == IdButtonRecord)
    {
        fillRectColor(drawItem.hDC, rect, RGB(93, 111, 113));
        RECT outerRect{rect.left + 1, rect.top + 1, rect.right - 1, rect.bottom - 1};
        drawFilledCircle(drawItem.hDC, outerRect, RGB(74, 83, 88), RGB(60, 68, 73));
        RECT innerRect{rect.left + 7, rect.top + 7, rect.right - 7, rect.bottom - 7};
        drawFilledCircle(drawItem.hDC, innerRect, RGB(225, 92, 77), RGB(197, 123, 104));
        return true;
    }

    if (controlId == IdButtonTempoDown || controlId == IdButtonTempoUp)
    {
        fillRectColor(drawItem.hDC, rect, RGB(93, 111, 113));
        return true;
    }

    if (isPatternNavButton)
    {
        fillRectColor(drawItem.hDC, rect, pressed ? RGB(52, 60, 66) : RGB(62, 71, 77));
        drawSurfaceFrame(drawItem.hDC, rect, RGB(50, 58, 63));
        if (controlId == IdButtonPatternPrev)
        {
            POINT arrow[3]{
                POINT{rect.left + 20, rect.top + 10},
                POINT{rect.left + 12, rect.top + ((rect.bottom - rect.top) / 2)},
                POINT{rect.left + 20, rect.bottom - 10}};
            HBRUSH arrowBrush = CreateSolidBrush(RGB(225, 232, 235));
            HPEN arrowPen = CreatePen(PS_SOLID, 1, RGB(225, 232, 235));
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
            drawHorizontalLine(drawItem.hDC, rect.left + 10, rect.right - 10, rect.top + ((rect.bottom - rect.top) / 2), RGB(225, 232, 235));
            drawVerticalLine(drawItem.hDC, rect.left + ((rect.right - rect.left) / 2), rect.top + 10, rect.bottom - 10, RGB(225, 232, 235));
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
        DrawTextA(drawItem.hDC, label, -1, &textRect, DT_CENTER | DT_VCENTER | DT_WORDBREAK);
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
        return "Rack / Playlist / Mixer";
    }

    const BrowserEntry& entry = workspaceModel_.browserEntries[static_cast<std::size_t>(workspaceModel_.selectedBrowserIndex)];
    return entry.label + " -> Rack / Playlist / Mixer";
}

