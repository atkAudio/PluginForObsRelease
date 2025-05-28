#pragma once

#include "atkaudio.h"

#include <string>

namespace atk
{
class DeviceIo
{
public:
    DeviceIo();
    ~DeviceIo();

    void process(float** buffer, int numChannels, int numSamples, double sampleRate);

    void setVisible(bool visible);
    void setMixInput(bool mixInput);

    void getState(std::string& s);
    void setState(std::string& s);

private:
    struct Impl;
    Impl* pImpl;
};
} // namespace atk