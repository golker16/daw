#pragma once

#include <string>
#include <atomic>

class AudioEngine
{
public:
    AudioEngine();
    ~AudioEngine();

    void initialize();
    void start();
    void stop();

    bool isRunning() const;
    std::string getStatusText() const;

private:
    std::atomic<bool> initialized_;
    std::atomic<bool> running_;
};
