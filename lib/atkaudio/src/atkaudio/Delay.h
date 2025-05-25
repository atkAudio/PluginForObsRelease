#pragma once

#include "atkaudio.h"

namespace atk
{
class Delay
{
public:
    Delay();
    ~Delay();

    void process(float** buffer, int numChannels, int numSamples, double sampleRate);

    void setDelay(float delay);

private:
    struct Impl;
    Impl* pImpl;
};
} // namespace atk
