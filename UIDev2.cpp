void UI::handlePluginManagerCommand(WORD commandId)
{
    if (pluginManagerHwnd_ == nullptr)
    {
        return;
    }

    if (commandId == 4001)
    {
        char buffer[256]{};
        GetWindowTextA(pluginManagerSearchEdit_, buffer, static_cast<int>(sizeof(buffer)));
        pluginManagerState_.searchText = buffer;
        applyPluginSearchFilter();
        updatePluginManagerControls();
        return;
    }

    if (commandId == 4002)
    {
        const LRESULT selection = SendMessageA(pluginManagerListBox_, LB_GETCURSEL, 0, 0);
        if (selection != LB_ERR)
        {
            pluginManagerState_.selectedIndex = static_cast<std::size_t>(selection);
        }
        updatePluginManagerControls();
        return;
    }

    switch (commandId)
    {
    case IdButtonPluginRescan:
        pluginManagerState_.statusText = "Plugin database refreshed.";
        break;

    case IdButtonPluginAddStub:
        requestCreatePluginStub();
        break;

    case IdButtonPluginToggleSandbox:
        requestTogglePluginSandboxMode();
        break;

    case IdButtonPluginClose:
        closePluginManagerWindow();
        return;

    default:
        break;
    }

    refreshPluginManagerState();
    updatePluginManagerControls();
}

bool UI::handleKeyDown(WPARAM wParam, LPARAM)
{
    const bool altPressed = (GetKeyState(VK_MENU) & 0x8000) != 0;

    switch (wParam)
    {
    case VK_F5:
        togglePane(WorkspacePane::Playlist);
        break;

    case VK_F6:
        togglePane(WorkspacePane::ChannelRack);
        break;

    case VK_F7:
        togglePane(WorkspacePane::PianoRoll);
        break;

    case VK_F9:
        togglePane(WorkspacePane::Mixer);
        break;

    case VK_F8:
        if (altPressed)
        {
            togglePane(WorkspacePane::Browser);
        }
        else
        {
            return false;
        }
        break;

    case VK_SPACE:
        requestTransportPlay();
        break;

    case VK_OEM_PLUS:
    case VK_ADD:
        cycleZoom(workspace_.focusedPane == WorkspacePane::PianoRoll, 1);
        break;

    case VK_OEM_MINUS:
    case VK_SUBTRACT:
        cycleZoom(workspace_.focusedPane == WorkspacePane::PianoRoll, -1);
        break;

    default:
        return false;
    }

    layoutControls();
    refreshFromEngineSnapshot();
    return true;
}

void UI::showAboutDialog() const
{
    std::ostringstream oss;
    oss
        << "DAW Cloud Template\n\n"
        << "FL-style workspace layer implemented on top of the current engine.\n\n"
        << "Backend: " << visibleState_.backendName << "\n"
        << "Device: " << visibleState_.deviceName << "\n"
        << "Project: " << visibleState_.project.projectName << "\n"
        << "Tempo: " << static_cast<int>(workspace_.tempoBpm + 0.5) << " BPM\n"
        << "Pattern: " << workspace_.activePattern << "\n"
        << "Mode: " << (workspace_.songMode ? "Song" : "Pattern") << "\n"
        << "Automation: " << boolLabel(visibleState_.automationEnabled, "On", "Off") << "\n"
        << "PDC: " << boolLabel(visibleState_.pdcEnabled, "On", "Off") << "\n"
        << "Anticipative: " << boolLabel(visibleState_.anticipativeProcessingEnabled, "On", "Off") << "\n";

    MessageBoxA(hwnd_, oss.str().c_str(), "About", MB_OK | MB_ICONINFORMATION);
}

void UI::selectNextTrack()
{
    if (visibleState_.project.tracks.empty())
    {
        selectedTrackIndex_ = 0;
        return;
    }

    selectedTrackIndex_ = (selectedTrackIndex_ + 1) % visibleState_.project.tracks.size();
}

void UI::selectPreviousTrack()
{
    if (visibleState_.project.tracks.empty())
    {
        selectedTrackIndex_ = 0;
        return;
    }

    selectedTrackIndex_ =
        selectedTrackIndex_ == 0
            ? (visibleState_.project.tracks.size() - 1)
            : (selectedTrackIndex_ - 1);
}

void UI::selectNextPattern()
{
    const int patternCount = std::max(1, static_cast<int>(workspaceModel_.patterns.size()));
    workspace_.activePattern = (workspace_.activePattern % patternCount) + 1;
    syncWorkspaceModel();
}

void UI::selectPreviousPattern()
{
    const int patternCount = std::max(1, static_cast<int>(workspaceModel_.patterns.size()));
    workspace_.activePattern = workspace_.activePattern <= 1 ? patternCount : workspace_.activePattern - 1;
    syncWorkspaceModel();
}

void UI::cycleZoom(bool pianoRoll, int delta)
{
    std::size_t& index = pianoRoll ? workspace_.pianoZoomIndex : workspace_.playlistZoomIndex;
    const int newIndex = static_cast<int>(index) + delta;
    if (newIndex < 0)
    {
        index = 4;
    }
    else
    {
        index = static_cast<std::size_t>(newIndex) % 5;
    }
}

void UI::cycleBrowserTab(int delta)
{
    const int newIndex = static_cast<int>(workspace_.browserTabIndex) + delta;
    if (newIndex < 0)
    {
        workspace_.browserTabIndex = kBrowserTabCount - 1;
    }
    else
    {
        workspace_.browserTabIndex = static_cast<std::size_t>(newIndex) % kBrowserTabCount;
    }
}

void UI::cycleEditorTool(bool pianoRoll, int delta)
{
    if (!pianoRoll)
    {
        static constexpr EditorTool kPlaylistTools[] = {EditorTool::Draw, EditorTool::Slice};
        constexpr int kPlaylistToolCount = static_cast<int>(sizeof(kPlaylistTools) / sizeof(kPlaylistTools[0]));
        int currentIndex = 0;
        for (int index = 0; index < kPlaylistToolCount; ++index)
        {
            if (workspace_.playlistTool == kPlaylistTools[index])
            {
                currentIndex = index;
                break;
            }
        }

        currentIndex += delta;
        if (currentIndex < 0)
        {
            currentIndex = kPlaylistToolCount - 1;
        }
        workspace_.playlistTool = kPlaylistTools[currentIndex % kPlaylistToolCount];
        return;
    }

    EditorTool& tool = workspace_.pianoTool;
    int current = static_cast<int>(tool);
    current += delta;
    if (current < 0)
    {
        current = 5;
    }
    tool = static_cast<EditorTool>(current % 6);
}

void UI::cycleSnap(int delta)
{
    static constexpr const char* kSnapModes[] = {"Off", "Line", "Cell", "Beat", "Bar", "Step"};
    constexpr std::size_t modeCount = sizeof(kSnapModes) / sizeof(kSnapModes[0]);

    const int newIndex = static_cast<int>(workspace_.snapIndex) + delta;
    if (newIndex < 0)
    {
        workspace_.snapIndex = modeCount - 1;
    }
    else
    {
        workspace_.snapIndex = static_cast<std::size_t>(newIndex) % modeCount;
    }
}

void UI::togglePane(WorkspacePane pane)
{
    switch (pane)
    {
    case WorkspacePane::Browser:
        workspace_.browserVisible = !workspace_.browserVisible;
        workspace_.focusedPane = workspace_.browserVisible ? WorkspacePane::Browser : WorkspacePane::Playlist;
        break;

    case WorkspacePane::ChannelRack:
        workspace_.channelRackVisible = !workspace_.channelRackVisible;
        workspace_.focusedPane = workspace_.channelRackVisible ? WorkspacePane::ChannelRack : WorkspacePane::Playlist;
        break;

    case WorkspacePane::PianoRoll:
        workspace_.pianoRollVisible = !workspace_.pianoRollVisible;
        workspace_.focusedPane = workspace_.pianoRollVisible ? WorkspacePane::PianoRoll : WorkspacePane::Playlist;
        break;

    case WorkspacePane::Playlist:
        workspace_.playlistVisible = true;
        workspace_.focusedPane = WorkspacePane::Playlist;
        break;

    case WorkspacePane::Mixer:
        workspace_.mixerVisible = !workspace_.mixerVisible;
        workspace_.focusedPane = workspace_.mixerVisible ? WorkspacePane::Mixer : WorkspacePane::Playlist;
        break;

    case WorkspacePane::Plugin:
        workspace_.pluginVisible = !workspace_.pluginVisible;
        workspace_.focusedPane = workspace_.pluginVisible ? WorkspacePane::Plugin : WorkspacePane::Playlist;
        break;
    }
}

std::string UI::currentSnapLabel() const
{
    static constexpr const char* kSnapModes[] = {"Off", "Line", "Cell", "Beat", "Bar", "Step"};
    return kSnapModes[std::min<std::size_t>(workspace_.snapIndex, 5)];
}

std::string UI::currentBrowserTabLabel() const
{
    return kBrowserTabs[0];
}

std::string UI::currentZoomLabel(bool pianoRoll) const
{
    static constexpr const char* kZoomLevels[] = {"Far", "Normal", "Close", "Detail", "Micro"};
    const std::size_t index = pianoRoll ? workspace_.pianoZoomIndex : workspace_.playlistZoomIndex;
    return kZoomLevels[std::min<std::size_t>(index, 4)];
}

std::string UI::currentToolLabel(bool pianoRoll) const
{
    const EditorTool tool = pianoRoll ? workspace_.pianoTool : workspace_.playlistTool;
    switch (tool)
    {
    case EditorTool::Draw: return "Draw";
    case EditorTool::Slice: return "Slice";
    case EditorTool::Paint: return pianoRoll ? "Paint" : "Draw";
    case EditorTool::Select: return pianoRoll ? "Select" : "Draw";
    case EditorTool::DeleteTool: return pianoRoll ? "Delete" : "Draw";
    case EditorTool::Mute: return pianoRoll ? "Mute" : "Slice";
    default: return "Draw";
    }
}

std::string UI::buildBrowserPanelText() const
{
    std::ostringstream browser;
    browser << "Project\n\n";
    for (std::size_t index = 0; index < workspaceModel_.browserEntries.size(); ++index)
    {
        const BrowserEntry& entry = workspaceModel_.browserEntries[index];
        browser << std::string(static_cast<std::size_t>(std::max(0, entry.indentLevel) * 2), ' ');
        browser
            << (entry.group ? (entry.expanded ? "v " : "> ") : (static_cast<int>(index) == workspaceModel_.selectedBrowserIndex ? "> " : "- "))
            << entry.label;
        if (entry.favorite)
        {
            browser << " | Active";
        }
        browser << "\n"
                << std::string(static_cast<std::size_t>(std::max(0, entry.indentLevel) * 2 + 4), ' ')
                << entry.category << " | " << entry.subtitle << "\n";
    }

    browser << "\nPatterns and audio clips stay synced with the current project.";
    return browser.str();
}

