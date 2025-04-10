#pragma once

#include "atkaudio.h"

namespace atk
{
class DLL_EXPORT PluginHost
{
public:
    PluginHost();
    ~PluginHost();

    void process(float** buffer, int numChannels, int numSamples, double sampleRate);

    void setVisible(bool visible);

    void getState(std::string& s);
    void setState(std::string& s);

private:
    struct Impl;
    Impl* pImpl;
};
} // namespace atk