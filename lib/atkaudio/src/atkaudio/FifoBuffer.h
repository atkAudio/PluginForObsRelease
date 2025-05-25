#pragma once

#include "atkaudio.h"

namespace atk
{

class FifoBuffer
{
public:
    FifoBuffer();
    ~FifoBuffer();

    void reset();

    void read(float* dest, int channel, int numSamples, bool advance = true);
    void write(const float* data, int channel, int numSamples, bool advance = true);
    void advanceRead(int numSamples);

    int getNumReady();
    int getTotalSize();
    int getFreeSpace();
    int getNumChannels();

    void setSize(int numChannels, int numSamples);

private:
    struct Impl;
    Impl* pImpl;
};

} // namespace atk
