#pragma once

#include <algorithm>
#include <atomic>
#include <vector>
#include <cstring>

namespace atk
{

// Simple lock-free FIFO buffer for audio
class FifoBuffer
{
public:
    FifoBuffer()
        : numChannels(0)
        , totalSize(0)
        , readPos(0)
        , writePos(0)
    {
    }

    void reset()
    {
        readPos.store(0, std::memory_order_release);
        writePos.store(0, std::memory_order_release);
        for (auto& channel : buffer)
            std::fill(channel.begin(), channel.end(), 0.0f);
    }

    void read(float* dest, int channel, int numSamples, bool advance = true)
    {
        if (channel >= numChannels || numSamples <= 0)
            return;

        const int readPosition = readPos.load(std::memory_order_acquire);
        const int available = getNumReady();
        const int toRead = std::min(numSamples, available);

        if (toRead <= 0)
            return;

        const int start1 = readPosition;
        const int size1 = std::min(toRead, totalSize - start1);
        const int size2 = toRead - size1;

        const float* src = buffer[channel].data();
        if (size1 > 0)
            std::copy(src + start1, src + start1 + size1, dest);

        if (size2 > 0)
            std::copy(src, src + size2, dest + size1);

        if (advance)
            readPos.store((readPosition + toRead) % totalSize, std::memory_order_release);
    }

    void write(const float* data, int channel, int numSamples, bool advance = true)
    {
        if (channel >= numChannels || numSamples <= 0)
            return;

        const int writePosition = writePos.load(std::memory_order_acquire);
        const int freeSpace = getFreeSpace();
        const int toWrite = std::min(numSamples, freeSpace);

        if (toWrite <= 0)
            return;

        const int start1 = writePosition;
        const int size1 = std::min(toWrite, totalSize - start1);
        const int size2 = toWrite - size1;

        float* dst = buffer[channel].data();
        if (size1 > 0)
            std::copy(data, data + size1, dst + start1);

        if (size2 > 0)
            std::copy(data + size1, data + size1 + size2, dst);

        if (advance)
            writePos.store((writePosition + toWrite) % totalSize, std::memory_order_release);
    }

    void advanceRead(int numSamples)
    {
        const int readPosition = readPos.load(std::memory_order_acquire);
        readPos.store((readPosition + numSamples) % totalSize, std::memory_order_release);
    }

    int getNumReady() const
    {
        const int writePosition = writePos.load(std::memory_order_acquire);
        const int readPosition = readPos.load(std::memory_order_acquire);
        if (writePosition >= readPosition)
            return writePosition - readPosition;
        else
            return totalSize - readPosition + writePosition;
    }

    int getTotalSize() const
    {
        return totalSize;
    }

    int getFreeSpace() const
    {
        // Keep one sample as guard to distinguish full from empty
        return totalSize - getNumReady() - 1;
    }

    int getNumChannels() const
    {
        return numChannels;
    }

    void setSize(int newNumChannels, int numSamples)
    {
        if (newNumChannels == numChannels && numSamples == totalSize)
            return;

        numChannels = newNumChannels;
        totalSize = numSamples;
        buffer.resize(numChannels);
        for (auto& channel : buffer)
            channel.resize(totalSize, 0.0f);

        reset();
    }

private:
    int numChannels;
    int totalSize;
    std::atomic<int> readPos;
    std::atomic<int> writePos;
    std::vector<std::vector<float>> buffer;
};

} // namespace atk
