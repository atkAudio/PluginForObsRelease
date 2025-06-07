#pragma once

#include "atkaudio.h"

#include <string>

namespace atk
{
class PluginHost2
{
public:
    PluginHost2();
    ~PluginHost2();

    void process(float** buffer, int numChannels, int numSamples, double sampleRate);

    void setVisible(bool visible);

    void getState(std::string& s);
    void setState(std::string& s);

    void initialise(int numInputChannels, int numOutputChannels, double sampleRate, void* obs_parent_source = nullptr);

private:
    struct Impl;
    Impl* pImpl;
};
} // namespace atk