std::string UI::buildChannelRackPanelText() const
{
    std::ostringstream rack;
    rack << "Channel Rack\n\n";
    if (workspaceModel_.patternLanes.empty())
    {
        rack << "No channels yet.\nUse Add Track to create instruments, samplers or automation channels.";
        return rack.str();
    }

    for (std::size_t index = 0; index < workspaceModel_.patternLanes.size(); ++index)
    {
        const PatternLaneState& lane = workspaceModel_.patternLanes[index];
        int enabledSteps = 0;
        for (const auto& step : lane.steps)
        {
            enabledSteps += step.enabled ? 1 : 0;
        }
        rack
            << (index == selectedTrackIndex_ ? "> " : "  ")
            << lane.name
            << " | Steps " << enabledSteps << '/' << lane.steps.size()
            << " | Swing " << lane.swing
            << " | Shuffle " << lane.shuffle
            << " | Notes " << lane.notes.size()
            << "\n";
    }

    rack << "\nCada fila ya modela un canal con secuencia, notas y groove por patrón.";
    return rack.str();
}

std::string UI::buildStepSequencerPanelText() const
{
    if (workspaceModel_.patternLanes.empty())
    {
        return "Step Sequencer\n\nNo active channels.";
    }

    const int laneIndex = clampValue(workspaceModel_.activeChannelIndex, 0, static_cast<int>(workspaceModel_.patternLanes.size() - 1));
    const PatternLaneState& lane = workspaceModel_.patternLanes[static_cast<std::size_t>(laneIndex)];

    std::ostringstream steps;
    steps
        << "Step Sequencer\n\n"
        << lane.name << "\n"
        << "Pattern " << workspace_.activePattern
        << " | Snap " << currentSnapLabel()
        << " | Swing " << lane.swing
        << " | Shuffle " << lane.shuffle
        << "\n\n";

    for (std::size_t step = 0; step < lane.steps.size(); ++step)
    {
        const ChannelStepState& cell = lane.steps[step];
        steps << (cell.enabled ? 'x' : '.');
        if ((step + 1) % 4 == 0)
        {
            steps << ' ';
        }
    }
    steps
        << "\nVelocity lane: ";
    for (std::size_t step = 0; step < lane.steps.size(); ++step)
    {
        steps << (lane.steps[step].enabled ? std::to_string(lane.steps[step].velocity / 10) : "-");
        if ((step + 1) % 4 == 0)
        {
            steps << ' ';
        }
    }
    steps << "\nClick cells here to toggle the pattern like FL step programming.";
    return steps.str();
}

std::string UI::buildPianoRollPanelText() const
{
    if (workspaceModel_.patternLanes.empty())
    {
        return "Piano Roll\n\nNo pattern lane selected.";
    }

    const int laneIndex = clampValue(workspaceModel_.activeChannelIndex, 0, static_cast<int>(workspaceModel_.patternLanes.size() - 1));
    const PatternLaneState& lane = workspaceModel_.patternLanes[static_cast<std::size_t>(laneIndex)];
    std::ostringstream piano;
    piano
        << "Piano Roll\n\n"
        << "Target Channel: " << lane.name
        << "\nPattern: " << workspace_.activePattern
        << " | Snap: " << currentSnapLabel()
        << " | Zoom: " << currentZoomLabel(true)
        << " | Tool: " << currentToolLabel(true)
        << "\nGhost notes: visible | Helpers: on | Velocity lane: visible | Notes: " << lane.notes.size()
        << "\nRange: " << noteLabelFromLane(0) << " .. " << noteLabelFromLane(kPianoLaneCount - 1)
        << "\n\n";

    for (const auto& note : lane.notes)
    {
        piano
            << noteLabelFromLane(note.lane)
            << " @ " << note.step
            << " len " << note.length
            << " vel " << note.velocity
            << (note.slide ? " | slide" : "")
            << (note.accent ? " | accent" : "")
            << "\n";
    }

    piano << "\nDrag notes to reshape melodies and accents inside the active pattern.";
    return piano.str();
}

std::string UI::buildPlaylistPanelText() const
{
    std::ostringstream playlist;
    playlist
        << "Playlist\n\n"
        << "Mode: " << (workspace_.songMode ? "Song arrangement" : "Pattern preview")
        << " | Pattern: " << workspace_.activePattern
        << " | Zoom: " << currentZoomLabel(false)
        << " | Tool: " << currentToolLabel(false)
        << " | Patterns loaded: " << workspaceModel_.patterns.size()
        << "\nTimeline: 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8\n\n";

    if (workspaceModel_.playlistBlocks.empty())
    {
        playlist << "Track 1  [empty lane]\n";
    }
    else
    {
        const int laneCount = std::max(1, static_cast<int>(visibleState_.project.tracks.size()));
        for (int lane = 0; lane < laneCount; ++lane)
        {
            const std::string laneName =
                lane < static_cast<int>(visibleState_.project.tracks.size())
                    ? visibleState_.project.tracks[static_cast<std::size_t>(lane)].name
                    : ("Track " + std::to_string(lane + 1));
            playlist << "Track " << (lane + 1) << " " << laneName << " : ";

            bool any = false;
            for (const auto& block : workspaceModel_.playlistBlocks)
            {
                if (block.lane == lane)
                {
                    playlist << "[" << block.label << " @" << block.startCell << " len " << block.lengthCells << " " << block.clipType;
                    if (block.patternNumber > 0)
                    {
                        playlist << " P" << block.patternNumber;
                    }
                    playlist << "] ";
                    any = true;
                }
            }
            if (!any)
            {
                playlist << "[empty]";
            }
            playlist << "\n";
        }
    }

    playlist
        << "\nMarkers: ";
    for (const auto& marker : workspaceModel_.markers)
    {
        playlist << "[" << marker.name << " @" << marker.timelineCell << "] ";
    }
    playlist
        << "\nPattern clips and audio clips share the same arranger."
        << "\nThis is the arrangement view targeted by F5.";
    return playlist.str();
}

std::string UI::buildMixerPanelText() const
{
    std::ostringstream mixer;
    mixer
        << "Mixer\n\n"
        << "Master | CPU " << static_cast<int>(visibleState_.cpuLoadApprox * 100.0 + 0.5)
        << "% | Peak " << visibleState_.peakBlockTimeUs
        << " us | Latency " << visibleState_.currentLatencySamples << " smp\n\n";

    for (const auto& strip : workspaceModel_.mixerStrips)
    {
        mixer
            << (strip.name == "Master" ? "Master" : ("Insert " + std::to_string(strip.insertSlot)))
            << " | " << strip.name
            << " | Fader " << strip.volumeDb << " dB"
            << " | Pan " << (strip.pan == 0.0 ? "C" : (strip.pan < 0.0 ? "L" : "R"))
            << " | Meter " << strip.peakLevel << "%"
            << " | Route " << (strip.routeTarget < 0 ? std::string("Master") : ("Ins " + std::to_string(strip.routeTarget + 1)))
            << " | Send " << static_cast<int>(strip.sendAmount * 100.0 + 0.5) << "% | FX ";
        for (std::size_t fx = 0; fx < strip.effects.size(); ++fx)
        {
            mixer << strip.effects[fx];
            if (fx + 1 < strip.effects.size())
            {
                mixer << '/';
            }
        }
        mixer << "\n";
    }

    mixer << "\nRouting, sends, insert FX and metering are represented in this strip rack.";
    return mixer.str();
}

std::string UI::buildPluginPanelText() const
{
    const ChannelSettingsState* channel = nullptr;
    if (!workspaceModel_.channelSettings.empty())
    {
        const int channelIndex = clampValue(workspaceModel_.activeChannelIndex, 0, static_cast<int>(workspaceModel_.channelSettings.size() - 1));
        channel = &workspaceModel_.channelSettings[static_cast<std::size_t>(channelIndex)];
    }

    std::ostringstream plugin;
    plugin
        << "Plugin / Channel Settings\n\n"
        << "Target: " << (channel == nullptr ? (visibleState_.selection.selectedTrackName.empty() ? "<none>" : visibleState_.selection.selectedTrackName) : channel->name) << "\n"
        << "Wrapper: " << boolLabel(visibleState_.pluginHostEnabled, "Plugin host enabled", "In-process") << "\n"
        << "Sandbox: " << boolLabel(visibleState_.pluginSandboxEnabled, "On", "Off") << "\n"
        << "64-bit path: " << boolLabel(visibleState_.prefer64BitMix, "On", "Off") << "\n"
        << "Automation: " << boolLabel(visibleState_.automationEnabled, "Sample-accurate", "Off") << "\n"
        << "PDC: " << boolLabel(visibleState_.pdcEnabled, "On", "Off") << "\n\n"
        << "Sampler / Synth Controls\n"
        << "  - Gain " << (channel == nullptr ? 0.80 : channel->gain) << "\n"
        << "  - Pan " << (channel == nullptr ? 0.00 : channel->pan) << "\n"
        << "  - Pitch " << (channel == nullptr ? 0.0 : channel->pitchSemitones) << " st\n"
        << "  - Attack " << (channel == nullptr ? 12.0 : channel->attackMs) << " ms\n"
        << "  - Release " << (channel == nullptr ? 180.0 : channel->releaseMs) << " ms\n"
        << "  - Filter cutoff " << (channel == nullptr ? 8400.0 : channel->filterCutoffHz) << " Hz\n"
        << "  - Resonance " << (channel == nullptr ? 0.20 : channel->resonance) << "\n"
        << "  - Mixer insert " << (channel == nullptr ? 1 : channel->mixerInsert) << "\n"
        << "  - Route target " << (channel == nullptr ? 0 : channel->routeTarget) << "\n"
        << "  - Reverse " << boolLabel(channel != nullptr && channel->reverse, "On", "Off") << "\n"
        << "  - Stretch " << boolLabel(channel == nullptr || channel->timeStretch, "On", "Off") << "\n\n";

    if (channel != nullptr)
    {
        plugin << "Plugin Rack\n";
        for (const auto& entry : channel->pluginRack)
        {
            plugin << "  - " << entry << "\n";
        }
        plugin << "\nPresets\n";
        for (const auto& preset : channel->presets)
        {
            plugin << "  - " << preset << "\n";
        }
    }

    plugin << "\nThis pane now represents persistent channel settings, plugin rack and routing state.";
    return plugin.str();
}

std::string UI::buildPluginManagerDetailText() const
{
    std::ostringstream detail;
    detail << "Plugin Details\n\n";

    if (pluginManagerState_.filteredPlugins.empty())
    {
        detail << "No plugins match the current filter.\n";
    }
    else
    {
        const auto& plugin =
            pluginManagerState_.filteredPlugins[std::min(pluginManagerState_.selectedIndex, pluginManagerState_.filteredPlugins.size() - 1)];
        detail
            << "Name: " << plugin.name << "\n"
            << "Vendor: " << plugin.vendor << "\n"
            << "Format: " << plugin.format << "\n"
            << "Hosting: " << plugin.processModeLabel << "\n"
            << "Latency: " << plugin.latencySamples << " samples\n"
            << "Double precision: " << boolLabel(plugin.supportsDoublePrecision, "Yes", "No") << "\n"
            << "Sample accurate automation: " << boolLabel(plugin.supportsSampleAccurateAutomation, "Yes", "No") << "\n"
            << "Plugin host enabled: " << boolLabel(visibleState_.pluginHostEnabled, "Yes", "No") << "\n"
            << "Sandbox enabled: " << boolLabel(visibleState_.pluginSandboxEnabled, "Yes", "No") << "\n";
    }

    if (!pluginManagerState_.statusText.empty())
    {
        detail << "\nStatus: " << pluginManagerState_.statusText;
    }

    return detail.str();
}

