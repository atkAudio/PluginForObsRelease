#include "FifoBuffer.h"

#include "FifoBuffer2.h"

#include <juce_audio_utils/juce_audio_utils.h>

struct atk::FifoBuffer::Impl
{
    Impl()
    {
        fifo.reset(new juce::AbstractFifo(1));
    }

    void reset()
    {
        fifo->reset();
        for (auto& i : buffer)
            std::fill(i.begin(), i.end(), 0.0f);
    }

    void read(float* dest, int channel, int numSamples, bool advance = true)
    {
        int start1, size1, start2, size2;
        fifo->prepareToRead(numSamples, start1, size1, start2, size2);

        if (buffer.size() <= channel)
            return;

        if (size1 > 0)
            memcpy(dest, buffer[channel].data() + start1, size1 * sizeof(float));

        if (size2 > 0)
            memcpy(dest + size1, buffer[channel].data() + start2, size2 * sizeof(float));

        if (advance)
            fifo->finishedRead(size1 + size2);
    }

    void write(const float* data, int channel, int numSamples, bool advance = true)
    {
        int start1, size1, start2, size2;
        fifo->prepareToWrite(numSamples, start1, size1, start2, size2);

        if (buffer.size() <= channel)
            return;

        if (size1 > 0)
            memcpy(buffer[channel].data() + start1, data, size1 * sizeof(float));

        if (size2 > 0)
            memcpy(buffer[channel].data() + start2, data + size1, size2 * sizeof(float));

        if (advance)
            fifo->finishedWrite(size1 + size2);
    }

    int getNumReady()
    {
        return fifo->getNumReady();
    }

    int getTotalSize()
    {
        return fifo->getTotalSize();
    }

    int getFreeSpace()
    {
        return fifo->getFreeSpace();
    }

    int getNumChannels()
    {
        return (int)buffer.size();
    }

    void setSize(int numChannels, int numSamples)
    {
        if (buffer.size() == numChannels && numSamples + 1 == fifo->getTotalSize())
            return;

        fifo.reset(new juce::AbstractFifo(numSamples + 1));
        buffer.resize(numChannels);
        for (auto& i : buffer)
            i.resize(fifo->getTotalSize());

        for (auto& i : buffer)
            std::fill(i.begin(), i.end(), 0.0f);

        fifo->reset();
    }

    std::unique_ptr<juce::AbstractFifo> fifo;

    std::vector<std::vector<float>> buffer;
};

atk::FifoBuffer::FifoBuffer()
    : pImpl(new Impl())
{
}

atk::FifoBuffer::~FifoBuffer()
{
    delete pImpl;
}

void atk::FifoBuffer::reset()
{
    pImpl->reset();
}

void atk::FifoBuffer::read(float* dest, int channel, int numSamples, bool advance)
{
    pImpl->read(dest, channel, numSamples, advance);
}

void atk::FifoBuffer::write(const float* data, int channel, int numSamples, bool advance)
{
    pImpl->write(data, channel, numSamples, advance);
}

void atk::FifoBuffer::advanceRead(int numSamples)
{
    pImpl->fifo->finishedRead(numSamples);
}

int atk::FifoBuffer::getNumReady()
{
    return pImpl->getNumReady();
}

int atk::FifoBuffer::getTotalSize()
{
    return pImpl->getTotalSize();
}

int atk::FifoBuffer::getFreeSpace()
{
    return pImpl->getFreeSpace();
}

int atk::FifoBuffer::getNumChannels()
{
    return pImpl->getNumChannels();
}

void atk::FifoBuffer::setSize(int numChannels, int numSamples)
{
    pImpl->setSize(numChannels, numSamples);
}
