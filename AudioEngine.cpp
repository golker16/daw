#include "AudioEngine.h"

AudioEngine::AudioEngine()
    : initialized_(false), running_(false)
{
}

AudioEngine::~AudioEngine()
{
    stop();
}

void AudioEngine::initialize()
{
    initialized_ = true;
}

void AudioEngine::start()
{
    if (initialized_)
    {
        running_ = true;
    }
}

void AudioEngine::stop()
{
    running_ = false;
}

bool AudioEngine::isRunning() const
{
    return running_.load();
}

std::string AudioEngine::getStatusText() const
{
    if (!initialized_)
        return "Audio Engine: not initialized";

    return running_ ? "Audio Engine: running" : "Audio Engine: stopped";
}