std::string UI::buildPluginManagerPathText() const
{
    std::ostringstream paths;
    paths << "Search Paths / Favorites / Blacklist\n\nPaths\n";
    for (const auto& path : pluginManagerState_.searchPaths)
    {
        paths << "  - " << path << "\n";
    }

    paths << "\nFavorites\n";
    for (const auto& favorite : pluginManagerState_.favorites)
    {
        paths << "  - " << favorite << "\n";
    }

    paths << "\nBlacklist\n";
    for (const auto& blocked : pluginManagerState_.blacklist)
    {
        paths << "  - " << blocked << "\n";
    }

    paths << "\nThis window models plugin scan paths, database state, wrapper options and sandbox decisions.";
    return paths.str();
}

void UI::applyPluginSearchFilter()
{
    pluginManagerState_.filteredPlugins.clear();

    std::string needle = pluginManagerState_.searchText;
    std::transform(needle.begin(), needle.end(), needle.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    for (const auto& plugin : pluginManagerState_.loadedPlugins)
    {
        std::string haystack = plugin.name + " " + plugin.vendor + " " + plugin.format;
        std::transform(haystack.begin(), haystack.end(), haystack.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (needle.empty() || haystack.find(needle) != std::string::npos)
        {
            pluginManagerState_.filteredPlugins.push_back(plugin);
        }
    }

    if (pluginManagerState_.selectedIndex >= pluginManagerState_.filteredPlugins.size())
    {
        pluginManagerState_.selectedIndex = 0;
    }
}

std::string UI::buildPluginManagerTableText() const
{
    std::ostringstream table;
    table << "Database View\r\n";
    table << "Name                Vendor          Fmt   Host           Lat\r\n";
    table << "-------------------------------------------------------------\r\n";

    if (pluginManagerState_.filteredPlugins.empty())
    {
        table << "<no plugins>\r\n";
        return table.str();
    }

    for (const auto& plugin : pluginManagerState_.filteredPlugins)
    {
        std::string name = plugin.name.substr(0, std::min<std::size_t>(18, plugin.name.size()));
        std::string vendor = plugin.vendor.substr(0, std::min<std::size_t>(14, plugin.vendor.size()));
        std::string fmt = plugin.format.substr(0, std::min<std::size_t>(5, plugin.format.size()));
        std::string host = plugin.processModeLabel.substr(0, std::min<std::size_t>(13, plugin.processModeLabel.size()));

        table
            << name << std::string(20 - name.size(), ' ')
            << vendor << std::string(16 - vendor.size(), ' ')
            << fmt << std::string(6 - fmt.size(), ' ')
            << host << std::string(15 - host.size(), ' ')
            << plugin.latencySamples << "\r\n";
    }

    return table.str();
}

bool UI::isPaneVisible(WorkspacePane pane) const
{
    switch (pane)
    {
    case WorkspacePane::Browser: return workspace_.browserVisible;
    case WorkspacePane::ChannelRack: return workspace_.channelRackVisible;
    case WorkspacePane::PianoRoll: return workspace_.pianoRollVisible;
    case WorkspacePane::Playlist: return workspace_.playlistVisible;
    case WorkspacePane::Mixer: return workspace_.mixerVisible;
    case WorkspacePane::Plugin: return workspace_.pluginVisible;
    default: return false;
    }
}

void UI::setPaneVisible(WorkspacePane pane, bool visible)
{
    switch (pane)
    {
    case WorkspacePane::Browser: workspace_.browserVisible = visible; break;
    case WorkspacePane::ChannelRack: workspace_.channelRackVisible = visible; break;
    case WorkspacePane::PianoRoll: workspace_.pianoRollVisible = visible; break;
    case WorkspacePane::Playlist: workspace_.playlistVisible = visible; break;
    case WorkspacePane::Mixer: workspace_.mixerVisible = visible; break;
    case WorkspacePane::Plugin: workspace_.pluginVisible = visible; break;
    default: break;
    }
}

UI::DockedPaneState* UI::findDockedPane(WorkspacePane pane)
{
    for (auto& entry : dockedPanes_)
    {
        if (entry.pane == pane)
        {
            return &entry;
        }
    }
    return nullptr;
}

UI::DockedPaneState* UI::findDockedPaneWindow(HWND hwnd)
{
    for (auto& entry : dockedPanes_)
    {
        if (entry.windowHandle == hwnd)
        {
            return &entry;
        }
    }
    return nullptr;
}

const UI::DockedPaneState* UI::findDockedPane(WorkspacePane pane) const
{
    for (const auto& entry : dockedPanes_)
    {
        if (entry.pane == pane)
        {
            return &entry;
        }
    }
    return nullptr;
}

const UI::DockedPaneState* UI::findDockedPaneWindow(HWND hwnd) const
{
    for (const auto& entry : dockedPanes_)
    {
        if (entry.windowHandle == hwnd)
        {
            return &entry;
        }
    }
    return nullptr;
}

void UI::detachPane(WorkspacePane pane)
{
    if (DockedPaneState* entry = findDockedPane(pane))
    {
        entry->detached = true;
    }
}

void UI::attachPane(WorkspacePane pane)
{
    if (DockedPaneState* entry = findDockedPane(pane))
    {
        entry->detached = false;
    }
}

std::string UI::paneTitle(WorkspacePane pane) const
{
    switch (pane)
    {
    case WorkspacePane::Browser: return "Browser";
    case WorkspacePane::ChannelRack: return "Channel Rack - Pattern " + std::to_string(workspace_.activePattern);
    case WorkspacePane::PianoRoll:
        return "Piano Roll - P" + std::to_string(workspace_.activePattern) +
            " - " + (visibleState_.selection.selectedTrackName.empty() ? std::string("No Channel") : visibleState_.selection.selectedTrackName);
    case WorkspacePane::Playlist: return "Playlist";
    case WorkspacePane::Mixer: return "Mixer - " + (visibleState_.project.projectName.empty() ? std::string("Untitled Project") : visibleState_.project.projectName);
    case WorkspacePane::Plugin: return "Plugin";
    default: return "Pane";
    }
}

UI::SurfaceKind UI::kindFromSurfaceHandle(HWND hwnd) const
{
    if (hwnd == browserPanel_) return SurfaceKind::Browser;
    if (hwnd == channelRackPanel_) return SurfaceKind::ChannelRack;
    if (hwnd == stepSequencerPanel_) return SurfaceKind::StepSequencer;
    if (hwnd == pianoRollPanel_) return SurfaceKind::PianoRoll;
    if (hwnd == playlistPanel_) return SurfaceKind::Playlist;
    if (hwnd == mixerPanel_) return SurfaceKind::Mixer;
    if (hwnd == pluginPanel_) return SurfaceKind::Plugin;
    return SurfaceKind::None;
}

void UI::paintSurface(HWND hwnd, SurfaceKind kind)
{
    PAINTSTRUCT paintStruct{};
    HDC dc = BeginPaint(hwnd, &paintStruct);

    RECT rect{};
    GetClientRect(hwnd, &rect);

    const COLORREF backgroundColor =
        kind == SurfaceKind::Browser ? blendColor(kUiGraphite, kUiShadow, 1, 5) :
        kind == SurfaceKind::Playlist ? blendColor(kUiPetrol, kUiShadow, 1, 5) :
        blendColor(kUiPetrol, kUiShadow, 1, 4);
    fillRectColor(dc, rect, backgroundColor);

    SetBkMode(dc, TRANSPARENT);

    switch (kind)
    {
    case SurfaceKind::Browser: paintBrowserSurface(dc, rect); break;
    case SurfaceKind::ChannelRack: paintChannelRackSurface(dc, rect); break;
    case SurfaceKind::StepSequencer: paintStepSequencerSurface(dc, rect); break;
    case SurfaceKind::PianoRoll: paintPianoRollSurface(dc, rect); break;
    case SurfaceKind::Playlist: paintPlaylistSurface(dc, rect); break;
    case SurfaceKind::Mixer: paintMixerSurface(dc, rect); break;
    case SurfaceKind::Plugin: paintPluginSurface(dc, rect); break;
    case SurfaceKind::None:
    default:
        break;
    }

    EndPaint(hwnd, &paintStruct);
}

void UI::paintBrowserSurface(HDC dc, const RECT& rect)
{
    const int contentTop = rect.top + 2;
    const int visibleBottom = rect.bottom - 2;

    RECT contentRect{rect.left + 1, rect.top + 1, rect.right - 1, rect.bottom - 1};
    fillRectColor(dc, contentRect, blendColor(kUiGraphite, kUiShadow, 1, 5));

    int y = contentTop - browserScrollY_;
    for (std::size_t index = 0; index < workspaceModel_.browserEntries.size(); ++index)
    {
        const BrowserEntry& entry = workspaceModel_.browserEntries[index];
        RECT itemRect{rect.left + 6, y, rect.right - 6, y + kBrowserRowHeight};
        y += kBrowserRowHeight;

        if (itemRect.bottom < contentTop)
        {
            continue;
        }
        if (itemRect.top > visibleBottom)
        {
            break;
        }

        const bool selected = static_cast<int>(index) == workspaceModel_.selectedBrowserIndex;
        fillRectColor(
            dc,
            itemRect,
            selected
                ? blendColor(kUiPetrol, kUiAnthracite, 1, 2)
                : (entry.group ? blendColor(kUiAnthracite, kUiShadow, 1, 5) : blendColor(kUiGraphite, kUiShadow, 1, 7)));

        if (selected)
        {
            RECT accentRect{itemRect.left, itemRect.top + 2, itemRect.left + 3, itemRect.bottom - 2};
            fillRectColor(dc, accentRect, kUiRedAccent);
        }

        drawHorizontalLine(dc, itemRect.left, itemRect.right, itemRect.bottom - 1, kUiLineSoft);

        const int treeX = itemRect.left + 8 + (entry.indentLevel * kBrowserIndentWidth);
        const int centerY = itemRect.top + (kBrowserRowHeight / 2);
        if (entry.group)
        {
            drawCollapseTriangle(dc, treeX, centerY - 4, entry.expanded, selected ? kUiText : kUiTextSoft);
        }
        else if (entry.folder)
        {
            RECT folderRect{treeX, centerY - 4, treeX + 10, centerY + 4};
            fillRectColor(dc, folderRect, RGB(188, 150, 81));
            drawSurfaceFrame(dc, folderRect, RGB(120, 91, 44));
        }
        else
        {
            RECT iconRect{treeX, centerY - 3, treeX + 6, centerY + 3};
            fillRectColor(dc, iconRect, entry.favorite ? kUiLime : blendColor(kUiLine, kUiTextDim, 1, 2));
        }

        RECT labelRect{treeX + 16, itemRect.top + 3, itemRect.right - 74, itemRect.top + 16};
        RECT subtitleRect{treeX + 16, itemRect.top + 14, itemRect.right - 74, itemRect.bottom - 3};
        RECT categoryRect{itemRect.right - 70, itemRect.top + 3, itemRect.right - 10, itemRect.top + 15};

        SetTextColor(dc, selected ? kUiText : (entry.group ? kUiTextSoft : kUiText));
        DrawTextA(dc, entry.label.c_str(), -1, &labelRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        SetTextColor(dc, kUiTextDim);
        DrawTextA(dc, entry.subtitle.c_str(), -1, &subtitleRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        SetTextColor(dc, entry.favorite ? kUiLimeDim : kUiTextDim);
        DrawTextA(dc, entry.category.c_str(), -1, &categoryRect, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    drawSurfaceFrame(dc, rect, kUiLine);
}

void UI::paintChannelRackSurface(HDC dc, const RECT& rect)
{
    drawSurfaceHeader(dc, rect, "Channel Rack", "Detached window");
    const int rowHeight = 40;
    const int contentTop = rect.top + kSurfaceHeaderHeight + 8;
    const int nameWidth = 180;
    const int cellSize = 14;

    int y = contentTop;
    for (std::size_t index = 0; index < workspaceModel_.patternLanes.size(); ++index)
    {
        if (y + rowHeight > rect.bottom - 8)
        {
            break;
        }

        const PatternLaneState& lane = workspaceModel_.patternLanes[index];
        RECT rowRect{rect.left + 8, y, rect.right - 8, y + rowHeight};
        const bool selected = index == selectedTrackIndex_;
        HBRUSH rowBrush = CreateSolidBrush(selected ? RGB(73, 82, 68) : RGB(39, 44, 49));
        FillRect(dc, &rowRect, rowBrush);
        DeleteObject(rowBrush);

        RECT nameRect{rowRect.left + 12, rowRect.top + 4, rowRect.left + nameWidth, rowRect.top + 22};
        SetTextColor(dc, selected ? RGB(247, 247, 247) : RGB(224, 228, 232));
        DrawTextA(dc, lane.name.c_str(), -1, &nameRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        RECT metaRect{rowRect.left + 12, rowRect.top + 21, rowRect.left + nameWidth, rowRect.bottom - 6};
        const std::string metaText =
            "Pattern " + std::to_string(workspace_.activePattern) + " | Notes " + std::to_string(lane.notes.size());
        SetTextColor(dc, RGB(168, 179, 189));
        DrawTextA(dc, metaText.c_str(), -1, &metaRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        for (int step = 0; step < kStepCount; ++step)
        {
            const int cellX = rowRect.left + nameWidth + 26 + step * (cellSize + 4);
            RECT cellRect{cellX, rowRect.top + 9, cellX + cellSize, rowRect.top + 23};
            const bool enabled = step < static_cast<int>(lane.steps.size()) && lane.steps[static_cast<std::size_t>(step)].enabled;
            HBRUSH cellBrush = CreateSolidBrush(enabled ? RGB(235, 155, 49) : RGB(63, 70, 79));
            FillRect(dc, &cellRect, cellBrush);
            DeleteObject(cellBrush);
        }

        y += rowHeight + 6;
    }

    drawSurfaceFrame(dc, rect, RGB(90, 98, 72));
}

void UI::paintStepSequencerSurface(HDC dc, const RECT& rect)
{
    drawSurfaceHeader(dc, rect, "Step Sequencer", "Detached rack grid");
    drawSurfaceFrame(dc, rect, RGB(108, 88, 42));

    const int top = rect.top + 34;
    const int left = rect.left + 14;
    const int availableWidth = static_cast<int>(rect.right) - left - 14;
    const int cellSize = std::max<int>(16, availableWidth / kStepCount);
    const int laneCount = std::min(4, static_cast<int>(workspaceModel_.patternLanes.size()));

    for (int laneIndex = 0; laneIndex < laneCount; ++laneIndex)
    {
        const PatternLaneState& lane = workspaceModel_.patternLanes[static_cast<std::size_t>(laneIndex)];
        const int y = top + laneIndex * 24;
        RECT labelRect{left, y, left + 90, y + 16};
        SetTextColor(dc, RGB(238, 238, 238));
        DrawTextA(dc, lane.name.c_str(), -1, &labelRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        for (int step = 0; step < kStepCount; ++step)
        {
            RECT cellRect{left + 96 + step * cellSize, y, left + 96 + (step + 1) * cellSize - 4, y + 16};
            const bool enabled = step < static_cast<int>(lane.steps.size()) && lane.steps[static_cast<std::size_t>(step)].enabled;
            HBRUSH brush = CreateSolidBrush(enabled ? RGB(235, 155, 49) : RGB(57, 62, 70));
            FillRect(dc, &cellRect, brush);
            DeleteObject(brush);
        }
    }

    RECT helpRect{rect.left + 14, rect.bottom - 28, rect.right - 14, rect.bottom - 8};
    SetTextColor(dc, RGB(212, 214, 218));
    DrawTextA(dc, "Click cells to toggle steps for the selected channel.", -1, &helpRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
}

void UI::paintPianoRollSurface(HDC dc, const RECT& rect)
{
    const std::string channelName =
        visibleState_.selection.selectedTrackName.empty()
            ? std::string("No channel selected")
            : visibleState_.selection.selectedTrackName;
    const std::string subtitle =
        "Pattern " + std::to_string(workspace_.activePattern) +
        " | " + channelName +
        " | Zoom " + currentZoomLabel(true) +
        " | Tool " + currentToolLabel(true);
    drawSurfaceHeader(dc, rect, "Piano Roll", subtitle);

    RECT drawRect = rect;
    drawRect.top += 24;
    const int keyWidth = 72;
    const int laneHeight = 22;
    const int columns = kPlaylistCellCount;
    const int gridLeft = drawRect.left + keyWidth;
    const int gridTop = drawRect.top;
    const int gridWidth = static_cast<int>(drawRect.right) - gridLeft;
    const int stepWidth = std::max<int>(18, gridWidth / columns);
    const int footerHeight = 28;
    RECT pianoGridRect{gridLeft, gridTop, drawRect.right, drawRect.bottom - footerHeight};
    const int laneCount = std::max<int>(1, (pianoGridRect.bottom - gridTop) / laneHeight);
    fillRectColor(dc, pianoGridRect, RGB(30, 36, 44));

    static HFONT keyFont = CreateFontA(
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
    HGDIOBJ oldFont = SelectObject(dc, keyFont);

    for (int lane = 0; lane < laneCount; ++lane)
    {
        const int noteLane = (kPianoLaneCount - 1) - lane;
        const int midiNote = 36 + std::max(0, noteLane);
        const int noteClass = midiNote % 12;
        const bool blackKey =
            noteClass == 1 || noteClass == 3 || noteClass == 6 || noteClass == 8 || noteClass == 10;
        const bool isC = noteClass == 0;
        const int y = gridTop + (lane * laneHeight);

        RECT keyRect{drawRect.left, y, gridLeft, y + laneHeight};
        RECT laneRect{gridLeft, y, drawRect.right, y + laneHeight};
        fillRectColor(dc, keyRect, blackKey ? RGB(34, 39, 46) : RGB(222, 226, 229));
        fillRectColor(
            dc,
            laneRect,
            blackKey
                ? RGB(36, 42, 51)
                : (isC ? RGB(44, 51, 62) : RGB(40, 46, 56)));

        if (isC)
        {
            RECT cAccent{drawRect.left, y, drawRect.left + 4, y + laneHeight};
            fillRectColor(dc, cAccent, RGB(238, 164, 79));
        }

        SetTextColor(dc, blackKey ? RGB(210, 215, 219) : RGB(56, 63, 72));
        RECT labelRect{drawRect.left + 10, y, gridLeft - 8, y + laneHeight};
        DrawTextA(dc, noteLabelFromLane(noteLane).c_str(), -1, &labelRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        drawHorizontalLine(dc, drawRect.left, drawRect.right, y, RGB(66, 74, 86));
    }

    for (int col = 0; col <= columns; ++col)
    {
        const int x = gridLeft + (col * stepWidth);
        const bool isBarLine = col % 4 == 0;
        drawVerticalLine(dc, x, gridTop, pianoGridRect.bottom, isBarLine ? RGB(90, 106, 122) : RGB(56, 66, 78));

        if (col < columns)
        {
            RECT topCellRect{x, gridTop, x + stepWidth, gridTop + 18};
            SetTextColor(dc, isBarLine ? RGB(210, 215, 220) : RGB(150, 158, 167));
            DrawTextA(dc, std::to_string(col + 1).c_str(), -1, &topCellRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
    }

    RECT dividerRect{gridLeft - 1, gridTop, gridLeft + 1, pianoGridRect.bottom};
    fillRectColor(dc, dividerRect, RGB(92, 101, 111));

    rebuildPianoVisuals(rect);

    HBRUSH noteBrush = CreateSolidBrush(RGB(232, 143, 36));
    HBRUSH selectedBrush = CreateSolidBrush(RGB(255, 212, 112));
    for (std::size_t index = 0; index < interactionState_.pianoNoteVisuals.size(); ++index)
    {
        const auto& note = interactionState_.pianoNoteVisuals[index];
        RECT noteRect{note.rect.x, note.rect.y, note.rect.x + note.rect.width, note.rect.y + note.rect.height};
        FillRect(dc, &noteRect, note.selected ? selectedBrush : noteBrush);
        drawSurfaceFrame(dc, noteRect, note.selected ? RGB(255, 240, 170) : RGB(188, 106, 26));
        RECT velocityGlow{noteRect.left + 2, noteRect.top + 2, noteRect.right - 2, noteRect.top + 5};
        fillRectColor(dc, velocityGlow, note.selected ? RGB(255, 236, 179) : RGB(255, 192, 120));
        RECT handleRect{noteRect.right - 5, noteRect.top, noteRect.right, noteRect.bottom};
        HBRUSH handleBrush = CreateSolidBrush(RGB(255, 235, 160));
        FillRect(dc, &handleRect, handleBrush);
        DeleteObject(handleBrush);
    }
    DeleteObject(noteBrush);
    DeleteObject(selectedBrush);

    if (interactionState_.marqueeActive && interactionState_.activeSurface == SurfaceKind::PianoRoll)
    {
        HPEN marqueePen = CreatePen(PS_DOT, 1, RGB(255, 255, 255));
        HGDIOBJ oldPen = SelectObject(dc, marqueePen);
        HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
        Rectangle(dc, interactionState_.marqueeRect.left, interactionState_.marqueeRect.top, interactionState_.marqueeRect.right, interactionState_.marqueeRect.bottom);
        SelectObject(dc, oldBrush);
        SelectObject(dc, oldPen);
        DeleteObject(marqueePen);
    }

    RECT footerRect{drawRect.left, drawRect.bottom - footerHeight, drawRect.right, drawRect.bottom};
    fillRectColor(dc, footerRect, RGB(26, 31, 38));
    drawHorizontalLine(dc, footerRect.left, footerRect.right, footerRect.top, RGB(64, 74, 86));
    SetTextColor(dc, RGB(214, 218, 222));
    RECT footerTextRect{footerRect.left + 10, footerRect.top, footerRect.right - 10, footerRect.bottom};
    const std::string footerText =
        "Click to add notes, drag to move, drag the right edge to resize. Notes are now stored in the project and drive generated pattern clips.";
    DrawTextA(dc, footerText.c_str(), -1, &footerTextRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    SelectObject(dc, oldFont);
    drawSurfaceFrame(dc, rect, RGB(88, 100, 118));
}

void UI::paintPlaylistSurface(HDC dc, const RECT& rect)
{
    const std::string subtitle =
        std::string(workspace_.songMode ? "Song arrangement" : "Pattern preview") +
        " | Zoom " + currentZoomLabel(false) +
        " | Tool " + currentToolLabel(false);
    drawSurfaceHeader(dc, rect, "Playlist - Arrangement", subtitle);

    const int laneHeight = kPlaylistLaneHeight;
    const int timelineTop = rect.top + kSurfaceHeaderHeight;
    const int timelineHeight = kPlaylistTimelineHeight;
    const int gridTop = timelineTop + timelineHeight;
    const int leftInset = kPlaylistTrackHeaderWidth;
    const int visibleGridWidth = std::max<int>(1, static_cast<int>(rect.right - rect.left) - leftInset);
    const int visibleGridHeight = std::max<int>(1, static_cast<int>(rect.bottom - gridTop));
    const int totalCells = playlistTotalCellCount();
    const int columnWidth = playlistColumnWidth(visibleGridWidth);
    const int trackCount = playlistTrackLaneCount();
    const int laneCount =
        std::max(
            trackCount + 8,
            std::max(
                kPlaylistMinVisibleTracks,
                (playlistScrollY_ + visibleGridHeight + laneHeight - 1) / laneHeight + 1));
    const int firstVisibleCell = std::max(0, playlistScrollX_ / std::max(1, columnWidth));
    const int visibleCellCount = (visibleGridWidth / std::max(1, columnWidth)) + 3;
    const int lastVisibleCell = std::min(totalCells, firstVisibleCell + visibleCellCount);
    const int firstVisibleLane = std::max(0, playlistScrollY_ / laneHeight);
    const int visibleLaneCount = (visibleGridHeight / laneHeight) + 3;
    const int lastVisibleLane = std::min(laneCount, firstVisibleLane + visibleLaneCount);

    RECT trackHeaderRect{rect.left, gridTop, rect.left + leftInset, rect.bottom};
    fillRectColor(dc, trackHeaderRect, blendColor(kUiAnthracite, kUiShadow, 1, 5));

    RECT timelineRect{rect.left + leftInset, timelineTop, rect.right, gridTop};
    fillRectColor(dc, timelineRect, blendColor(kUiPetrol, kUiAnthracite, 1, 3));

    RECT gutterRect{rect.left, timelineTop, rect.left + leftInset, gridTop};
    fillRectColor(dc, gutterRect, kUiAnthracite);

    RECT gridRect{rect.left + leftInset, gridTop, rect.right, rect.bottom};
    fillRectColor(dc, gridRect, blendColor(kUiPetrol, kUiShadow, 1, 5));

    for (int lane = firstVisibleLane; lane < lastVisibleLane; ++lane)
    {
        const int y = gridTop + (lane * laneHeight) - playlistScrollY_;
        const bool isTrackLane = lane < trackCount;
        const bool laneSelected = isTrackLane && static_cast<std::size_t>(lane) == selectedTrackIndex_;
        const std::string laneName =
            isTrackLane
                ? visibleState_.project.tracks[static_cast<std::size_t>(lane)].name
                : ("Track " + std::to_string(lane + 1));
        const std::string laneMeta = "Track " + std::to_string(lane + 1);

        RECT laneRect{rect.left + leftInset, y, rect.right, y + laneHeight};
        RECT headerLaneRect{rect.left, y, rect.left + leftInset, y + laneHeight};
        fillRectColor(
            dc,
            laneRect,
            laneSelected
                ? blendColor(kUiPetrol, kUiAnthracite, 1, 3)
                : ((lane % 2) == 0 ? blendColor(kUiPetrol, kUiShadow, 1, 6) : blendColor(kUiPetrol, kUiAnthracite, 1, 8)));
        fillRectColor(
            dc,
            headerLaneRect,
            laneSelected
                ? blendColor(kUiGraphite, kUiAnthracite, 1, 2)
                : ((lane % 2) == 0 ? blendColor(kUiAnthracite, kUiShadow, 1, 5) : blendColor(kUiGraphite, kUiShadow, 1, 6)));

        RECT metaRect{rect.left + 12, y + 4, rect.left + leftInset - 30, y + 15};
        RECT labelRect{rect.left + 12, y + 15, rect.left + leftInset - 30, y + laneHeight - 4};
        SetTextColor(dc, kUiTextDim);
        DrawTextA(dc, laneMeta.c_str(), -1, &metaRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        SetTextColor(dc, isTrackLane ? kUiText : kUiTextSoft);
        DrawTextA(dc, laneName.c_str(), -1, &labelRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        if (isTrackLane)
        {
            RECT indicatorRect{rect.left + leftInset - 18, y + (laneHeight / 2) - 4, rect.left + leftInset - 10, y + (laneHeight / 2) + 4};
            drawFilledCircle(dc, indicatorRect, laneSelected ? kUiLime : kUiLimeDim, blendColor(kUiLime, kUiShadow, 1, 3));
        }

        drawHorizontalLine(dc, rect.left, rect.right, y, kUiLineSoft);
    }

    for (int col = firstVisibleCell; col <= lastVisibleCell; ++col)
    {
        const int x = rect.left + leftInset + (col * columnWidth) - playlistScrollX_;
        const bool majorDivision = (col % 4) == 0;
        drawVerticalLine(
            dc,
            x,
            timelineTop,
            rect.bottom,
            majorDivision ? kUiLine : blendColor(kUiPetrol, kUiLineSoft, 1, 6));

        if (col < totalCells)
        {
            RECT beatRect{x + 4, timelineTop + 5, x + 32, gridTop - 4};
            SetTextColor(dc, majorDivision ? kUiText : kUiTextSoft);
            const std::string beatLabel = std::to_string(col + 1);
            DrawTextA(dc, beatLabel.c_str(), -1, &beatRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        }
    }

    drawVerticalLine(dc, rect.left + leftInset, timelineTop, rect.bottom, kUiLine);

    for (const auto& marker : workspaceModel_.markers)
    {
        const int x = rect.left + leftInset + (marker.timelineCell * columnWidth) - playlistScrollX_;
        if (x < rect.left + leftInset - columnWidth || x > rect.right)
        {
            continue;
        }
        drawVerticalLine(dc, x, timelineTop, rect.bottom, blendColor(kUiLime, kUiLine, 1, 3));

        RECT markerRect{x + 4, timelineTop + 2, x + 72, timelineTop + 18};
        SetTextColor(dc, kUiTextSoft);
        DrawTextA(dc, marker.name.c_str(), -1, &markerRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }

    rebuildPlaylistVisuals(rect);

    for (const auto& clip : interactionState_.playlistClipVisuals)
    {
        RECT clipRect{clip.rect.x, clip.rect.y, clip.rect.x + clip.rect.width, clip.rect.y + clip.rect.height};
        const bool isAudioClip = clip.clipType == "Audio";
        fillRectColor(
            dc,
            clipRect,
            clip.selected
                ? (isAudioClip ? RGB(100, 122, 103) : RGB(89, 112, 126))
                : (isAudioClip ? RGB(73, 93, 77) : RGB(66, 86, 100)));
        drawSurfaceFrame(dc, clipRect, clip.selected ? blendColor(kUiLime, kUiLine, 1, 3) : kUiLineSoft);
        RECT leftHandle{clipRect.left, clipRect.top, clipRect.left + 6, clipRect.bottom};
        RECT rightHandle{clipRect.right - 6, clipRect.top, clipRect.right, clipRect.bottom};
        fillRectColor(dc, leftHandle, blendColor(kUiTextSoft, isAudioClip ? RGB(112, 140, 118) : RGB(100, 130, 145), 1, 2));
        fillRectColor(dc, rightHandle, blendColor(kUiTextSoft, isAudioClip ? RGB(112, 140, 118) : RGB(100, 130, 145), 1, 2));

        RECT clipLabelRect{clipRect.left + 10, clipRect.top + 2, clipRect.right - 8, clipRect.bottom - 2};
        SetTextColor(dc, kUiText);
        DrawTextA(dc, clip.label.c_str(), -1, &clipLabelRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }

    if (interactionState_.marqueeActive && interactionState_.activeSurface == SurfaceKind::Playlist)
    {
        HPEN marqueePen = CreatePen(PS_DOT, 1, kUiText);
        HGDIOBJ oldPen = SelectObject(dc, marqueePen);
        HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
        Rectangle(dc, interactionState_.marqueeRect.left, interactionState_.marqueeRect.top, interactionState_.marqueeRect.right, interactionState_.marqueeRect.bottom);
        SelectObject(dc, oldBrush);
        SelectObject(dc, oldPen);
        DeleteObject(marqueePen);
    }

    drawSurfaceFrame(dc, rect, kUiLine);
}

void UI::paintMixerSurface(HDC dc, const RECT& rect)
{
    drawSurfaceHeader(dc, rect, "Mixer", "Routing, FX and meters");
    const int stripWidth = 84;
    const int stripGap = 12;
    const int meterTop = rect.top + 40;
    const int meterBottom = rect.bottom - 18;
    int x = rect.left + 12;

    for (std::size_t stripIndex = 0; stripIndex < workspaceModel_.mixerStrips.size(); ++stripIndex)
    {
        const MixerStripState& strip = workspaceModel_.mixerStrips[stripIndex];
        RECT stripRect{x, rect.top + 8, x + stripWidth, rect.bottom - 8};
        HBRUSH stripBrush = CreateSolidBrush(RGB(44, 48, 56));
        FillRect(dc, &stripRect, stripBrush);
        DeleteObject(stripBrush);

        RECT meterRect{x + 28, meterTop, x + 50, meterBottom};
        HBRUSH meterBg = CreateSolidBrush(RGB(25, 26, 29));
        FillRect(dc, &meterRect, meterBg);
        DeleteObject(meterBg);

        const int fillHeight = ((meterBottom - meterTop) * strip.peakLevel) / 100;
        RECT fillRect{meterRect.left, meterBottom - fillHeight, meterRect.right, meterBottom};
        HBRUSH fillBrush = CreateSolidBrush(strip.name == "Master" ? RGB(239, 178, 52) : RGB(105, 230, 120));
        FillRect(dc, &fillRect, fillBrush);
        DeleteObject(fillBrush);

        RECT titleRect{x + 6, rect.bottom - 28, x + stripWidth - 6, rect.bottom - 8};
        SetTextColor(dc, RGB(232, 232, 232));
        DrawTextA(dc, strip.name.c_str(), -1, &titleRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        RECT fxRect{x + 6, rect.top + 10, x + stripWidth - 6, rect.top + 34};
        std::string fxLabel = strip.effects.empty() ? "Empty" : strip.effects.front();
        SetTextColor(dc, RGB(170, 180, 192));
        DrawTextA(dc, fxLabel.c_str(), -1, &fxRect, DT_CENTER | DT_VCENTER | DT_WORDBREAK);
        x += stripWidth + stripGap;
    }
    drawSurfaceFrame(dc, rect, RGB(94, 104, 118));
}

void UI::paintPluginSurface(HDC dc, const RECT& rect)
{
    drawSurfaceHeader(dc, rect, "Plugin / Channel Settings", "Wrapper and macro controls");
    RECT drawRect = rect;
    drawRect.top += 24;
    HBRUSH moduleBrush = CreateSolidBrush(RGB(48, 54, 64));
    RECT upper{rect.left + 12, rect.top + 12, rect.right - 12, rect.top + 72};
    FillRect(dc, &upper, moduleBrush);
    RECT lower{rect.left + 12, rect.top + 86, rect.right - 12, rect.bottom - 12};
    FillRect(dc, &lower, moduleBrush);
    DeleteObject(moduleBrush);

    SetTextColor(dc, RGB(236, 236, 236));
    DrawTextA(dc, buildPluginPanelText().c_str(), -1, &drawRect, DT_LEFT | DT_TOP | DT_WORDBREAK);
    drawSurfaceFrame(dc, rect, RGB(88, 98, 110));
}

void UI::handleSurfaceMouseDown(HWND hwnd, SurfaceKind kind, int x, int y)
{
    workspace_.focusedPane =
        kind == SurfaceKind::Browser ? WorkspacePane::Browser :
        kind == SurfaceKind::ChannelRack ? WorkspacePane::ChannelRack :
        kind == SurfaceKind::StepSequencer ? WorkspacePane::ChannelRack :
        kind == SurfaceKind::PianoRoll ? WorkspacePane::PianoRoll :
        kind == SurfaceKind::Playlist ? WorkspacePane::Playlist :
        kind == SurfaceKind::Mixer ? WorkspacePane::Mixer :
        WorkspacePane::Plugin;

    interactionState_.activeSurface = kind;
    interactionState_.mouseDown = true;
    interactionState_.dragStart = POINT{x, y};
    interactionState_.dragCurrent = POINT{x, y};
    interactionState_.marqueeActive = false;
    SetCapture(hwnd);

    if (kind == SurfaceKind::Playlist)
    {
        bool hitClip = false;
        const bool sliceToolActive = workspace_.playlistTool == EditorTool::Slice;
        interactionState_.draggingClip = false;
        interactionState_.resizingClipLeft = false;
        interactionState_.resizingClipRight = false;
        interactionState_.editingAutomationPoint = false;
        interactionState_.selectedClipId = 0;
        interactionState_.selectedAutomationLaneIndex = static_cast<std::size_t>(-1);
        interactionState_.selectedAutomationPointIndex = static_cast<std::size_t>(-1);
        for (auto& automation : workspaceModel_.automationLanes)
        {
            automation.selected = false;
            automation.selectedPoint = static_cast<std::size_t>(-1);
        }
        for (auto& clip : interactionState_.playlistClipVisuals)
        {
            const bool hit =
                x >= clip.rect.x && x <= (clip.rect.x + clip.rect.width) &&
                y >= clip.rect.y && y <= (clip.rect.y + clip.rect.height);
            clip.selected = hit;
            if (hit)
            {
                hitClip = true;
                interactionState_.selectedClipId = clip.clipId;
                const bool canManipulateClip = !sliceToolActive || clip.clipType != "Pattern";
                if (canManipulateClip)
                {
                    const bool leftEdge = x <= (clip.rect.x + 6);
                    const bool rightEdge = x >= (clip.rect.x + clip.rect.width - 6);
                    interactionState_.resizingClipLeft = leftEdge && clip.clipId < 4000;
                    interactionState_.resizingClipRight = rightEdge;
                    interactionState_.draggingClip = !interactionState_.resizingClipLeft && !interactionState_.resizingClipRight;
                }
                if (clip.clipId >= 4000)
                {
                    for (std::size_t automationIndex = 0; automationIndex < workspaceModel_.automationLanes.size(); ++automationIndex)
                    {
                        auto& automation = workspaceModel_.automationLanes[automationIndex];
                        if (automation.clipId == clip.clipId)
                        {
                            automation.selected = true;
                            interactionState_.selectedAutomationLaneIndex = automationIndex;
                            const int pointCount = static_cast<int>(automation.values.size());
                            const int width = std::max(1, clip.rect.width - 6);
                            for (int point = 0; point < pointCount; ++point)
                            {
                                const int pointX = clip.rect.x + (width * point) / std::max(1, pointCount - 1) + 3;
                                const int pointY = clip.rect.y + clip.rect.height - 4 - ((clip.rect.height - 8) * automation.values[static_cast<std::size_t>(point)]) / 100;
                                if (std::abs(x - pointX) <= 6 && std::abs(y - pointY) <= 6)
                                {
                                    interactionState_.editingAutomationPoint = true;
                                    interactionState_.selectedAutomationPointIndex = static_cast<std::size_t>(point);
                                    automation.selectedPoint = static_cast<std::size_t>(point);
                                    interactionState_.draggingClip = false;
                                    interactionState_.resizingClipRight = false;
                                    break;
                                }
                            }
                            break;
                        }
                    }
                }
            }
        }
        if (!hitClip &&
            !interactionState_.editingAutomationPoint &&
            workspace_.playlistTool != EditorTool::Draw &&
            workspace_.playlistTool != EditorTool::Slice)
        {
            interactionState_.marqueeActive = true;
            interactionState_.marqueeRect = RECT{x, y, x, y};
        }
    }
    else if (kind == SurfaceKind::PianoRoll)
    {
        bool hitNote = false;
        interactionState_.draggingNote = false;
        interactionState_.resizingNote = false;
        interactionState_.selectedNoteIndex = static_cast<std::size_t>(-1);
        for (std::size_t index = 0; index < interactionState_.pianoNoteVisuals.size(); ++index)
        {
            auto& note = interactionState_.pianoNoteVisuals[index];
            const bool hit =
                x >= note.rect.x && x <= (note.rect.x + note.rect.width) &&
                y >= note.rect.y && y <= (note.rect.y + note.rect.height);
            note.selected = hit;
            if (hit)
            {
                hitNote = true;
                interactionState_.resizingNote = x >= (note.rect.x + note.rect.width - 6);
                interactionState_.draggingNote = !interactionState_.resizingNote;
                interactionState_.selectedNoteIndex = index;
            }
        }
        if (!hitNote)
        {
            interactionState_.marqueeActive = true;
            interactionState_.marqueeRect = RECT{x, y, x, y};
        }
    }
    else if (kind == SurfaceKind::Browser)
    {
        interactionState_.browserDragActive = false;
        const int contentTop = 2;
        const int browserIndex = y < contentTop ? -1 : ((y - contentTop + browserScrollY_) / kBrowserRowHeight);
        if (!workspaceModel_.browserEntries.empty() &&
            browserIndex >= 0 &&
            browserIndex < static_cast<int>(workspaceModel_.browserEntries.size()))
        {
            interactionState_.selectedBrowserItemIndex = static_cast<std::size_t>(browserIndex);
            workspaceModel_.selectedBrowserIndex = browserIndex;
            const BrowserEntry& entry = workspaceModel_.browserEntries[static_cast<std::size_t>(browserIndex)];
            if (entry.group)
            {
                toggleBrowserGroup(entry.id);
                interactionState_.mouseDown = false;
                ReleaseCapture();
                return;
            }

            if (entry.id.rfind("project:pattern:", 0) == 0)
            {
                selectPatternByNumber(std::atoi(entry.id.substr(16).c_str()));
            }
            else if (entry.id.rfind("project:audio:", 0) == 0)
            {
                interactionState_.selectedClipId = static_cast<std::uint32_t>(std::strtoul(entry.id.substr(14).c_str(), nullptr, 10));
                for (const auto& block : workspaceModel_.playlistBlocks)
                {
                    if (block.clipId == interactionState_.selectedClipId)
                    {
                        selectedTrackIndex_ = static_cast<std::size_t>(std::max(0, block.lane));
                        break;
                    }
                }
                invalidateSurface(playlistPanel_);
            }
        }
    }
    else if (kind == SurfaceKind::ChannelRack)
    {
        const int laneIndex = clampValue((y - 30) / 30, 0, std::max(0, static_cast<int>(workspaceModel_.patternLanes.size()) - 1));
        selectedTrackIndex_ = static_cast<std::size_t>(laneIndex);
        workspaceModel_.activeChannelIndex = laneIndex;
    }
    else if (kind == SurfaceKind::StepSequencer && !workspaceModel_.patternLanes.empty())
    {
        RECT clientRect{};
        GetClientRect(hwnd, &clientRect);
        const int clientWidth = static_cast<int>(clientRect.right);
        const int stepWidth = std::max<int>(16, std::max<int>(1, clientWidth - 110) / kStepCount);
        const int laneIndex = clampValue((y - 34) / 24, 0, std::max(0, static_cast<int>(workspaceModel_.patternLanes.size()) - 1));
        const int stepIndex = clampValue((x - 110) / stepWidth, 0, kStepCount - 1);
        selectedTrackIndex_ = static_cast<std::size_t>(laneIndex);
        workspaceModel_.activeChannelIndex = laneIndex;
        auto& lane = workspaceModel_.patternLanes[static_cast<std::size_t>(laneIndex)];
        if (stepIndex < static_cast<int>(lane.steps.size()))
        {
            lane.steps[static_cast<std::size_t>(stepIndex)].enabled = !lane.steps[static_cast<std::size_t>(stepIndex)].enabled;
            lane.steps[static_cast<std::size_t>(stepIndex)].velocity =
                lane.steps[static_cast<std::size_t>(stepIndex)].enabled ? 108 : 0;
            workspaceModel_.patterns[static_cast<std::size_t>(workspaceModel_.selectedPatternIndex)].lanes[static_cast<std::size_t>(laneIndex)] = lane;

            AudioEngine::EngineCommand command{};
            command.type = AudioEngine::CommandType::TogglePatternStep;
            command.textValue =
                std::to_string(workspace_.activePattern) + "|" +
                std::to_string(lane.trackId) + "|" +
                std::to_string(stepIndex);
            engine_.postCommand(command);
        }
    }

    invalidateSurface(hwnd);
}

void UI::handleSurfaceMouseMove(HWND hwnd, SurfaceKind kind, int x, int y, WPARAM flags)
{
    if (!interactionState_.mouseDown || (flags & MK_LBUTTON) == 0)
    {
        return;
    }

    interactionState_.dragCurrent = POINT{x, y};

    if (kind == SurfaceKind::Playlist && interactionState_.draggingClip)
    {
        const int dx = x - interactionState_.dragStart.x;
        const int dy = y - interactionState_.dragStart.y;
        for (auto& clip : interactionState_.playlistClipVisuals)
        {
            if (clip.clipId == interactionState_.selectedClipId)
            {
                clip.rect.x += dx;
                clip.rect.y += dy;
                interactionState_.dragStart.x = x;
                interactionState_.dragStart.y = y;
                break;
            }
        }
    }
    else if (kind == SurfaceKind::Playlist && (interactionState_.resizingClipLeft || interactionState_.resizingClipRight))
    {
        for (auto& clip : interactionState_.playlistClipVisuals)
        {
            if (clip.clipId != interactionState_.selectedClipId)
            {
                continue;
            }

            if (interactionState_.resizingClipLeft)
            {
                const int right = clip.rect.x + clip.rect.width;
                clip.rect.x = std::min(x, right - 24);
                clip.rect.width = std::max<int>(24, right - clip.rect.x);
            }
            else
            {
                clip.rect.width = std::max<int>(24, clip.rect.width + (x - static_cast<int>(interactionState_.dragStart.x)));
                interactionState_.dragStart.x = x;
            }
            break;
        }
    }
    else if (kind == SurfaceKind::Playlist && interactionState_.editingAutomationPoint &&
             interactionState_.selectedAutomationLaneIndex < workspaceModel_.automationLanes.size() &&
             interactionState_.selectedAutomationPointIndex < workspaceModel_.automationLanes[interactionState_.selectedAutomationLaneIndex].values.size())
    {
        RECT clientRect{};
        GetClientRect(hwnd, &clientRect);
        const int laneHeight = kPlaylistLaneHeight;
        const int timelineHeight = kSurfaceHeaderHeight + kPlaylistTimelineHeight;
        auto& automation = workspaceModel_.automationLanes[interactionState_.selectedAutomationLaneIndex];
        const int clipY = timelineHeight + 6 + (automation.lane * laneHeight);
        const int normalized = clampValue(100 - ((y - clipY) * 100) / std::max<int>(10, laneHeight - 10), 0, 100);
        automation.values[interactionState_.selectedAutomationPointIndex] = normalized;
    }
    else if (kind == SurfaceKind::PianoRoll && interactionState_.draggingNote &&
             interactionState_.selectedNoteIndex < interactionState_.pianoNoteVisuals.size())
    {
        auto& note = interactionState_.pianoNoteVisuals[interactionState_.selectedNoteIndex];
        note.rect.x += (x - interactionState_.dragStart.x);
        note.rect.y += (y - interactionState_.dragStart.y);
        interactionState_.dragStart = POINT{x, y};
    }
    else if (kind == SurfaceKind::PianoRoll && interactionState_.resizingNote &&
             interactionState_.selectedNoteIndex < interactionState_.pianoNoteVisuals.size())
    {
        auto& note = interactionState_.pianoNoteVisuals[interactionState_.selectedNoteIndex];
        note.rect.width = std::max<int>(18, note.rect.width + (x - static_cast<int>(interactionState_.dragStart.x)));
        interactionState_.dragStart = POINT{x, y};
    }
    else if (interactionState_.marqueeActive)
    {
        interactionState_.marqueeRect.left = std::min<int>(static_cast<int>(interactionState_.dragStart.x), x);
        interactionState_.marqueeRect.top = std::min<int>(static_cast<int>(interactionState_.dragStart.y), y);
        interactionState_.marqueeRect.right = std::max<int>(static_cast<int>(interactionState_.dragStart.x), x);
        interactionState_.marqueeRect.bottom = std::max<int>(static_cast<int>(interactionState_.dragStart.y), y);
    }

    invalidateSurface(hwnd);
}

void UI::handleSurfaceMouseUp(HWND hwnd, SurfaceKind kind, int x, int y)
{
    if (kind == SurfaceKind::PianoRoll && interactionState_.draggingNote &&
        interactionState_.selectedNoteIndex < interactionState_.pianoNoteVisuals.size() &&
        !workspaceModel_.patternLanes.empty())
    {
        RECT clientRect{};
        GetClientRect(hwnd, &clientRect);
        const int keyWidth = 72;
        const int laneHeight = 22;
        const int usableWidth = std::max<int>(1, static_cast<int>(clientRect.right) - keyWidth);
        const int stepWidth = std::max<int>(18, usableWidth / kPlaylistCellCount);

        UiNoteVisual& noteVisual = interactionState_.pianoNoteVisuals[interactionState_.selectedNoteIndex];
        PianoNoteState& noteState =
            workspaceModel_.patternLanes[static_cast<std::size_t>(workspaceModel_.activeChannelIndex)]
                .notes[interactionState_.selectedNoteIndex];

        noteState.step = clampValue((noteVisual.rect.x - keyWidth - 20) / stepWidth, 0, kPlaylistCellCount - 1);
        const int laneFromTop = (noteVisual.rect.y - 24) / laneHeight;
        noteState.lane = clampValue((kPianoLaneCount - 1) - laneFromTop, 0, kPianoLaneCount - 1);
        noteState.selected = true;
        workspaceModel_.patterns[static_cast<std::size_t>(workspaceModel_.selectedPatternIndex)]
            .lanes[static_cast<std::size_t>(workspaceModel_.activeChannelIndex)]
            .notes[interactionState_.selectedNoteIndex] = noteState;

        AudioEngine::EngineCommand command{};
        command.type = AudioEngine::CommandType::UpsertMidiNote;
        command.textValue =
            std::to_string(workspace_.activePattern) + "|" +
            std::to_string(workspaceModel_.patternLanes[static_cast<std::size_t>(workspaceModel_.activeChannelIndex)].trackId) + "|" +
            std::to_string(interactionState_.selectedNoteIndex) + "|" +
            std::to_string(noteState.lane) + "|" +
            std::to_string(noteState.step) + "|" +
            std::to_string(noteState.length) + "|" +
            std::to_string(noteState.velocity) + "|" +
            (noteState.accent ? "1" : "0") + "|" +
            (noteState.slide ? "1" : "0") + "|0";
        engine_.postCommand(command);
    }
    else if (kind == SurfaceKind::PianoRoll && interactionState_.resizingNote &&
             interactionState_.selectedNoteIndex < interactionState_.pianoNoteVisuals.size() &&
             !workspaceModel_.patternLanes.empty())
    {
        RECT clientRect{};
        GetClientRect(hwnd, &clientRect);
        const int keyWidth = 72;
        const int usableWidth = std::max<int>(1, static_cast<int>(clientRect.right) - keyWidth);
        const int stepWidth = std::max<int>(18, usableWidth / kPlaylistCellCount);

        UiNoteVisual& noteVisual = interactionState_.pianoNoteVisuals[interactionState_.selectedNoteIndex];
        PianoNoteState& noteState =
            workspaceModel_.patternLanes[static_cast<std::size_t>(workspaceModel_.activeChannelIndex)]
                .notes[interactionState_.selectedNoteIndex];
        noteState.length = std::max<int>(1, noteVisual.rect.width / std::max<int>(1, stepWidth));
        workspaceModel_.patterns[static_cast<std::size_t>(workspaceModel_.selectedPatternIndex)]
            .lanes[static_cast<std::size_t>(workspaceModel_.activeChannelIndex)]
            .notes[interactionState_.selectedNoteIndex] = noteState;

        AudioEngine::EngineCommand command{};
        command.type = AudioEngine::CommandType::UpsertMidiNote;
        command.textValue =
            std::to_string(workspace_.activePattern) + "|" +
            std::to_string(workspaceModel_.patternLanes[static_cast<std::size_t>(workspaceModel_.activeChannelIndex)].trackId) + "|" +
            std::to_string(interactionState_.selectedNoteIndex) + "|" +
            std::to_string(noteState.lane) + "|" +
            std::to_string(noteState.step) + "|" +
            std::to_string(noteState.length) + "|" +
            std::to_string(noteState.velocity) + "|" +
            (noteState.accent ? "1" : "0") + "|" +
            (noteState.slide ? "1" : "0") + "|0";
        engine_.postCommand(command);
    }

    if (kind == SurfaceKind::PianoRoll && !workspaceModel_.patternLanes.empty() &&
        !interactionState_.draggingNote && !interactionState_.resizingNote &&
        !interactionState_.marqueeActive && interactionState_.selectedNoteIndex == static_cast<std::size_t>(-1))
    {
        RECT clientRect{};
        GetClientRect(hwnd, &clientRect);
        const int keyWidth = 72;
        const int laneHeight = 22;
        const int gridTop = 24;
        const int usableWidth = std::max<int>(1, static_cast<int>(clientRect.right) - keyWidth);
        const int stepWidth = std::max<int>(18, usableWidth / kPlaylistCellCount);
        if (x > keyWidth && y > gridTop)
        {
            const int laneFromTop = (y - gridTop) / laneHeight;
            const int lane = clampValue((kPianoLaneCount - 1) - laneFromTop, 0, kPianoLaneCount - 1);
            const int step = clampValue((x - keyWidth) / stepWidth, 0, kPlaylistCellCount - 1);
            auto& laneState = workspaceModel_.patternLanes[static_cast<std::size_t>(workspaceModel_.activeChannelIndex)];
            laneState.notes.push_back(PianoNoteState{lane, step, 2, 96, false, false, false});
            workspaceModel_.patterns[static_cast<std::size_t>(workspaceModel_.selectedPatternIndex)]
                .lanes[static_cast<std::size_t>(workspaceModel_.activeChannelIndex)] = laneState;

            AudioEngine::EngineCommand command{};
            command.type = AudioEngine::CommandType::UpsertMidiNote;
            command.textValue =
                std::to_string(workspace_.activePattern) + "|" +
                std::to_string(laneState.trackId) + "|" +
                std::to_string(laneState.notes.size() - 1) + "|" +
                std::to_string(lane) + "|" +
                std::to_string(step) + "|2|96|0|0|1";
            engine_.postCommand(command);
        }
    }

    if (interactionState_.mouseDown && interactionState_.marqueeActive)
    {
        if (kind == SurfaceKind::Playlist)
        {
            for (auto& clip : interactionState_.playlistClipVisuals)
            {
                const RECT clipRect{
                    clip.rect.x,
                    clip.rect.y,
                    clip.rect.x + clip.rect.width,
                    clip.rect.y + clip.rect.height};
                RECT overlap{};
                clip.selected = IntersectRect(&overlap, &clipRect, &interactionState_.marqueeRect) != 0;
            }
        }
        else if (kind == SurfaceKind::PianoRoll)
        {
            for (auto& note : interactionState_.pianoNoteVisuals)
            {
                const RECT noteRect{
                    note.rect.x,
                    note.rect.y,
                    note.rect.x + note.rect.width,
                    note.rect.y + note.rect.height};
                RECT overlap{};
                note.selected = IntersectRect(&overlap, &noteRect, &interactionState_.marqueeRect) != 0;
            }
            if (workspaceModel_.activeChannelIndex >= 0 &&
                workspaceModel_.activeChannelIndex < static_cast<int>(workspaceModel_.patternLanes.size()))
            {
                auto& notes = workspaceModel_.patternLanes[static_cast<std::size_t>(workspaceModel_.activeChannelIndex)].notes;
                for (std::size_t index = 0; index < notes.size() && index < interactionState_.pianoNoteVisuals.size(); ++index)
                {
                    notes[index].selected = interactionState_.pianoNoteVisuals[index].selected;
                }
                workspaceModel_.patterns[static_cast<std::size_t>(workspaceModel_.selectedPatternIndex)]
                    .lanes[static_cast<std::size_t>(workspaceModel_.activeChannelIndex)]
                    .notes = notes;
            }
        }
    }

    if (kind == SurfaceKind::Playlist)
    {
        const bool playlistClipManipulated =
            interactionState_.draggingClip ||
            interactionState_.resizingClipLeft ||
            interactionState_.resizingClipRight;

        if (interactionState_.selectedClipId != 0)
        {
            const auto clipIt = std::find_if(
                interactionState_.playlistClipVisuals.begin(),
                interactionState_.playlistClipVisuals.end(),
                [&](const UiClipVisual& clip) { return clip.clipId == interactionState_.selectedClipId; });

            if (clipIt != interactionState_.playlistClipVisuals.end() &&
                (clipIt->clipId >= 4000 || !visibleState_.project.tracks.empty()))
            {
                RECT clientRect{};
                GetClientRect(hwnd, &clientRect);
                const int laneHeight = kPlaylistLaneHeight;
                const int timelineHeight = kSurfaceHeaderHeight + kPlaylistTimelineHeight;
                const int leftInset = kPlaylistTrackHeaderWidth;
                const int clientWidth = static_cast<int>(clientRect.right - clientRect.left);
                const int columnWidth = playlistColumnWidth(std::max<int>(1, clientWidth - leftInset));
                const int targetLane = std::max<int>(0, (clipIt->rect.y + playlistScrollY_ - timelineHeight) / laneHeight);
                const int targetStartCell =
                    clampValue((clipIt->rect.x + playlistScrollX_ - leftInset) / std::max<int>(1, columnWidth), 0, playlistTotalCellCount() - 1);
                const int targetLengthCells =
                    std::max<int>(2, clipIt->rect.width / std::max<int>(1, columnWidth));

                if (playlistClipManipulated)
                {
                    if (clipIt->clipId >= 4000)
                    {
                        const auto playlistItemIt = std::find_if(
                            visibleState_.project.playlistItems.begin(),
                            visibleState_.project.playlistItems.end(),
                            [&](const AudioEngine::PlaylistItemState& item) { return item.itemId == clipIt->clipId; });
                        for (auto& automation : workspaceModel_.automationLanes)
                        {
                            if (automation.clipId == clipIt->clipId)
                            {
                                automation.lane = targetLane;
                                automation.startCell = targetStartCell;
                                automation.lengthCells = targetLengthCells;
                                automation.selected = true;
                                break;
                            }
                        }

                        if (playlistItemIt != visibleState_.project.playlistItems.end())
                        {
                            AudioEngine::EngineCommand command{};
                            command.type = AudioEngine::CommandType::MoveClip;
                            command.uintValue = clipIt->clipId;
                            command.secondaryUintValue = playlistItemIt->trackId;
                            command.doubleValue = static_cast<double>(targetStartCell) / 2.0;
                            command.textValue = std::to_string(static_cast<double>(targetLengthCells) / 2.0);
                            engine_.postCommand(command);
                        }
                    }
                    else
                    {
                        const int safeLane = clampValue(targetLane, 0, static_cast<int>(visibleState_.project.tracks.size() - 1));
                        const double startTime = static_cast<double>(targetStartCell) / 2.0;
                        const double durationTime = static_cast<double>(targetLengthCells) / 2.0;
                        for (auto& block : workspaceModel_.playlistBlocks)
                        {
                            if (block.clipId == clipIt->clipId)
                            {
                                block.lane = safeLane;
                                block.startCell = targetStartCell;
                                block.lengthCells = targetLengthCells;
                                break;
                            }
                        }

                        AudioEngine::EngineCommand command{};
                        command.type = AudioEngine::CommandType::MoveClip;
                        command.uintValue = clipIt->clipId;
                        command.secondaryUintValue = visibleState_.project.tracks[static_cast<std::size_t>(safeLane)].trackId;
                        command.doubleValue = startTime;
                        command.textValue = std::to_string(durationTime);
                        engine_.postCommand(command);
                    }
                }
                else if (workspace_.playlistTool == EditorTool::Slice &&
                         clipIt->clipId < 4000 &&
                         clipIt->clipType == "Pattern")
                {
                    const auto blockIt = std::find_if(
                        workspaceModel_.playlistBlocks.begin(),
                        workspaceModel_.playlistBlocks.end(),
                        [&](const PlaylistBlockState& block) { return block.clipId == clipIt->clipId; });

                    if (blockIt != workspaceModel_.playlistBlocks.end())
                    {
                        const int clipEndCell = blockIt->startCell + blockIt->lengthCells;
                        const int splitCell =
                            clampValue(
                                (x + playlistScrollX_ - leftInset) / std::max<int>(1, columnWidth),
                                blockIt->startCell + 1,
                                clipEndCell - 1);

                        if (splitCell > blockIt->startCell &&
                            splitCell < clipEndCell &&
                            blockIt->lane >= 0 &&
                            blockIt->lane < static_cast<int>(visibleState_.project.tracks.size()))
                        {
                            const double leftDuration = static_cast<double>(splitCell - blockIt->startCell) / 2.0;
                            const double rightDuration = static_cast<double>(clipEndCell - splitCell) / 2.0;
                            if (leftDuration >= 0.5 && rightDuration >= 0.5)
                            {
                                AudioEngine::EngineCommand leftCommand{};
                                leftCommand.type = AudioEngine::CommandType::MoveClip;
                                leftCommand.uintValue = clipIt->clipId;
                                leftCommand.secondaryUintValue = visibleState_.project.tracks[static_cast<std::size_t>(blockIt->lane)].trackId;
                                leftCommand.doubleValue = static_cast<double>(blockIt->startCell) / 2.0;
                                leftCommand.textValue = std::to_string(leftDuration);
                                engine_.postCommand(leftCommand);

                                AudioEngine::EngineCommand rightCommand{};
                                rightCommand.type = AudioEngine::CommandType::AddClipToTrack;
                                rightCommand.uintValue = visibleState_.project.tracks[static_cast<std::size_t>(blockIt->lane)].trackId;
                                rightCommand.textValue = "Pattern " + std::to_string(std::max(1, blockIt->patternNumber));
                                rightCommand.doubleValue = static_cast<double>(splitCell) / 2.0;
                                rightCommand.secondaryTextValue = std::to_string(rightDuration);
                                engine_.postCommand(rightCommand);
                            }
                        }
                    }
                }
            }
        }

        if (workspace_.playlistTool == EditorTool::Draw &&
            interactionState_.selectedClipId == 0 &&
            !interactionState_.marqueeActive &&
            !visibleState_.project.tracks.empty())
        {
            RECT clientRect{};
            GetClientRect(hwnd, &clientRect);
            const int timelineHeight = kSurfaceHeaderHeight + kPlaylistTimelineHeight;
            const int leftInset = kPlaylistTrackHeaderWidth;
            if (x >= leftInset && y >= timelineHeight)
            {
                const int clientWidth = static_cast<int>(clientRect.right - clientRect.left);
                const int columnWidth = playlistColumnWidth(std::max<int>(1, clientWidth - leftInset));
                const int targetLane = std::max<int>(0, (y + playlistScrollY_ - timelineHeight) / kPlaylistLaneHeight);
                const int safeLane = clampValue(targetLane, 0, static_cast<int>(visibleState_.project.tracks.size() - 1));
                const int targetCell =
                    clampValue((x + playlistScrollX_ - leftInset) / std::max<int>(1, columnWidth), 0, playlistTotalCellCount() - 1);

                selectedTrackIndex_ = static_cast<std::size_t>(safeLane);

                AudioEngine::EngineCommand command{};
                command.type = AudioEngine::CommandType::AddClipToTrack;
                command.uintValue = visibleState_.project.tracks[static_cast<std::size_t>(safeLane)].trackId;
                command.textValue = "Pattern " + std::to_string(std::max(1, workspace_.activePattern));
                command.doubleValue = static_cast<double>(targetCell) / 2.0;
                command.secondaryTextValue = std::to_string(getPatternPlaybackLengthSeconds());
                engine_.postCommand(command);
            }
        }
    }

    if (kind == SurfaceKind::Playlist &&
        interactionState_.editingAutomationPoint &&
        interactionState_.selectedAutomationLaneIndex < workspaceModel_.automationLanes.size() &&
        interactionState_.selectedAutomationPointIndex < workspaceModel_.automationLanes[interactionState_.selectedAutomationLaneIndex].values.size())
    {
        const auto& automation = workspaceModel_.automationLanes[interactionState_.selectedAutomationLaneIndex];
        const int pointCount = static_cast<int>(automation.values.size());
        const int pointIndex = static_cast<int>(interactionState_.selectedAutomationPointIndex);
        const int cell =
            automation.startCell +
            (automation.lengthCells * pointIndex) / std::max(1, pointCount - 1);

        AudioEngine::EngineCommand command{};
        command.type = AudioEngine::CommandType::SetAutomationPoint;
        command.textValue =
            std::to_string(automation.clipId) + "|" +
            std::to_string(pointIndex) + "|" +
            std::to_string(cell) + "|" +
            std::to_string(automation.values[interactionState_.selectedAutomationPointIndex]);
        engine_.postCommand(command);
    }

    if (interactionState_.browserDragActive)
    {
        POINT screenPoint{};
        GetCursorPos(&screenPoint);
        HWND targetWindow = WindowFromPoint(screenPoint);
        const SurfaceKind targetKind = kindFromSurfaceHandle(targetWindow);

        const std::string droppedName =
            workspaceModel_.browserEntries.empty()
                ? (currentBrowserTabLabel() + " Item " + std::to_string(interactionState_.selectedBrowserItemIndex + 1))
                : workspaceModel_.browserEntries[static_cast<std::size_t>(workspaceModel_.selectedBrowserIndex)].label;

        if (targetKind == SurfaceKind::Playlist && visibleState_.selection.selectedTrackId != 0)
        {
            AudioEngine::EngineCommand command{};
            command.type = AudioEngine::CommandType::AddClipToTrack;
            command.uintValue = visibleState_.selection.selectedTrackId;
            command.textValue = droppedName;
            engine_.postCommand(command);
        }
        else if (targetKind == SurfaceKind::ChannelRack)
        {
            AudioEngine::EngineCommand command{};
            command.type = AudioEngine::CommandType::AddTrack;
            command.textValue = droppedName;
            engine_.postCommand(command);
        }
    }

    interactionState_.mouseDown = false;
    interactionState_.draggingClip = false;
    interactionState_.draggingNote = false;
    interactionState_.resizingClipLeft = false;
    interactionState_.resizingClipRight = false;
    interactionState_.resizingNote = false;
    interactionState_.editingAutomationPoint = false;
    interactionState_.browserDragActive = false;
    interactionState_.marqueeActive = false;
    interactionState_.selectedAutomationLaneIndex = static_cast<std::size_t>(-1);
    interactionState_.selectedAutomationPointIndex = static_cast<std::size_t>(-1);
    ReleaseCapture();
    invalidateSurface(hwnd);
}


