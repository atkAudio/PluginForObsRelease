#include "AudioProcessorGraphMT.h"

#include "GraphPartitioner.h"
#include "AudioThreadPool.h"

#include <juce_dsp/juce_dsp.h>

// Pre-allocated buffer configuration for chain processing
#define CHAIN_MAX_CHANNELS 64 // Maximum audio channels per chain

namespace atk
{

// Import JUCE types into atk namespace for convenience
using atk::GraphPartitioner;
using juce::approximatelyEqual;
using juce::Array;
using juce::AudioBuffer;
using juce::AudioPlayHead;
using juce::AudioProcessor;
using juce::AudioProcessorEditor;
using juce::ChangeBroadcaster;
using juce::FloatVectorOperations;
using juce::jmax;
using juce::jmin;
using juce::LockingAsyncUpdater;
using juce::Logger;
using juce::MemoryBlock;
using juce::MessageManager;
using juce::MidiBuffer;
using juce::nullopt;
using juce::PluginDescription;
using juce::ReferenceCountedArray;
using juce::ReferenceCountedObjectPtr;
using juce::ScopedLock;
using juce::SpinLock;
using juce::String;
using juce::Thread;
using juce::Timer;
using juce::uint32;
using juce::uint64;

/*  Provides a comparison function for various types that have an associated NodeID,
    for use with equal_range, lower_bound etc.
*/
class ImplicitNode
{
public:
    using Node = AudioProcessorGraphMT::Node;
    using NodeID = AudioProcessorGraphMT::NodeID;
    using NodeAndChannel = AudioProcessorGraphMT::NodeAndChannel;

    ImplicitNode(NodeID x)
        : node(x)
    {
    }

    ImplicitNode(NodeAndChannel x)
        : ImplicitNode(x.nodeID)
    {
    }

    ImplicitNode(const Node* x)
        : ImplicitNode(x->nodeID)
    {
    }

    ImplicitNode(const std::pair<const NodeAndChannel, std::set<NodeAndChannel>>& x)
        : ImplicitNode(x.first)
    {
    }

    /*  This is the comparison function. */
    static bool compare(ImplicitNode a, ImplicitNode b)
    {
        return a.node < b.node;
    }

private:
    NodeID node;
};

//==============================================================================
/*  A copyable type holding all the nodes, and allowing fast lookup by id. */
class Nodes
{
public:
    using Node = AudioProcessorGraphMT::Node;
    using NodeID = AudioProcessorGraphMT::NodeID;

    const ReferenceCountedArray<Node>& getNodes() const
    {
        return array;
    }

    Node::Ptr getNodeForId(NodeID nodeID) const
    {
        const auto iter = std::lower_bound(array.begin(), array.end(), nodeID, ImplicitNode::compare);
        return iter != array.end() && (*iter)->nodeID == nodeID ? *iter : nullptr;
    }

    Node::Ptr addNode(std::unique_ptr<AudioProcessor> newProcessor, const NodeID nodeID)
    {
        if (newProcessor == nullptr)
        {
            // Cannot add a null audio processor!
            jassertfalse;
            return {};
        }

        if (std::any_of(array.begin(), array.end(), [&](auto* n) { return n->getProcessor() == newProcessor.get(); }))
        {
            // This audio processor has already been added to the graph!
            jassertfalse;
            return {};
        }

        const auto iter = std::lower_bound(array.begin(), array.end(), nodeID, ImplicitNode::compare);

        if (iter != array.end() && (*iter)->nodeID == nodeID)
        {
            // This nodeID has already been used for a node in the graph!
            jassertfalse;
            return {};
        }

        return array.insert((int)std::distance(array.begin(), iter), new Node{nodeID, std::move(newProcessor)});
    }

    Node::Ptr removeNode(NodeID nodeID)
    {
        const auto iter = std::lower_bound(array.begin(), array.end(), nodeID, ImplicitNode::compare);
        return iter != array.end() && (*iter)->nodeID == nodeID
                 ? array.removeAndReturn((int)std::distance(array.begin(), iter))
                 : nullptr;
    }

    bool operator==(const Nodes& other) const
    {
        return array == other.array;
    }

    bool operator!=(const Nodes& other) const
    {
        return array != other.array;
    }

private:
    ReferenceCountedArray<Node> array;
};

//==============================================================================
/*  A value type holding a full set of graph connections. */
class Connections
{
public:
    using Node = AudioProcessorGraphMT::Node;
    using NodeID = AudioProcessorGraphMT::NodeID;
    using Connection = AudioProcessorGraphMT::Connection;
    using NodeAndChannel = AudioProcessorGraphMT::NodeAndChannel;

private:
    static auto equalRange(const std::set<NodeAndChannel>& pins, const NodeID node)
    {
        return std::equal_range(pins.cbegin(), pins.cend(), node, ImplicitNode::compare);
    }

    using Map = std::map<NodeAndChannel, std::set<NodeAndChannel>>;

public:
    static constexpr auto midiChannelIndex = AudioProcessorGraphMT::midiChannelIndex;

    bool addConnection(const Nodes& n, const Connection& c)
    {
        String msg = "Connections::addConnection: src="
                   + String((int)c.source.nodeID.uid)
                   + "."
                   + String(c.source.channelIndex)
                   + " -> dst="
                   + String((int)c.destination.nodeID.uid)
                   + "."
                   + String(c.destination.channelIndex);
        DBG(msg);
        Logger::writeToLog(msg);

        if (!canConnect(n, c))
        {
            DBG("  canConnect returned FALSE");
            Logger::writeToLog("  canConnect returned FALSE");
            return false;
        }

        sourcesForDestination[c.destination].insert(c.source);

        String countMsg = "  Connection added. Total connections: " + String(getConnections().size());
        DBG(countMsg);
        Logger::writeToLog(countMsg);
        jassert(isConnected(c));
        return true;
    }

    bool removeConnection(const Connection& c)
    {
        const auto iter = sourcesForDestination.find(c.destination);
        return iter != sourcesForDestination.cend() && iter->second.erase(c.source) == 1;
    }

    bool removeIllegalConnections(const Nodes& n)
    {
        auto anyRemoved = false;

        for (auto& dest : sourcesForDestination)
        {
            const auto initialSize = dest.second.size();
            dest.second = removeIllegalConnections(n, std::move(dest.second), dest.first);
            anyRemoved |= (dest.second.size() != initialSize);
        }

        return anyRemoved;
    }

    bool disconnectNode(NodeID n)
    {
        const auto matchingDestinations = getMatchingDestinations(n);
        auto result = matchingDestinations.first != matchingDestinations.second;
        sourcesForDestination.erase(matchingDestinations.first, matchingDestinations.second);

        for (auto& pair : sourcesForDestination)
        {
            const auto range = equalRange(pair.second, n);
            result |= range.first != range.second;
            pair.second.erase(range.first, range.second);
        }

        return result;
    }

    static bool isConnectionLegal(const Nodes& n, Connection c)
    {
        const auto source = n.getNodeForId(c.source.nodeID);
        const auto dest = n.getNodeForId(c.destination.nodeID);

        const auto sourceChannel = c.source.channelIndex;
        const auto destChannel = c.destination.channelIndex;

        const auto sourceIsMIDI = AudioProcessorGraphMT::midiChannelIndex == sourceChannel;
        const auto destIsMIDI = AudioProcessorGraphMT::midiChannelIndex == destChannel;

        return sourceChannel >= 0
            && destChannel >= 0
            && source != dest
            && sourceIsMIDI == destIsMIDI
            && source != nullptr
            && (sourceIsMIDI ? source->getProcessor()->producesMidi()
                             : sourceChannel < source->getProcessor()->getTotalNumOutputChannels())
            && dest != nullptr
            && (destIsMIDI ? dest->getProcessor()->acceptsMidi()
                           : destChannel < dest->getProcessor()->getTotalNumInputChannels());
    }

    bool canConnect(const Nodes& n, Connection c) const
    {
        return isConnectionLegal(n, c) && !isConnected(c);
    }

    bool isConnected(Connection c) const
    {
        const auto iter = sourcesForDestination.find(c.destination);

        return iter != sourcesForDestination.cend() && iter->second.find(c.source) != iter->second.cend();
    }

    bool isConnected(NodeID srcID, NodeID destID) const
    {
        const auto matchingDestinations = getMatchingDestinations(destID);

        return std::any_of(
            matchingDestinations.first,
            matchingDestinations.second,
            [srcID](const auto& pair)
            {
                const auto [begin, end] = equalRange(pair.second, srcID);
                return begin != end;
            }
        );
    }

    std::set<NodeID> getSourceNodesForDestination(NodeID destID) const
    {
        const auto matchingDestinations = getMatchingDestinations(destID);

        std::set<NodeID> result;
        std::for_each(
            matchingDestinations.first,
            matchingDestinations.second,
            [&](const auto& pair)
            {
                for (const auto& source : pair.second)
                    result.insert(source.nodeID);
            }
        );
        return result;
    }

    std::set<NodeAndChannel> getSourcesForDestination(const NodeAndChannel& p) const
    {
        const auto iter = sourcesForDestination.find(p);
        return iter != sourcesForDestination.cend() ? iter->second : std::set<NodeAndChannel>{};
    }

    std::vector<Connection> getConnections() const
    {
        std::vector<Connection> result;

        for (auto& pair : sourcesForDestination)
            for (const auto& source : pair.second)
                result.emplace_back(source, pair.first);

        std::sort(result.begin(), result.end());
        result.erase(std::unique(result.begin(), result.end()), result.end());
        return result;
    }

    bool isAnInputTo(NodeID source, NodeID dest) const
    {
        return getConnectedRecursive(source, dest, {}).found;
    }

    bool operator==(const Connections& other) const
    {
        return sourcesForDestination == other.sourcesForDestination;
    }

    bool operator!=(const Connections& other) const
    {
        return sourcesForDestination != other.sourcesForDestination;
    }

    class DestinationsForSources
    {
    public:
        explicit DestinationsForSources(Map m)
            : map(std::move(m))
        {
        }

        bool
        isSourceConnectedToDestinationNodeIgnoringChannel(const NodeAndChannel& source, NodeID dest, int channel) const
        {
            if (const auto destIter = map.find(source); destIter != map.cend())
            {
                const auto [begin, end] = equalRange(destIter->second, dest);
                return std::any_of(
                    begin,
                    end,
                    [&](const NodeAndChannel& nodeAndChannel)
                    { return nodeAndChannel != NodeAndChannel{dest, channel}; }
                );
            }

            return false;
        }

    private:
        Map map;
    };

    /*  Reverses the graph, to allow fast lookup by source.
        This is expensive, don't call this more than necessary!
    */
    auto getDestinationsForSources() const
    {
        Map destinationsForSources;

        for (const auto& [destination, sources] : sourcesForDestination)
            for (const auto& source : sources)
                destinationsForSources[source].insert(destination);

        return DestinationsForSources(std::move(destinationsForSources));
    }

private:
    struct SearchState
    {
        std::set<NodeID> visited;
        bool found = false;
    };

    SearchState getConnectedRecursive(NodeID source, NodeID dest, SearchState state) const
    {
        state.visited.insert(dest);

        for (const auto& s : getSourceNodesForDestination(dest))
        {
            if (state.found || s == source)
                return {std::move(state.visited), true};

            if (state.visited.find(s) == state.visited.cend())
                state = getConnectedRecursive(source, s, std::move(state));
        }

        return state;
    }

    static std::set<NodeAndChannel>
    removeIllegalConnections(const Nodes& nodes, std::set<NodeAndChannel> sources, NodeAndChannel destination)
    {
        for (auto source = sources.cbegin(); source != sources.cend();)
            if (!isConnectionLegal(nodes, {*source, destination}))
                source = sources.erase(source);
            else
                ++source;

        return sources;
    }

    std::pair<Map::const_iterator, Map::const_iterator> getMatchingDestinations(NodeID destID) const
    {
        return std::equal_range(
            sourcesForDestination.cbegin(),
            sourcesForDestination.cend(),
            destID,
            ImplicitNode::compare
        );
    }

    Map sourcesForDestination;
};

//==============================================================================
/*  Settings used to prepare a node for playback. */
struct PrepareSettings
{
    double sampleRate = 0.0;
    int blockSize = 0;

    auto tie() const noexcept
    {
        return std::tie(sampleRate, blockSize);
    }

    bool operator==(const PrepareSettings& other) const
    {
        return tie() == other.tie();
    }

    bool operator!=(const PrepareSettings& other) const
    {
        return tie() != other.tie();
    }
};

//==============================================================================
/*  Keeps track of the PrepareSettings applied to each node. */
class NodeStates
{
public:
    using Node = AudioProcessorGraphMT::Node;
    using NodeID = AudioProcessorGraphMT::NodeID;

    /*  Called from prepareToPlay and releaseResources with the PrepareSettings that should be
        used next time the graph is rebuilt.
    */
    void setState(std::optional<PrepareSettings> newSettings)
    {
        const std::lock_guard<std::mutex> lock(mutex);
        next = newSettings;
    }

    /*  Call from the audio thread only. */
    std::optional<PrepareSettings> getLastRequestedSettings() const
    {
        return next;
    }

    /*  Call from the main thread only!

        Called after updating the graph topology to prepare any currently-unprepared nodes.

        To ensure that all nodes are initialised with the same sample rate, buffer size, etc. as
        the enclosing graph, we must ensure that any operation that uses these details (preparing
        individual nodes) is synchronized with prepare-to-play and release-resources on the
        enclosing graph.

        If the new PrepareSettings are different to the last-seen settings, all nodes will
        be prepared/unprepared as necessary. If the PrepareSettings have not changed, then only
        new nodes will be prepared/unprepared.

        Returns the settings that were applied to the nodes.
    */
    std::optional<PrepareSettings> applySettings(const Nodes& n)
    {
        const auto settingsChanged = [this]
        {
            const std::lock_guard<std::mutex> lock(mutex);
            const auto result = current != next;
            current = next;
            return result;
        }();

        // It may look like releaseResources and prepareToPlay could race with calls to processBlock
        // here, because applySettings is called from the main thread, processBlock is called from
        // the audio thread (normally), and there's no explicit mutex ensuring that the calls don't
        // overlap.
        // However, it is part of the AudioProcessor contract that users shall not call
        // processBlock, prepareToPlay, and/or releaseResources concurrently. That is, there's an
        // implied mutex synchronising these functions on each AudioProcessor.
        //
        // Inside processBlock, we always ensure that the current RenderSequence's PrepareSettings
        // match the graph's settings before attempting to call processBlock on any of the graph
        // nodes; as a result, it's impossible to start calling processBlock on a node on the audio
        // thread while a render sequence rebuild (including prepareToPlay/releaseResources calls)
        // is already in progress here.
        //
        // Due to the implied mutex between prepareToPlay/releaseResources/processBlock, it's also
        // impossible to receive new PrepareSettings and to start a new RenderSequence rebuild while
        // a processBlock call is in progress.

        if (settingsChanged)
        {
            for (const auto& node : n.getNodes())
                node->getProcessor()->releaseResources();

            preparedNodes.clear();
        }

        if (current.has_value())
        {
            for (const auto& node : n.getNodes())
            {
                if (preparedNodes.find(node->nodeID) != preparedNodes.cend())
                    continue;

                preparedNodes.insert(node->nodeID);

                auto* nodeProcessor = node->getProcessor();
                nodeProcessor->setProcessingPrecision(AudioProcessor::singlePrecision);
                nodeProcessor->setRateAndBufferSizeDetails(current->sampleRate, current->blockSize);
                nodeProcessor->prepareToPlay(current->sampleRate, current->blockSize);
            }
        }

        return current;
    }

    /*  Call from the main thread to indicate that a node has been removed from the graph.
     */
    void removeNode(const NodeID n)
    {
        preparedNodes.erase(n);
    }

    /*  Call from the main thread to indicate that all nodes have been removed from the graph.
     */
    void clear()
    {
        preparedNodes.clear();
    }

private:
    std::mutex mutex;
    std::set<NodeID> preparedNodes;
    std::optional<PrepareSettings> current, next;
};

//==============================================================================
struct GraphRenderSequence
{
    using Node = AudioProcessorGraphMT::Node;

    struct GlobalIO
    {
        AudioBuffer<float>& audioIn;
        AudioBuffer<float>& audioOut;
        MidiBuffer& midiIn;
        MidiBuffer& midiOut;
    };

    struct Context
    {
        GlobalIO globalIO;
        AudioPlayHead* audioPlayHead;
        int numSamples;
    };

    void perform(AudioBuffer<float>& buffer, MidiBuffer& midiMessages, AudioPlayHead* audioPlayHead)
    {
        auto numSamples = buffer.getNumSamples();
        auto maxSamples = maxBlockSize;

        if (numSamples > maxSamples)
        {
            // Being asked to render more samples than our buffers have, so divide the buffer into chunks
            int chunkStartSample = 0;
            while (chunkStartSample < numSamples)
            {
                auto chunkSize = jmin(maxSamples, numSamples - chunkStartSample);

                AudioBuffer<float>
                    audioChunk(buffer.getArrayOfWritePointers(), buffer.getNumChannels(), chunkStartSample, chunkSize);
                midiChunk.clear();
                midiChunk.addEvents(midiMessages, chunkStartSample, chunkSize, -chunkStartSample);

                // Splitting up the buffer like this will cause the play head and host time to be
                // invalid for all but the first chunk...
                perform(audioChunk, midiChunk, audioPlayHead);

                chunkStartSample += maxSamples;
            }

            return;
        }

        currentAudioOutputBuffer.setSize(jmax(1, buffer.getNumChannels()), numSamples, false, false, true);
        currentAudioOutputBuffer.clear();
        currentMidiOutputBuffer.clear();

        // For chains with external buffers: nodes process directly on the chain's MIDI buffer (midiMessages parameter)
        // For full graph: nodes use internal midiBuffers array
        // This mimics how audio works: external buffer for chains, internal buffers for full graph
        MidiBuffer* midiBuffersToUse = (midiBuffers.size() == 1) ? &midiMessages : midiBuffers.data();

        // Prepare all RenderOps with buffer pointers for this process cycle
        // As long as the buffer doesn't resize (which we ensure), these pointers remain stable
        auto writePointers = buffer.getArrayOfWritePointers();
        for (const auto& op : renderOps)
            op->prepare(writePointers, midiBuffersToUse);

        // Process directly on the input buffer (which is the pooled buffer for chains)
        const Context context{
            {buffer, currentAudioOutputBuffer, midiMessages, currentMidiOutputBuffer},
            audioPlayHead,
            numSamples
        };

        for (size_t opIndex = 0; opIndex < renderOps.size(); ++opIndex)
            renderOps[opIndex]->process(context);

        // MIDI output: For chains with external buffer (midiBuffers.size() == 1), nodes modified midiMessages in-place
        // For full graph with internal buffers, copy MIDI output back
        if (midiBuffers.size() > 1)
        {
            midiMessages.clear();
            midiMessages.addEvents(currentMidiOutputBuffer, 0, buffer.getNumSamples(), 0);
        }
    }

    void addClearChannelOp(int index)
    {
        struct ClearOp final : public RenderOp
        {
            explicit ClearOp(int indexIn)
                : index(indexIn)
            {
            }

            void prepare(float* const* renderBuffer, MidiBuffer*) override
            {
                channelBuffer = renderBuffer[index];
            }

            void process(const Context& c) override
            {
                FloatVectorOperations::clear(channelBuffer, c.numSamples);
            }

            float* channelBuffer = nullptr;
            int index = 0;
        };

        renderOps.push_back(std::make_unique<ClearOp>(index));
    }

    void addCopyChannelOp(int srcIndex, int dstIndex)
    {
        struct CopyOp final : public RenderOp
        {
            explicit CopyOp(int fromIn, int toIn)
                : from(fromIn)
                , to(toIn)
            {
            }

            void prepare(float* const* renderBuffer, MidiBuffer*) override
            {
                fromBuffer = renderBuffer[from];
                toBuffer = renderBuffer[to];
            }

            void process(const Context& c) override
            {
                FloatVectorOperations::copy(toBuffer, fromBuffer, c.numSamples);
            }

            float* fromBuffer = nullptr;
            float* toBuffer = nullptr;
            int from = 0, to = 0;
        };

        renderOps.push_back(std::make_unique<CopyOp>(srcIndex, dstIndex));
    }

    void addAddChannelOp(int srcIndex, int dstIndex)
    {
        struct AddOp final : public RenderOp
        {
            explicit AddOp(int fromIn, int toIn)
                : from(fromIn)
                , to(toIn)
            {
            }

            void prepare(float* const* renderBuffer, MidiBuffer*) override
            {
                fromBuffer = renderBuffer[from];
                toBuffer = renderBuffer[to];
            }

            void process(const Context& c) override
            {
                FloatVectorOperations::add(toBuffer, fromBuffer, c.numSamples);
            }

            float* fromBuffer = nullptr;
            float* toBuffer = nullptr;
            int from = 0, to = 0;
        };

        renderOps.push_back(std::make_unique<AddOp>(srcIndex, dstIndex));
    }

    void addClearMidiBufferOp(int index)
    {
        struct ClearOp final : public RenderOp
        {
            explicit ClearOp(int indexIn)
                : index(indexIn)
            {
            }

            void prepare(float* const*, MidiBuffer* buffers) override
            {
                channelBuffer = buffers + index;
            }

            void process(const Context&) override
            {
                channelBuffer->clear();
            }

            MidiBuffer* channelBuffer = nullptr;
            int index = 0;
        };

        renderOps.push_back(std::make_unique<ClearOp>(index));
    }

    void addCopyMidiBufferOp(int srcIndex, int dstIndex)
    {
        struct CopyOp final : public RenderOp
        {
            explicit CopyOp(int fromIn, int toIn)
                : from(fromIn)
                , to(toIn)
            {
            }

            void prepare(float* const*, MidiBuffer* buffers) override
            {
                fromBuffer = buffers + from;
                toBuffer = buffers + to;
            }

            void process(const Context&) override
            {
                *toBuffer = *fromBuffer;
            }

            MidiBuffer* fromBuffer = nullptr;
            MidiBuffer* toBuffer = nullptr;
            int from = 0, to = 0;
        };

        renderOps.push_back(std::make_unique<CopyOp>(srcIndex, dstIndex));
    }

    void addAddMidiBufferOp(int srcIndex, int dstIndex)
    {
        struct AddOp final : public RenderOp
        {
            explicit AddOp(int fromIn, int toIn)
                : from(fromIn)
                , to(toIn)
            {
            }

            void prepare(float* const*, MidiBuffer* buffers) override
            {
                fromBuffer = buffers + from;
                toBuffer = buffers + to;
            }

            void process(const Context& c) override
            {
                toBuffer->addEvents(*fromBuffer, 0, c.numSamples, 0);
            }

            MidiBuffer* fromBuffer = nullptr;
            MidiBuffer* toBuffer = nullptr;
            int from = 0, to = 0;
        };

        renderOps.push_back(std::make_unique<AddOp>(srcIndex, dstIndex));
    }

    void addDelayChannelOp(int chan, int delaySize)
    {
        struct DelayChannelOp final : public RenderOp
        {
            DelayChannelOp(int chan, int delaySize)
                : buffer((size_t)(delaySize + 1), 0.0f)
                , channel(chan)
                , writeIndex(delaySize)
            {
            }

            const char* getOpName() const override
            {
                return "DelayChannelOp";
            }

            void prepare(float* const* renderBuffer, MidiBuffer*) override
            {
                channelBuffer = renderBuffer[channel];
            }

            void process(const Context& c) override
            {
                auto* data = channelBuffer;

                for (int i = c.numSamples; --i >= 0;)
                {
                    buffer[(size_t)writeIndex] = *data;
                    *data++ = buffer[(size_t)readIndex];

                    if (++readIndex >= (int)buffer.size())
                        readIndex = 0;
                    if (++writeIndex >= (int)buffer.size())
                        writeIndex = 0;
                }
            }

            std::vector<float> buffer;
            float* channelBuffer = nullptr;
            const int channel;
            int readIndex = 0, writeIndex;
        };

        renderOps.push_back(std::make_unique<DelayChannelOp>(chan, delaySize));
    }

    void addProcessOp(const Node::Ptr& node, const Array<int>& audioChannelsUsed, int totalNumChans, int midiBuffer)
    {
        // I/O nodes are now skipped during createOrderedNodeList, so this should never receive them.
        // We handle input/output externally in perform() by pre-copying and post-copying buffers.
        jassert(dynamic_cast<const AudioProcessorGraphMT::AudioGraphIOProcessor*>(node->getProcessor()) == nullptr);

        auto op = [&]() -> std::unique_ptr<NodeOp>
        {
            return std::make_unique<ProcessOp>(
                node,
                audioChannelsUsed,
                totalNumChans,
                midiBuffer,
                *precisionConversionBuffer
            );
        }();

        renderOps.push_back(std::move(op));
    }

    void prepareBuffers(int blockSize, AudioBuffer<float>* externalBuffer = nullptr)
    {
        maxBlockSize = blockSize;

        currentAudioOutputBuffer.setSize(numBuffersNeeded + 1, blockSize, false, false, true);
        currentAudioOutputBuffer.clear();

        precisionConversionBuffer->setSize(numBuffersNeeded, blockSize, false, false, true);

        currentMidiOutputBuffer.clear();

        midiBuffers.clearQuick();
        midiBuffers.resize(numMidiBuffersNeeded);

        const int defaultMIDIBufferSize = 512;

        midiChunk.ensureSize(defaultMIDIBufferSize);

        for (auto&& m : midiBuffers)
            m.ensureSize(defaultMIDIBufferSize);

        // If external buffer provided, prepare all RenderOps immediately
        if (externalBuffer != nullptr)
            for (const auto& op : renderOps)
                op->prepare(externalBuffer->getArrayOfWritePointers(), midiBuffers.data());
    }

    int numBuffersNeeded = 0, numMidiBuffersNeeded = 0;
    int maxBlockSize = 0;

    AudioBuffer<float> currentAudioOutputBuffer;

    MidiBuffer currentMidiOutputBuffer;

    Array<MidiBuffer> midiBuffers;
    MidiBuffer midiChunk;

private:
    //==============================================================================
    struct RenderOp
    {
        virtual ~RenderOp() = default;
        virtual void prepare(float* const*, MidiBuffer*) = 0;
        virtual void process(const Context&) = 0;

        virtual const char* getOpName() const
        {
            return "RenderOp";
        }
    };

    struct NodeOp : public RenderOp
    {
        NodeOp(const Node::Ptr& n, const Array<int>& audioChannelsUsed, int totalNumChans, int midiBufferIndex)
            : node(n)
            , processor(*n->getProcessor())
            , audioChannelsToUse(audioChannelsUsed)
            , totalChannels(totalNumChans)
            , audioChannels((size_t)totalNumChans)
            , midiBufferToUse(midiBufferIndex)
        {
            while (audioChannelsToUse.size() < totalChannels)
                audioChannelsToUse.add(0);
        }

        const char* getOpName() const override
        {
            return "NodeOp";
        }

        void prepare(float* const* renderBuffer, MidiBuffer* buffers) final
        {
            // Store fresh pointers from the render buffer for this process cycle
            jassert(renderBuffer != nullptr);

            for (size_t i = 0; i < audioChannels.size(); ++i)
            {
                int channelIndex = audioChannelsToUse.getUnchecked((int)i);
                audioChannels[i] = renderBuffer[channelIndex];
            }

            midiBuffer = buffers + midiBufferToUse;
        }

        void process(const Context& c) final
        {
            processor.setPlayHead(c.audioPlayHead);

            auto numAudioChannels = [this]
            {
                if (const auto* proc = node->getProcessor())
                    if (proc->getTotalNumInputChannels() == 0 && proc->getTotalNumOutputChannels() == 0)
                        return 0;

                return totalChannels;
            }();

            AudioBuffer<float> buffer{audioChannels.data(), numAudioChannels, c.numSamples};

            if (processor.isSuspended())
            {
                buffer.clear();
            }
            else
            {
                const auto bypass = node->isBypassed() && processor.getBypassParameter() == nullptr;
                processWithBuffer(c.globalIO, bypass, buffer, *midiBuffer);
            }
        }

        virtual void processWithBuffer(const GlobalIO&, bool bypass, AudioBuffer<float>& audio, MidiBuffer& midi) = 0;

        const Node::Ptr node;
        AudioProcessor& processor;
        MidiBuffer* midiBuffer = nullptr;

        Array<int> audioChannelsToUse;
        const int totalChannels;
        std::vector<float*> audioChannels;
        const int midiBufferToUse;
    };

    struct ProcessOp final : public NodeOp
    {
        ProcessOp(
            const Node::Ptr& n,
            const Array<int>& audioChannelsUsed,
            int totalNumChans,
            int midiBufferIndex,
            AudioBuffer<float>& tempBuffer
        )
            : NodeOp(n, audioChannelsUsed, totalNumChans, midiBufferIndex)
            , temporaryBuffer(tempBuffer)
        {
        }

        void processWithBuffer(const GlobalIO&, bool bypass, AudioBuffer<float>& audio, MidiBuffer& midi) final
        {
            const ScopedLock lock{this->processor.getCallbackLock()};

            if (this->processor.isUsingDoublePrecision())
            {
                // The graph is processing in single-precision, but this node is expecting a
                // double-precision buffer. All nodes should be set to single-precision.
                jassertfalse;
                audio.clear();
                midi.clear();
            }
            else
            {
                if (bypass)
                    this->processor.processBlockBypassed(audio, midi);
                else
                    this->processor.processBlock(audio, midi);
            }
        }

        AudioBuffer<float>& temporaryBuffer;
    };

    /*  I/O Operations are no longer used in rendering pipeline.
        Input/output is handled externally in GraphRenderSequence::perform()
        by processing directly on the external pooled buffer.

        This eliminates the need for special I/O node processing and allows us to:
        - Handle all I/O externally in a consistent manner
        - Simplify parallel processing (no special cases for I/O nodes)
        - Support filtered subgraphs without I/O nodes seamlessly

    struct MidiInOp final : public NodeOp
    {
        using NodeOp::NodeOp;

        void processWithBuffer(const GlobalIO& g, bool bypass, AudioBuffer<FloatType>& audio, MidiBuffer& midi) final
        {
            if (!bypass)
                midi.addEvents(g.midiIn, 0, audio.getNumSamples(), 0);
        }
    };

    struct MidiOutOp final : public NodeOp
    {
        using NodeOp::NodeOp;

        void processWithBuffer(const GlobalIO& g, bool bypass, AudioBuffer<FloatType>& audio, MidiBuffer& midi) final
        {
            if (!bypass)
                g.midiOut.addEvents(midi, 0, audio.getNumSamples(), 0);
        }
    };

    struct AudioInOp final : public NodeOp
    {
        using NodeOp::NodeOp;

        void processWithBuffer(const GlobalIO& g, bool bypass, AudioBuffer<FloatType>& audio, MidiBuffer&) final
        {
            if (bypass)
                return;

            for (int i = jmin(g.audioIn.getNumChannels(), audio.getNumChannels()); --i >= 0;)
                audio.copyFrom(i, 0, g.audioIn, i, 0, audio.getNumSamples());
        }
    };

    struct AudioOutOp final : public NodeOp
    {
        using NodeOp::NodeOp;

        void processWithBuffer(const GlobalIO& g, bool bypass, AudioBuffer<FloatType>& audio, MidiBuffer&) final
        {
            if (bypass)
                return;

            for (int i = jmin(g.audioOut.getNumChannels(), audio.getNumChannels()); --i >= 0;)
                g.audioOut.addFrom(i, 0, audio, i, 0, audio.getNumSamples());
        }
    };
    */

    std::vector<std::unique_ptr<RenderOp>> renderOps;

    std::unique_ptr<AudioBuffer<float>> precisionConversionBuffer = std::make_unique<AudioBuffer<float>>();
};

//==============================================================================
struct SequenceAndLatency
{
    GraphRenderSequence sequence;
    int latencySamples = 0;
};

//==============================================================================
class RenderSequenceBuilder
{
public:
    using Node = AudioProcessorGraphMT::Node;
    using NodeID = AudioProcessorGraphMT::NodeID;
    using Connection = AudioProcessorGraphMT::Connection;
    using NodeAndChannel = AudioProcessorGraphMT::NodeAndChannel;

    static constexpr auto midiChannelIndex = AudioProcessorGraphMT::midiChannelIndex;

    // Calculate latency for all nodes in the graph (recursive, with memoization)
    // Returns a map of nodeID -> latency samples
    static std::unordered_map<uint32, int> calculateGlobalDelays(const Nodes& n, const Connections& c)
    {
        std::unordered_map<uint32, int> delays;
        const auto orderedNodes = createOrderedNodeList(n, c);

        // Process nodes in topological order so dependencies are already calculated
        for (const auto* node : orderedNodes)
        {
            // Find max latency among all input sources
            int maxInputLatency = 0;
            const auto sources = c.getSourceNodesForDestination(node->nodeID);
            for (const auto& sourceNodeID : sources)
            {
                auto it = delays.find(sourceNodeID.uid);
                int sourceLatency = (it != delays.end()) ? it->second : 0;
                maxInputLatency = jmax(maxInputLatency, sourceLatency);
            }

            // This node's latency = max input latency + processor's own latency
            const int thisNodeLatency = maxInputLatency + node->getProcessor()->getLatencySamples();
            delays[node->nodeID.uid] = thisNodeLatency;
        }

        return delays;
    }

    static SequenceAndLatency build(const Nodes& n, const Connections& c)
    {
        GraphRenderSequence sequence;
        const RenderSequenceBuilder builder(n, c, sequence);
        return {std::move(sequence), builder.totalLatency};
    }

    // Filtered build: only process nodes in the given set, with pre-computed global delays
    static SequenceAndLatency buildFiltered(
        const Nodes& n,
        const Connections& c,
        const std::set<NodeID>& nodeFilter,
        const std::unordered_map<uint32, int>& globalDelays
    )
    {
        GraphRenderSequence sequence;
        const RenderSequenceBuilder builder(n, c, sequence, nodeFilter, globalDelays);
        return {std::move(sequence), builder.totalLatency};
    }

private:
    //==============================================================================
    const Array<Node*> orderedNodes;

    struct AssignedBuffer
    {
        NodeAndChannel channel;

        static constexpr AssignedBuffer createReadOnlyEmpty() noexcept
        {
            return {
                {zeroNodeID, 0}
            };
        }

        static constexpr AssignedBuffer createFree() noexcept
        {
            return {
                {freeNodeID, 0}
            };
        }

        constexpr bool isReadOnlyEmpty() const noexcept
        {
            return channel.nodeID == zeroNodeID;
        }

        constexpr bool isFree() const noexcept
        {
            return channel.nodeID == freeNodeID;
        }

        constexpr bool isAssigned() const noexcept
        {
            return !(isReadOnlyEmpty() || isFree());
        }

        constexpr void setFree() noexcept
        {
            channel = {freeNodeID, 0};
        }

        constexpr void setAssignedToNonExistentNode() noexcept
        {
            channel = {anonNodeID, 0};
        }

    private:
        constexpr static inline NodeID anonNodeID{0x7ffffffd};
        constexpr static inline NodeID zeroNodeID{0x7ffffffe};
        constexpr static inline NodeID freeNodeID{0x7fffffff};
    };

    Array<AssignedBuffer> audioBuffers, midiBuffers;

    enum
    {
        readOnlyEmptyBufferIndex = 0
    };

    std::unordered_map<uint32, int> delays;
    int totalLatency = 0;

    int getNodeDelay(NodeID nodeID) const noexcept
    {
        const auto iter = delays.find(nodeID.uid);
        return iter != delays.end() ? iter->second : 0;
    }

    int getInputLatencyForNode(const Connections& c, NodeID nodeID) const
    {
        const auto sources = c.getSourceNodesForDestination(nodeID);
        int maxLatency = std::accumulate(
            sources.cbegin(),
            sources.cend(),
            0,
            [this](auto acc, auto source) { return jmax(acc, this->getNodeDelay(source)); }
        );

        return maxLatency;
    }

    //==============================================================================
    static void getAllParentsOfNode(
        const NodeID& child,
        std::set<NodeID>& parents,
        const std::map<NodeID, std::set<NodeID>>& otherParents,
        const Connections& c
    )
    {
        for (const auto& parentNode : c.getSourceNodesForDestination(child))
        {
            if (parentNode == child)
                continue;

            if (parents.insert(parentNode).second)
            {
                const auto parentParents = otherParents.find(parentNode);

                if (parentParents != otherParents.end())
                {
                    parents.insert(parentParents->second.begin(), parentParents->second.end());
                    continue;
                }

                getAllParentsOfNode(parentNode, parents, otherParents, c);
            }
        }
    }

    static Array<Node*> createOrderedNodeList(const Nodes& n, const Connections& c)
    {
        return createOrderedNodeList(n, c, nullptr);
    }

    static Array<Node*> createOrderedNodeList(const Nodes& n, const Connections& c, const std::set<NodeID>* nodeFilter)
    {
        Array<Node*> result;

        std::map<NodeID, std::set<NodeID>> nodeParents;

        for (auto& node : n.getNodes())
        {
            const auto nodeID = node->nodeID;

            // Skip I/O nodes - we handle input/output externally in perform()
            if (auto* ioNode = dynamic_cast<AudioProcessorGraphMT::AudioGraphIOProcessor*>(node->getProcessor()))
                continue;

            // Skip nodes not in filter (if filter is provided)
            if (nodeFilter && nodeFilter->find(nodeID) == nodeFilter->end())
                continue;

            int insertionIndex = 0;

            for (; insertionIndex < result.size(); ++insertionIndex)
            {
                auto& parents = nodeParents[result.getUnchecked(insertionIndex)->nodeID];

                if (parents.find(nodeID) != parents.end())
                    break;
            }

            result.insert(insertionIndex, node);
            getAllParentsOfNode(nodeID, nodeParents[node->nodeID], nodeParents, c);
        }

        return result;
    }

    //==============================================================================
    int findBufferForInputAudioChannel(
        const Connections& c,
        const Connections::DestinationsForSources& reversed,
        GraphRenderSequence& sequence,
        Node& node,
        const int inputChan,
        const int ourRenderingIndex,
        const int maxLatency
    )
    {
        auto& processor = *node.getProcessor();
        auto numOuts = processor.getTotalNumOutputChannels();

        auto sources = c.getSourcesForDestination({node.nodeID, inputChan});

        // Handle an unconnected input channel...
        if (sources.empty())
        {
            if (inputChan >= numOuts)
                return readOnlyEmptyBufferIndex;

            auto index = getFreeBuffer(audioBuffers);
            sequence.addClearChannelOp(index);

            return index;
        }

        // Handle an input from a single source..
        if (sources.size() == 1)
        {
            // channel with a straightforward single input..
            auto src = *sources.begin();

            int bufIndex = getBufferContaining(src);

            if (bufIndex < 0)
            {
                // If not found, this is probably a feedback loop
                bufIndex = readOnlyEmptyBufferIndex;
                jassert(bufIndex >= 0);
            }

            const auto nodeDelay = getNodeDelay(src.nodeID);
            const auto needsDelay = nodeDelay < maxLatency;

            DBG("[LATENCY]   Input source: nodeID="
                << (int)src.nodeID.uid
                << " nodeDelay="
                << nodeDelay
                << " maxLatency="
                << maxLatency
                << " needsDelay="
                << (needsDelay ? "YES" : "NO"));

            if ((inputChan < numOuts || needsDelay) && isBufferNeededLater(reversed, ourRenderingIndex, inputChan, src))
            {
                // We can't modify this channel because it's needed later by another node,
                // so we need to use a copy of it.
                // If the input channel index matches any output channel index, this implies that
                // the output would overwrite the content of the input buffer.
                // If the input needs to be delayed by some amount, this will modify the buffer
                // in-place which will produce the wrong delay if a subsequent input needs a
                // different delay value.
                auto newFreeBuffer = getFreeBuffer(audioBuffers);
                sequence.addCopyChannelOp(bufIndex, newFreeBuffer);
                bufIndex = newFreeBuffer;
            }

            if (needsDelay)
                sequence.addDelayChannelOp(bufIndex, maxLatency - nodeDelay);

            return bufIndex;
        }

        // Handle a mix of several outputs coming into this input..
        int reusableInputIndex = -1;
        int bufIndex = -1;

        {
            auto i = 0;
            for (const auto& src : sources)
            {
                auto sourceBufIndex = getBufferContaining(src);

                if (sourceBufIndex >= 0 && !isBufferNeededLater(reversed, ourRenderingIndex, inputChan, src))
                {
                    // we've found one of our input chans that can be re-used..
                    reusableInputIndex = i;
                    bufIndex = sourceBufIndex;

                    auto nodeDelay = getNodeDelay(src.nodeID);

                    if (nodeDelay < maxLatency)
                    {
                        auto delaySamples = maxLatency - nodeDelay;
                        sequence.addDelayChannelOp(bufIndex, delaySamples);
                    }

                    break;
                }

                ++i;
            }
        }

        if (reusableInputIndex < 0)
        {
            // can't re-use any of our input chans, so get a new one and copy everything into it..
            bufIndex = getFreeBuffer(audioBuffers);
            jassert(bufIndex != 0);

            audioBuffers.getReference(bufIndex).setAssignedToNonExistentNode();

            auto srcIndex = getBufferContaining(*sources.begin());

            if (srcIndex < 0)
                sequence.addClearChannelOp(bufIndex); // if not found, this is probably a feedback loop
            else
                sequence.addCopyChannelOp(srcIndex, bufIndex);

            reusableInputIndex = 0;
            auto nodeDelay = getNodeDelay(sources.begin()->nodeID);

            if (nodeDelay < maxLatency)
            {
                auto delaySamples = maxLatency - nodeDelay;
                sequence.addDelayChannelOp(bufIndex, delaySamples);
            }
        }

        {
            auto i = 0;
            for (const auto& src : sources)
            {
                if (i != reusableInputIndex)
                {
                    int srcIndex = getBufferContaining(src);

                    if (srcIndex >= 0)
                    {
                        auto nodeDelay = getNodeDelay(src.nodeID);

                        if (nodeDelay < maxLatency)
                        {
                            if (!isBufferNeededLater(reversed, ourRenderingIndex, inputChan, src))
                            {
                                sequence.addDelayChannelOp(srcIndex, maxLatency - nodeDelay);
                            }
                            else // buffer is reused elsewhere, can't be delayed
                            {
                                auto bufferToDelay = getFreeBuffer(audioBuffers);
                                sequence.addCopyChannelOp(srcIndex, bufferToDelay);
                                sequence.addDelayChannelOp(bufferToDelay, maxLatency - nodeDelay);
                                srcIndex = bufferToDelay;
                            }
                        }

                        sequence.addAddChannelOp(srcIndex, bufIndex);
                    }
                }

                ++i;
            }
        }

        return bufIndex;
    }

    int findBufferForInputMidiChannel(
        const Connections& c,
        const Connections::DestinationsForSources& reversed,
        GraphRenderSequence& sequence,
        Node& node,
        int ourRenderingIndex
    )
    {
        auto& processor = *node.getProcessor();
        auto sources = c.getSourcesForDestination({node.nodeID, midiChannelIndex});

        // No midi inputs..
        if (sources.empty())
        {
            auto midiBufferToUse =
                getFreeBuffer(midiBuffers); // need to pick a buffer even if the processor doesn't use midi

            if (processor.acceptsMidi() || processor.producesMidi())
                sequence.addClearMidiBufferOp(midiBufferToUse);

            return midiBufferToUse;
        }

        // One midi input..
        if (sources.size() == 1)
        {
            auto src = *sources.begin();
            auto midiBufferToUse = getBufferContaining(src);

            if (midiBufferToUse >= 0)
            {
                if (isBufferNeededLater(reversed, ourRenderingIndex, midiChannelIndex, src))
                {
                    // can't mess up this channel because it's needed later by another node, so we
                    // need to use a copy of it..
                    auto newFreeBuffer = getFreeBuffer(midiBuffers);
                    sequence.addCopyMidiBufferOp(midiBufferToUse, newFreeBuffer);
                    midiBufferToUse = newFreeBuffer;
                }
            }
            else
            {
                // probably a feedback loop, so just use an empty one..
                midiBufferToUse =
                    getFreeBuffer(midiBuffers); // need to pick a buffer even if the processor doesn't use midi
            }

            return midiBufferToUse;
        }

        // Multiple midi inputs..
        int midiBufferToUse = -1;
        int reusableInputIndex = -1;

        {
            auto i = 0;
            for (const auto& src : sources)
            {
                auto sourceBufIndex = getBufferContaining(src);

                if (sourceBufIndex >= 0 && !isBufferNeededLater(reversed, ourRenderingIndex, midiChannelIndex, src))
                {
                    // we've found one of our input buffers that can be re-used..
                    reusableInputIndex = i;
                    midiBufferToUse = sourceBufIndex;
                    break;
                }

                ++i;
            }
        }

        if (reusableInputIndex < 0)
        {
            // can't re-use any of our input buffers, so get a new one and copy everything into it..
            midiBufferToUse = getFreeBuffer(midiBuffers);
            jassert(midiBufferToUse >= 0);

            auto srcIndex = getBufferContaining(*sources.begin());

            if (srcIndex >= 0)
                sequence.addCopyMidiBufferOp(srcIndex, midiBufferToUse);
            else
                sequence.addClearMidiBufferOp(midiBufferToUse);

            reusableInputIndex = 0;
        }

        {
            auto i = 0;
            for (const auto& src : sources)
            {
                if (i != reusableInputIndex)
                {
                    auto srcIndex = getBufferContaining(src);

                    if (srcIndex >= 0)
                        sequence.addAddMidiBufferOp(srcIndex, midiBufferToUse);
                }

                ++i;
            }
        }

        return midiBufferToUse;
    }

    void createRenderingOpsForNode(
        const Connections& c,
        const Connections::DestinationsForSources& reversed,
        GraphRenderSequence& sequence,
        Node& node,
        const int ourRenderingIndex
    )
    {
        auto& processor = *node.getProcessor();
        auto numIns = processor.getTotalNumInputChannels();
        auto numOuts = processor.getTotalNumOutputChannels();
        auto totalChans = jmax(numIns, numOuts);

        Array<int> audioChannelsToUse;
        const auto maxInputLatency = getInputLatencyForNode(c, node.nodeID);

        for (int inputChan = 0; inputChan < numIns; ++inputChan)
        {
            // get a list of all the inputs to this node
            auto index = findBufferForInputAudioChannel(
                c,
                reversed,
                sequence,
                node,
                inputChan,
                ourRenderingIndex,
                maxInputLatency
            );
            jassert(index >= 0);

            audioChannelsToUse.add(index);

            if (inputChan < numOuts)
                audioBuffers.getReference(index).channel = {node.nodeID, inputChan};
        }

        for (int outputChan = numIns; outputChan < numOuts; ++outputChan)
        {
            auto index = getFreeBuffer(audioBuffers);
            jassert(index != 0);
            audioChannelsToUse.add(index);

            audioBuffers.getReference(index).channel = {node.nodeID, outputChan};
        }

        auto midiBufferToUse = findBufferForInputMidiChannel(c, reversed, sequence, node, ourRenderingIndex);

        if (processor.producesMidi())
            midiBuffers.getReference(midiBufferToUse).channel = {node.nodeID, midiChannelIndex};

        const auto thisNodeLatency = maxInputLatency + processor.getLatencySamples();
        delays[node.nodeID.uid] = thisNodeLatency;

        // For subgraphs, always track the maximum latency of all nodes processed.
        // The original JUCE code only tracked terminal nodes (numOuts == 0) for the full graph,
        // but for filtered sequences we need to know the max latency of the subgraph regardless
        // of whether nodes are terminal or not - this is used for delay compensation between levels.
        totalLatency = jmax(totalLatency, thisNodeLatency);

        sequence.addProcessOp(node, audioChannelsToUse, totalChans, midiBufferToUse);
    }

    //==============================================================================
    static int getFreeBuffer(Array<AssignedBuffer>& buffers)
    {
        for (int i = 1; i < buffers.size(); ++i)
            if (buffers.getReference(i).isFree())
                return i;

        buffers.add(AssignedBuffer::createFree());
        return buffers.size() - 1;
    }

    int getBufferContaining(NodeAndChannel output) const noexcept
    {
        int i = 0;

        for (auto& b : output.isMIDI() ? midiBuffers : audioBuffers)
        {
            if (b.channel == output)
                return i;

            ++i;
        }

        return -1;
    }

    void markAnyUnusedBuffersAsFree(
        const Connections::DestinationsForSources& c,
        Array<AssignedBuffer>& buffers,
        const int stepIndex
    )
    {
        for (auto& b : buffers)
            if (b.isAssigned() && !isBufferNeededLater(c, stepIndex, -1, b.channel))
                b.setFree();
    }

    bool isBufferNeededLater(
        const Connections::DestinationsForSources& c,
        const int stepIndexToSearchFrom,
        const int inputChannelOfIndexToIgnore,
        const NodeAndChannel output
    ) const
    {
        if (orderedNodes.size() <= stepIndexToSearchFrom)
            return false;

        if (c.isSourceConnectedToDestinationNodeIgnoringChannel(
                output,
                orderedNodes.getUnchecked(stepIndexToSearchFrom)->nodeID,
                inputChannelOfIndexToIgnore
            ))
        {
            return true;
        }

        return std::any_of(
            orderedNodes.begin() + stepIndexToSearchFrom + 1,
            orderedNodes.end(),
            [&](const auto* node)
            { return c.isSourceConnectedToDestinationNodeIgnoringChannel(output, node->nodeID, -1); }
        );
    }

    RenderSequenceBuilder(const Nodes& n, const Connections& c, GraphRenderSequence& sequence)
        : orderedNodes(createOrderedNodeList(n, c))
    {
        audioBuffers.add(AssignedBuffer::createReadOnlyEmpty()); // first buffer is read-only zeros
        midiBuffers.add(AssignedBuffer::createReadOnlyEmpty());

        const auto reversed = c.getDestinationsForSources();

        for (int i = 0; i < orderedNodes.size(); ++i)
        {
            createRenderingOpsForNode(c, reversed, sequence, *orderedNodes.getUnchecked(i), i);
            markAnyUnusedBuffersAsFree(reversed, audioBuffers, i);
            markAnyUnusedBuffersAsFree(reversed, midiBuffers, i);
        }

        sequence.numBuffersNeeded = audioBuffers.size();
        sequence.numMidiBuffersNeeded = midiBuffers.size();
    }

    // Filtered constructor for linear chains - simplified buffer management
    // Linear chains don't need complex buffer allocation: each node uses direct channel mapping (0→0, 1→1, etc.)
    RenderSequenceBuilder(
        const Nodes& n,
        const Connections& c,
        GraphRenderSequence& sequence,
        const std::set<NodeID>& nodeFilter,
        const std::unordered_map<uint32, int>& globalDelays
    )
        : orderedNodes(createOrderedNodeList(n, c, &nodeFilter))
    {
        // For linear chains, use simple sequential buffer allocation
        // No need for complex buffer reuse logic - each node uses its own channels

        // For cross-subgraph delay compensation:
        // - Use globalDelays to initialize delays map with accumulated latencies from OTHER subgraphs
        // - Within the subgraph, delays accumulate naturally as we process nodes in order
        delays = globalDelays;

        DBG("[LATENCY] === RenderSequenceBuilder (filtered) ===");
        DBG("[LATENCY] Building filtered sequence for " << orderedNodes.size() << " nodes");
        for (auto* node : orderedNodes)
        {
            DBG("[LATENCY]   Node in subgraph: "
                << node->getProcessor()->getName()
                << " (nodeID="
                << (int)node->nodeID.uid
                << ")");
        }
        for (const auto& [nodeId, delay] : globalDelays)
            DBG("[LATENCY]   GlobalDelay: nodeID=" << (int)nodeId << " delay=" << delay);

        int maxChannelsNeeded = 0;
        int maxMidiBuffersNeeded = 1; // At least one MIDI buffer

        // First pass: determine maximum channel count needed
        for (auto* node : orderedNodes)
        {
            auto& processor = *node->getProcessor();
            maxChannelsNeeded = jmax(
                maxChannelsNeeded,
                jmax(processor.getTotalNumInputChannels(), processor.getTotalNumOutputChannels())
            );
        }

        // Second pass: create rendering ops with direct channel mapping
        int midiBufferIndex = 0;
        // Add process ops for each node, with clear ops BEFORE each node to clear unconnected inputs
        for (int i = 0; i < orderedNodes.size(); ++i)
        {
            auto* node = orderedNodes.getUnchecked(i);
            auto& processor = *node->getProcessor();
            auto numIns = processor.getTotalNumInputChannels();
            auto numOuts = processor.getTotalNumOutputChannels();
            auto totalChans = jmax(numIns, numOuts);

            // Calculate max input latency for this node (for cross-subgraph delay compensation)
            // Check all input channels to find the maximum source latency
            int maxInputLatency = 0;
            for (int ch = 0; ch < numIns; ++ch)
            {
                NodeAndChannel destPin{node->nodeID, ch};
                auto channelSources = c.getSourcesForDestination(destPin);
                for (const auto& src : channelSources)
                {
                    int srcLatency = getNodeDelay(src.nodeID);
                    maxInputLatency = jmax(maxInputLatency, srcLatency);
                }
            }

            DBG("[LATENCY] Node " << processor.getName() << " maxInputLatency=" << maxInputLatency);

            DBG("Chain node "
                << i
                << " ("
                << processor.getName()
                << "): "
                << numIns
                << " inputs, "
                << numOuts
                << " outputs, "
                << totalChans
                << " total channels");

            // Clear any input channels that have NO incoming connection
            // Check each input channel to see if it has a source connection
            for (int ch = 0; ch < numIns; ++ch)
            {
                NodeAndChannel destPin{node->nodeID, ch};
                auto sources = c.getSourcesForDestination(destPin);

                if (sources.empty())
                {
                    DBG("  -> Clearing unconnected input channel " << ch);
                    sequence.addClearChannelOp(ch);
                }
            }

            // Direct channel mapping: channel 0→0, 1→1, 2→2, etc.
            Array<int> audioChannelsToUse;
            for (int ch = 0; ch < totalChans; ++ch)
                audioChannelsToUse.add(ch);

            const auto thisNodeLatency = getInputLatencyForNode(c, node->nodeID) + processor.getLatencySamples();
            delays[node->nodeID.uid] = thisNodeLatency;
            totalLatency = jmax(totalLatency, thisNodeLatency);

            sequence.addProcessOp(node, audioChannelsToUse, totalChans, midiBufferIndex);
        }

        sequence.numBuffersNeeded = maxChannelsNeeded;
        sequence.numMidiBuffersNeeded = maxMidiBuffersNeeded;
    }
};

//==============================================================================
/*  A full graph of audio processors, ready to process at a particular sample rate, block size,
    and precision.

    Instances of this class will be created on the main thread, and then passed over to the audio
    thread for processing.
*/
class RenderSequence
{
public:
    using AudioGraphIOProcessor = AudioProcessorGraphMT::AudioGraphIOProcessor;
    using NodeID = AudioProcessorGraphMT::NodeID;

    RenderSequence(const PrepareSettings s, const Nodes& n, const Connections& c)
        : RenderSequence(s, RenderSequenceBuilder::build(n, c))
    {
    }

    // Filtered constructor: only process nodes in the given set with pre-computed global delays
    // Buffer must be float (single precision) - we only use pooled float buffers for chains
    RenderSequence(
        const PrepareSettings s,
        const Nodes& n,
        const Connections& c,
        const std::set<NodeID>& nodeFilter,
        const std::unordered_map<uint32, int>& globalDelays,
        AudioBuffer<float>& buffer
    )
        : RenderSequence(s, RenderSequenceBuilder::buildFiltered(n, c, nodeFilter, globalDelays), &buffer)
    {
    }

    RenderSequence(
        const PrepareSettings s,
        const Nodes& n,
        const Connections& c,
        const std::set<NodeID>& nodeFilter,
        const std::unordered_map<uint32, int>& globalDelays
    )
        : RenderSequence(s, RenderSequenceBuilder::buildFiltered(n, c, nodeFilter, globalDelays))
    {
    }

    void process(AudioBuffer<float>& audio, MidiBuffer& midi, AudioPlayHead* playHead)
    {
        sequence.sequence.perform(audio, midi, playHead);
    }

    int getLatencySamples() const
    {
        return sequence.latencySamples;
    }

    PrepareSettings getSettings() const
    {
        return settings;
    }

private:
    // Private constructor that handles buffer preparation
    RenderSequence(const PrepareSettings s, SequenceAndLatency&& built, AudioBuffer<float>* buffer)
        : settings(s)
        , sequence(std::move(built))
    {
        // If buffer provided, use the external buffer
        // Otherwise use internal buffers
        if (buffer != nullptr)
            sequence.sequence.prepareBuffers(settings.blockSize, buffer);
        else
            sequence.sequence.prepareBuffers(settings.blockSize, nullptr);
    }

    RenderSequence(const PrepareSettings s, SequenceAndLatency&& built)
        : RenderSequence(s, std::move(built), static_cast<AudioBuffer<float>*>(nullptr))
    {
    }

    PrepareSettings settings;
    SequenceAndLatency sequence;
};

//==============================================================================
/*  Thread-safe buffer pool for reusing chain buffers across graph rebuilds.
    Buffers are reference-counted to handle the race condition where:
    - Audio thread is using buffers from current ParallelRenderSequence
    - Message thread is building new ParallelRenderSequence

    Buffers are sized dynamically based on the blockSize from prepareToPlay.
*/
class ChainBufferPool
{
public:
    struct PooledBuffer
    {
        AudioBuffer<float> audioBuffer;
        MidiBuffer midiBuffer;
        std::atomic<int> refCount{0};

        PooledBuffer(int blockSize)
        {
            audioBuffer.setSize(CHAIN_MAX_CHANNELS, blockSize, false, false, true);
            midiBuffer.ensureSize(blockSize);
        }

        void resize(int blockSize)
        {
            audioBuffer.setSize(CHAIN_MAX_CHANNELS, blockSize, false, false, true);
            audioBuffer.clear();
            midiBuffer.ensureSize(blockSize);
        }
    };

    std::shared_ptr<PooledBuffer> acquireBuffer(int blockSize)
    {
        std::lock_guard<std::mutex> lock(mutex);

        // Find a free buffer (refCount == 0) and resize if needed
        for (auto& buffer : buffers)
        {
            int expected = 0;
            if (buffer->refCount.compare_exchange_strong(expected, 1))
            {
                // Resize buffer if blockSize changed
                if (buffer->audioBuffer.getNumSamples() != blockSize)
                    buffer->resize(blockSize);

                // Return with custom deleter that resets refCount when shared_ptr is destroyed
                return std::shared_ptr<PooledBuffer>(
                    buffer.get(),
                    [buffer](PooledBuffer*) mutable { buffer->refCount.store(0); }
                );
            }
        }

        // No free buffer found, allocate a new one with the correct size
        auto newBuffer = std::make_shared<PooledBuffer>(blockSize);
        newBuffer->refCount.store(1);
        buffers.push_back(newBuffer);

        // Return with custom deleter
        return std::shared_ptr<PooledBuffer>(
            newBuffer.get(),
            [newBuffer](PooledBuffer*) mutable { newBuffer->refCount.store(0); }
        );
    }

    void releaseBuffer(std::shared_ptr<PooledBuffer> buffer)
    {
        if (buffer)
            buffer->refCount.store(0);
    }

    size_t getPoolSize() const
    {
        std::lock_guard<std::mutex> lock(mutex);
        return buffers.size();
    }

private:
    std::vector<std::shared_ptr<PooledBuffer>> buffers;
    mutable std::mutex mutex;
};

//==============================================================================
/*  Pool for persistent delay lines that survive graph rebuilds.
    Delay lines are keyed by connection (source chain/node → dest chain/node).
    This preserves delay state during graph reconfiguration, preventing clicks/glitches.

    Delay lines are preallocated to MAX_DELAY_SAMPLES for realtime safety since they
    are always in use by the audio thread when the graph is processing.
*/
class DelayLinePool
{
public:
    // Maximum delay line size: ~21 seconds at 48kHz, ~23 seconds at 44.1kHz
    static constexpr int MAX_DELAY_SAMPLES = 1024 * 1024;

    // Key identifies a specific delay line by source and destination
    struct DelayLineKey
    {
        uint32 sourceId;
        uint32 destId;

        bool operator==(const DelayLineKey& other) const
        {
            return sourceId == other.sourceId && destId == other.destId;
        }
    };

    struct DelayLineKeyHash
    {
        std::size_t operator()(const DelayLineKey& k) const
        {
            return std::hash<uint64>()((uint64(k.sourceId) << 32) | uint64(k.destId));
        }
    };

    struct PooledDelayLine
    {
        juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::Linear> delayLine;
        std::atomic_int delayAmount{0};
        std::atomic<bool> inUse{false};
        std::chrono::steady_clock::time_point lastUsed;
    };

    // Acquire or create a delay line for a specific connection
    std::shared_ptr<PooledDelayLine>
    acquireDelayLine(const DelayLineKey& key, int delayNeeded, double sampleRate, uint32 blockSize, int numChannels)
    {
        std::lock_guard<std::mutex> lock(mutex);
        jassert(delayNeeded <= MAX_DELAY_SAMPLES);

        auto it = delayLines.find(key);
        if (it != delayLines.end())
        {
            // Reuse existing delay line
            auto& pooledLine = it->second;
            pooledLine->inUse.store(true);
            pooledLine->lastUsed = std::chrono::steady_clock::now();
            pooledLine->delayAmount.store(delayNeeded);
            return pooledLine;
        }

        // Create new delay line preallocated to maximum size
        auto pooledLine = std::make_shared<PooledDelayLine>();
        pooledLine->delayLine.prepare(juce::dsp::ProcessSpec{sampleRate, blockSize, static_cast<uint32>(numChannels)});
        pooledLine->delayLine.reset();
        pooledLine->delayLine.setMaximumDelayInSamples(MAX_DELAY_SAMPLES);
        pooledLine->delayAmount.store(delayNeeded);
        pooledLine->inUse.store(true);
        pooledLine->lastUsed = std::chrono::steady_clock::now();

        delayLines[key] = pooledLine;
        return pooledLine;
    }

    // Mark delay line as no longer in use
    void releaseDelayLine(const DelayLineKey& key)
    {
        std::lock_guard<std::mutex> lock(mutex);
        auto it = delayLines.find(key);
        if (it != delayLines.end())
        {
            it->second->inUse.store(false);
            it->second->lastUsed = std::chrono::steady_clock::now();
        }
    }

    // Clean up delay lines that haven't been used for a while (called periodically)
    void cleanupUnused(std::chrono::seconds maxAge = std::chrono::seconds(5))
    {
        std::lock_guard<std::mutex> lock(mutex);
        auto now = std::chrono::steady_clock::now();

        for (auto it = delayLines.begin(); it != delayLines.end();)
            if (!it->second->inUse && (now - it->second->lastUsed) > maxAge)
                it = delayLines.erase(it);
            else
                ++it;
    }

    size_t getPoolSize() const
    {
        std::lock_guard<std::mutex> lock(mutex);
        return delayLines.size();
    }

private:
    std::unordered_map<DelayLineKey, std::shared_ptr<PooledDelayLine>, DelayLineKeyHash> delayLines;
    mutable std::mutex mutex;
};

//==============================================================================
/*  Parallel-ready render sequence that partitions the graph into independent chains
    that can be executed concurrently. Each chain has its own RenderSequence and buffers.

    Chains are executed in parallel within each topological level using a thread pool:
    - Each chain has isolated buffers (no shared mutable state)
    - Dependency tracking uses atomics for thread-safe coordination
    - Execution respects topological order (level-by-level barrier synchronization)
*/
class ParallelRenderSequence
{
public:
    using NodeID = AudioProcessorGraphMT::NodeID;
    using Node = AudioProcessorGraphMT::Node;
    using Connection = AudioProcessorGraphMT::Connection;

    // Forward declaration for job context
    struct ChainRenderSequence;

    // Helper class for delay-compensated mixing
    // Applies delay to individual sources before mixing them into destination
    // Uses pooled delay lines that persist across graph rebuilds
    class DelayCompensatingMixer
    {
    public:
        explicit DelayCompensatingMixer(uint32 destId_, DelayLinePool* pool_)
            : destId(destId_)
            , pool(pool_)
        {
        }

        // Register a source that will be mixed with delay compensation
        void registerSource(
            uint32 sourceId,
            int sourceLatency,
            int totalLatency,
            double sampleRate,
            uint32 blockSize,
            int numChannels = 2
        )
        {
            int delayNeeded = totalLatency - sourceLatency;

            // Acquire delay line from pool (reuses if connection already exists)
            DelayLinePool::DelayLineKey key{sourceId, destId};
            auto pooledLine = pool->acquireDelayLine(key, delayNeeded, sampleRate, blockSize, numChannels);

            // Store the pooled delay line
            delayLines[sourceId] = pooledLine;
        }

        // Mix a source into destination with delay compensation
        void mixWithDelay(uint32 sourceId, const float* src, float* dst, int numSamples, int channel = 0)
        {
            auto it = delayLines.find(sourceId);
            if (it == delayLines.end())
            {
                // Source not registered - fallback to direct add
                FloatVectorOperations::add(dst, src, numSamples);
                return;
            }

            auto& pooledLine = it->second;
            auto& delayLine = pooledLine->delayLine;
            int delay = pooledLine->delayAmount.load();

            // Apply delay compensation
            for (int i = 0; i < numSamples; ++i)
            {
                delayLine.pushSample(channel, src[i]);
                float delayedSample = delayLine.popSample(channel, delay);
                dst[i] += delayedSample;
            }
        }

        void clearSources()
        {
            // Release all delay lines back to pool
            for (auto& [sourceId, pooledLine] : delayLines)
            {
                DelayLinePool::DelayLineKey key{sourceId, destId};
                pool->releaseDelayLine(key);
            }
            delayLines.clear();
        }

    private:
        uint32 destId;
        DelayLinePool* pool;
        std::unordered_map<uint32, std::shared_ptr<DelayLinePool::PooledDelayLine>> delayLines;
    };

    // Each chain represents an independent subgraph that can execute in parallel
    struct ChainRenderSequence
    {
        std::unique_ptr<RenderSequence> sequence;
        int chainLatency = 0;
        int latencySum = 0; // Sum of all processor latencies in this subgraph for change detection
        int topologicalLevel = 0;
        size_t subgraphIndex = 0;
        bool connectsToOutput = false;     // True if this chain has audio outputs connecting to the audio output node
        bool connectsToMidiOutput = false; // True if this chain has MIDI outputs connecting to the MIDI output node

        // Reference to pooled buffer (shared ownership with pool)
        std::shared_ptr<ChainBufferPool::PooledBuffer> pooledBuffer;

        // Convenience accessors to the pooled buffers
        AudioBuffer<float>& getAudioBuffer()
        {
            return pooledBuffer->audioBuffer;
        }

        const AudioBuffer<float>& getAudioBuffer() const
        {
            return pooledBuffer->audioBuffer;
        }

        MidiBuffer& getMidiBuffer()
        {
            return pooledBuffer->midiBuffer;
        }

        // Dependency tracking for parallel execution
        std::atomic<int> pendingDependencies{0};
        int initialDependencyCount = 0;
        std::vector<ChainRenderSequence*> dependentChains;

        // Input delay compensation mixer for cross-subgraph latency alignment
        // Each chain that feeds into this chain registers as a source with its latency
        DelayCompensatingMixer inputMixer;

        ChainRenderSequence(uint32 chainId, DelayLinePool* pool)
            : inputMixer(chainId, pool)
        {
        }

        ~ChainRenderSequence()
        {
            // Release buffer back to pool by marking it as free
            if (pooledBuffer)
                pooledBuffer->refCount.store(0, std::memory_order_release);
        }

        // Non-copyable, non-movable due to atomic
        ChainRenderSequence(const ChainRenderSequence&) = delete;
        ChainRenderSequence& operator=(const ChainRenderSequence&) = delete;
        ChainRenderSequence(ChainRenderSequence&&) = delete;
        ChainRenderSequence& operator=(ChainRenderSequence&&) = delete;
    };

    // Job context for parallel chain processing
    // Contains all data needed to process a chain on a worker thread
    struct ChainProcessingJob
    {
        ChainRenderSequence* chain;
        AudioBuffer<float>* audioBufferView;
        AudioPlayHead* playHead;

        // Static function for thread pool execution
        static void execute(void* context)
        {
            auto* job = static_cast<ChainProcessingJob*>(context);
            if (job && job->chain && job->audioBufferView)
                job->chain->sequence->process(*job->audioBufferView, job->chain->getMidiBuffer(), job->playHead);
        }
    };

    ParallelRenderSequence(
        const PrepareSettings& s,
        AudioProcessorGraphMT& graph,
        const Nodes& n,
        const Connections& c,
        ChainBufferPool& pool,
        DelayLinePool& delayPool
    )
        : settings(s)
        , nodes(n)
        , bufferPool(pool)
        , delayLinePool(delayPool)
        , outputMixer(UINT32_MAX, &delayPool) // Use UINT32_MAX as destId for output mixer
    {
        // Extract parallel subgraphs
        GraphPartitioner extractor;
        subgraphs = extractor.extractUniversalParallelization(graph);
        connectionsVec = c.getConnections();
        extractor.buildSubgraphDependencies(subgraphs, connectionsVec);

        DBG("[PARALLEL] Extracted " << subgraphs.size() << " subgraphs");
        for (size_t i = 0; i < subgraphs.size(); ++i)
        {
            DBG("[PARALLEL]   Subgraph "
                << i
                << " has "
                << subgraphs[i].nodeIDs.size()
                << " nodes, level "
                << subgraphs[i].topologicalLevel);
        }

        // Find I/O nodes (needed for both processor graphs and passthrough-only graphs)
        for (const auto& node : n.getNodes())
        {
            if (auto* ioProc = dynamic_cast<AudioProcessorGraphMT::AudioGraphIOProcessor*>(node->getProcessor()))
            {
                if (ioProc->getType() == AudioProcessorGraphMT::AudioGraphIOProcessor::audioInputNode)
                    audioInputNodeID = node->nodeID;
                else if (ioProc->getType() == AudioProcessorGraphMT::AudioGraphIOProcessor::audioOutputNode)
                    audioOutputNodeID = node->nodeID;
                else if (ioProc->getType() == AudioProcessorGraphMT::AudioGraphIOProcessor::midiInputNode)
                    midiInputNodeID = node->nodeID;
                else if (ioProc->getType() == AudioProcessorGraphMT::AudioGraphIOProcessor::midiOutputNode)
                    midiOutputNodeID = node->nodeID;
            }
        }

        // Build passthrough mappings for direct Input → Output connections (no processors)
        // This must be done even when subgraphs.empty() to handle passthrough-only graphs
        for (const auto& conn : connectionsVec)
        {
            if (conn.source.nodeID == audioInputNodeID && conn.destination.nodeID == audioOutputNodeID)
            {
                passthroughChannelMap[conn.source.channelIndex] = conn.destination.channelIndex;
                DBG("Passthrough: input ch"
                    << conn.source.channelIndex
                    << " -> output ch"
                    << conn.destination.channelIndex);
            }
        }

        DBG("Total passthrough mappings: " << passthroughChannelMap.size());

        // If no subgraphs (no processor nodes), we're done - only passthrough connections exist
        if (subgraphs.empty())
        {
            DBG("No processor nodes - passthrough-only graph");
            return;
        }

        // Calculate global delays for ALL nodes in the graph
        // This is critical for cross-subgraph delay compensation
        const auto globalDelays = RenderSequenceBuilder::calculateGlobalDelays(n, c);

        // Store global delays for later use in output latency compensation
        nodeLatencies = globalDelays;

        DBG("[LATENCY] Global delays calculated for " << nodeLatencies.size() << " nodes:");
        for (const auto& [nodeId, latency] : nodeLatencies)
            if (latency > 0)
                DBG("[LATENCY]   Node " << (int)nodeId << ": " << latency << " samples");

        // Build filtered RenderSequences for all subgraphs
        chains.reserve(subgraphs.size());
        maxTopologicalLevel = 0;

        for (size_t i = 0; i < subgraphs.size(); ++i)
        {
            const auto& subgraph = subgraphs[i];
            auto chain = std::make_unique<ChainRenderSequence>(static_cast<uint32>(i), &delayLinePool);

            // Acquire buffer from pool first (thread-safe, reuses freed buffers)
            // Buffer is sized to blockSize from prepareToPlay
            chain->pooledBuffer = bufferPool.acquireBuffer(s.blockSize);

            // Build filtered RenderSequence with pooled buffer
            // RenderOps will be prepared immediately with the pooled buffer (no separate binding step)
            // Pass globalDelays so nodes at subgraph boundaries can see input latencies from other subgraphs
            // Linear chains use direct channel mapping (0→0, 1→1, etc.) - no complex buffer allocation needed
            chain->sequence =
                std::make_unique<RenderSequence>(s, n, c, subgraph.nodeIDs, globalDelays, chain->getAudioBuffer());
            chain->chainLatency = chain->sequence->getLatencySamples();
            chain->topologicalLevel = subgraph.topologicalLevel;
            chain->subgraphIndex = i;

            // Store latency sum for runtime change detection
            for (const auto& nodeID : subgraph.nodeIDs)
                if (auto node = n.getNodeForId(nodeID))
                    if (auto* proc = node->getProcessor())
                        chain->latencySum += proc->getLatencySamples();

            // Check if any node in this subgraph connects to the audio or MIDI output nodes
            chain->connectsToOutput = false;
            chain->connectsToMidiOutput = false;
            for (const auto& conn : connectionsVec)
            {
                if (subgraph.nodeIDs.count(conn.source.nodeID) > 0)
                {
                    // Check if destination is an output node
                    auto destNode = n.getNodeForId(conn.destination.nodeID);
                    if (destNode)
                    {
                        if (auto* ioProc =
                                dynamic_cast<AudioProcessorGraphMT::AudioGraphIOProcessor*>(destNode->getProcessor()))
                        {
                            if (ioProc->getType() == AudioProcessorGraphMT::AudioGraphIOProcessor::audioOutputNode)
                                chain->connectsToOutput = true;
                            else if (ioProc->getType() == AudioProcessorGraphMT::AudioGraphIOProcessor::midiOutputNode)
                                chain->connectsToMidiOutput = true;
                        }
                    }
                }
            }

            // Set up dependency tracking
            chain->initialDependencyCount = static_cast<int>(subgraph.dependsOn.size());
            chain->pendingDependencies.store(chain->initialDependencyCount, std::memory_order_relaxed);

            maxTopologicalLevel = std::max(maxTopologicalLevel, chain->topologicalLevel);
            totalLatency = std::max(totalLatency, chain->chainLatency);

            chains.push_back(std::move(chain));
        }

        // Build dependency pointers
        for (size_t i = 0; i < subgraphs.size(); ++i)
        {
            for (size_t dependentIdx : subgraphs[i].dependents)
                if (dependentIdx < chains.size())
                    chains[i]->dependentChains.push_back(chains[dependentIdx].get());
        }

        // Build inter-chain MIDI connection mapping
        // Check which chains have MIDI connections to each other
        for (const auto& conn : connectionsVec)
        {
            // Check if this is a MIDI connection (uses midiChannelIndex)
            if (conn.source.channelIndex == AudioProcessorGraphMT::midiChannelIndex
                && conn.destination.channelIndex == AudioProcessorGraphMT::midiChannelIndex)
            {
                // Find which chain contains source node
                size_t sourceChainIdx = SIZE_MAX;
                for (size_t i = 0; i < subgraphs.size(); ++i)
                {
                    if (subgraphs[i].nodeIDs.count(conn.source.nodeID) > 0)
                    {
                        sourceChainIdx = i;
                        break;
                    }
                }

                // Find which chain contains dest node
                size_t destChainIdx = SIZE_MAX;
                for (size_t i = 0; i < subgraphs.size(); ++i)
                {
                    if (subgraphs[i].nodeIDs.count(conn.destination.nodeID) > 0)
                    {
                        destChainIdx = i;
                        break;
                    }
                }

                // If both found and different chains, record the MIDI connection
                if (sourceChainIdx != SIZE_MAX && destChainIdx != SIZE_MAX)
                {
                    midiChainConnections.insert({sourceChainIdx, destChainIdx});
                    DBG("MIDI connection: Chain " << sourceChainIdx << " -> Chain " << destChainIdx);
                }
            }
        }

        DBG("Total inter-chain MIDI connections: " << midiChainConnections.size());

        // Initialize input mixers for delay compensation
        // Query the graph to find which nodes (from other chains) feed into each chain's first node
        for (size_t i = 0; i < chains.size(); ++i)
        {
            auto& destChain = chains[i];
            const auto& destSubgraph = subgraphs[i];

            // Find the first node in this chain's subgraph (topologically first)
            // Since subgraphs are linear chains, we need to find the node with no internal predecessor
            NodeID firstNodeInChain;
            bool foundFirstNode = false;

            for (const auto& nodeID : destSubgraph.nodeIDs)
            {
                // Check if this node has any input from within the same subgraph
                bool hasInternalInput = false;
                for (const auto& conn : connectionsVec)
                {
                    if (conn.destination.nodeID == nodeID && destSubgraph.nodeIDs.count(conn.source.nodeID) > 0)
                    {
                        hasInternalInput = true;
                        break;
                    }
                }

                if (!hasInternalInput)
                {
                    firstNodeInChain = nodeID;
                    foundFirstNode = true;
                    break;
                }
            }

            if (!foundFirstNode)
                continue;

            // Now find all connections from other chains to this first node
            // Build a map of source chain -> accumulated latency
            std::unordered_map<size_t, int> sourceChainLatencies;

            for (const auto& conn : connectionsVec)
            {
                if (conn.destination.nodeID == firstNodeInChain)
                {
                    // Find which chain contains the source node
                    for (size_t j = 0; j < subgraphs.size(); ++j)
                    {
                        if (i != j && subgraphs[j].nodeIDs.count(conn.source.nodeID) > 0)
                        {
                            // Source node is in chain j, get its accumulated latency from globalDelays
                            int accumulatedLatency = 0;
                            auto it = nodeLatencies.find(conn.source.nodeID.uid);
                            if (it != nodeLatencies.end())
                                accumulatedLatency = it->second;

                            sourceChainLatencies[j] = std::max(sourceChainLatencies[j], accumulatedLatency);
                            break;
                        }
                    }
                }
            }

            // Calculate max input latency
            int maxInputLatency = 0;
            for (const auto& [chainIdx, latency] : sourceChainLatencies)
                maxInputLatency = std::max(maxInputLatency, latency);

            // Register each source chain with this chain's input mixer
            for (const auto& [sourceChainIdx, accumulatedLatency] : sourceChainLatencies)
            {
                // Get the source chain's channel count (chains[sourceChainIdx] should exist)
                int numChannels = 2; // Default to stereo
                if (sourceChainIdx < chains.size())
                    numChannels = chains[sourceChainIdx]->getAudioBuffer().getNumChannels();

                destChain->inputMixer.registerSource(
                    static_cast<int>(sourceChainIdx), // sourceId = source chain index
                    accumulatedLatency,               // accumulated latency from globalDelays
                    maxInputLatency,                  // max of all input latencies
                    s.sampleRate,
                    s.blockSize,
                    numChannels // Number of channels
                );
            }
        }

        // DELAY COMPENSATION STRATEGY:
        // =============================
        // Global delays calculated BEFORE building subgraphs give each node's accumulated latency.
        // These are passed to filtered RenderSequenceBuilders so that:
        //
        // 1. Nodes with SINGLE input: No compensation needed (data flows naturally)
        // 2. Nodes with MULTIPLE inputs at DIFFERENT latencies: Compensate for the difference
        //
        // Key insight: Only the DIFFERENCE in input latencies matters. A node receiving inputs
        // from parallel paths (e.g., PathA=256ms, PathB=128ms) needs to delay the faster path
        // by 128ms to align them. This happens automatically in createRenderingOpsForNode when
        // it sees maxLatency > nodeDelay for a particular input.
        //
        // Within a subgraph's linear chain (A→B), delays accumulate naturally with no compensation.
        // At subgraph boundaries where parallel paths merge, global delays enable proper alignment.

        // Organize chains by topological level
        chainsByLevel.resize(maxTopologicalLevel + 1);
        for (auto& chain : chains)
            chainsByLevel[chain->topologicalLevel].push_back(chain.get());

        // Build input channel mappings: Audio Input node → chains at level 0
        // Maps (chainIndex, destinationChannel) → sourceChannel from host input
        for (const auto& conn : connectionsVec)
        {
            if (conn.source.nodeID == audioInputNodeID)
            {
                // Find which chain contains the destination node
                for (size_t i = 0; i < subgraphs.size(); ++i)
                {
                    if (subgraphs[i].nodeIDs.count(conn.destination.nodeID) > 0)
                    {
                        // Map: chain i, destination channel → source channel
                        inputChannelMap[{i, conn.destination.channelIndex}] = conn.source.channelIndex;

                        DBG("Input mapping: Chain "
                            << i
                            << ", dest ch "
                            << conn.destination.channelIndex
                            << " <- source ch "
                            << conn.source.channelIndex);

                        break;
                    }
                }
            }
        }

        DBG("Total input mappings: " << inputChannelMap.size());

        // Build MIDI input mappings: MIDI Input node → chains
        // Track which chains should receive MIDI input
        for (const auto& conn : connectionsVec)
        {
            if (conn.source.nodeID == midiInputNodeID)
            {
                // Find which chain contains the destination node
                for (size_t i = 0; i < subgraphs.size(); ++i)
                {
                    if (subgraphs[i].nodeIDs.count(conn.destination.nodeID) > 0)
                    {
                        midiInputChains.insert(i);

                        DBG("MIDI input mapping: Chain " << i << " receives MIDI");

                        break;
                    }
                }
            }
        }

        DBG("Total MIDI input chains: " << midiInputChains.size());

        // Build output channel mappings: chains → Audio Output node
        // Maps (chainIndex, sourceChannel) → destinationChannel to host output
        // Also build delay compensation info for each mapping
        struct OutputMappingInfo
        {
            int destChannel;
            NodeID sourceNodeID;
            int delayNeeded;
        };

        std::map<std::pair<size_t, int>, OutputMappingInfo> outputMappingInfo; // (chainIndex, sourceChannel) -> info

        for (const auto& conn : connectionsVec)
        {
            if (conn.destination.nodeID == audioOutputNodeID)
            {
                // Find which chain contains the source node
                for (size_t i = 0; i < subgraphs.size(); ++i)
                {
                    if (subgraphs[i].nodeIDs.count(conn.source.nodeID) > 0)
                    {
                        // Map: chain i, source channel → destination channel
                        outputChannelMap[{i, conn.source.channelIndex}] = conn.destination.channelIndex;

                        // Store mapping info for delay compensation
                        outputMappingInfo[{i, conn.source.channelIndex}] = {
                            conn.destination.channelIndex,
                            conn.source.nodeID,
                            0 // delay will be calculated next
                        };

                        break;
                    }
                }
            }
        }

        // Calculate delay compensation for each output mapping
        // Each chain->output mapping needs to be registered with the output mixer
        int maxOutputLatency = 0;
        std::map<std::pair<size_t, int>, int>
            outputSourceLatencies; // (chainIndex, sourceChannel) -> accumulated latency

        for (auto& [key, info] : outputMappingInfo)
        {
            auto it = nodeLatencies.find(info.sourceNodeID.uid);
            int sourceLatency = (it != nodeLatencies.end()) ? it->second : 0;
            outputSourceLatencies[key] = sourceLatency;
            maxOutputLatency = std::max(maxOutputLatency, sourceLatency);
        }

        // Register each output source with the output mixer
        // Use a unique ID that combines chain index and source channel
        for (const auto& [key, info] : outputMappingInfo)
        {
            const auto& [chainIdx, sourceChannel] = key;
            int sourceLatency = outputSourceLatencies[key];

            // Create unique source ID by combining chain index and channel
            uint32 sourceId = (uint32(chainIdx) << 16) | uint32(sourceChannel);

            outputMixer.registerSource(sourceId, sourceLatency, maxOutputLatency, s.sampleRate, s.blockSize);
        }

        // Find the maximum output channel number (for buffer allocation)
        int maxOutputChannels = 0;
        for (const auto& [key, info] : outputMappingInfo)
            maxOutputChannels = std::max(maxOutputChannels, info.destChannel + 1);
        for (const auto& [inputCh, outputCh] : passthroughChannelMap)
            maxOutputChannels = std::max(maxOutputChannels, outputCh + 1);

        // ========================================================================
        // PRE-ALLOCATE RESOURCES FOR REALTIME-SAFE PARALLEL PROCESSING
        // ========================================================================
        // Allocate all resources during graph rebuild (message thread) to avoid
        // allocations in the audio thread during process()

        // Get thread pool for worker count (needed for barrier creation)
        auto* threadPool = atk::AudioThreadPool::getInstance();
        const int numWorkers = threadPool ? threadPool->getNumWorkers() : 0;

        // Pre-allocate barriers (one per topological level)
        barriers.clear();
        barriers.reserve(chainsByLevel.size());
        for (size_t level = 0; level < chainsByLevel.size(); ++level)
        {
            if (!chainsByLevel[level].empty() && numWorkers > 0)
            {
                // Create barrier: numWorkers + 1 (for main thread participation)
                auto barrier = atk::ThreadBarrier::make(numWorkers + 1);
                barrier->configure(s.blockSize, s.sampleRate);
                barriers.push_back(std::move(barrier));
            }
            else
            {
                // No parallel execution needed for this level (empty or no workers)
                barriers.push_back(nullptr);
            }
        }

        // Pre-allocate job and buffer view vectors for each level
        jobsPerLevel.clear();
        bufferViewsPerLevel.clear();
        jobsPerLevel.resize(chainsByLevel.size());
        bufferViewsPerLevel.resize(chainsByLevel.size());

        for (size_t level = 0; level < chainsByLevel.size(); ++level)
        {
            const size_t numChainsAtLevel = chainsByLevel[level].size();
            if (numChainsAtLevel > 0)
            {
                // Pre-size vectors to exact capacity needed (no allocation during process())
                // We'll use resize() to create default-constructed elements that we can assign to
                jobsPerLevel[level].resize(numChainsAtLevel);
                bufferViewsPerLevel[level].resize(numChainsAtLevel);
            }
        }

        DBG("[PARALLEL] Pre-allocated " << barriers.size() << " barriers and job vectors for realtime-safe processing");
    }

    void process(AudioBuffer<float>& audio, MidiBuffer& midi, AudioPlayHead* playHead)
    {
        // Handle passthrough for graphs with no processor nodes (only I/O nodes)
        if (chains.empty() && !passthroughChannelMap.empty())
        {
            // Use a pooled buffer as temporary storage
            auto tempBuffer = bufferPool.acquireBuffer(audio.getNumSamples());
            tempBuffer->audioBuffer
                .setSize(tempBuffer->audioBuffer.getNumChannels(), audio.getNumSamples(), false, false, true);
            tempBuffer->audioBuffer.clear();

            const int numSamples = audio.getNumSamples();

            // The passthrough map directly stores inputChannel -> outputChannel
            // Copy from host input to the correct temp buffer channels
            for (const auto& mapping : passthroughChannelMap)
            {
                int inputChannel = mapping.first;   // Host input channel (from Audio Input node)
                int outputChannel = mapping.second; // Host output channel (to Audio Output node)

                if (inputChannel < audio.getNumChannels() && outputChannel < tempBuffer->audioBuffer.getNumChannels())
                {
                    const auto* src = audio.getReadPointer(inputChannel);
                    auto* dst = tempBuffer->audioBuffer.getWritePointer(outputChannel);
                    FloatVectorOperations::copy(dst, src, numSamples);
                }
            }

            // Copy temp buffer back to host output
            audio.clear();
            const int numChannels = std::min(tempBuffer->audioBuffer.getNumChannels(), audio.getNumChannels());
            for (int ch = 0; ch < numChannels; ++ch)
            {
                const auto* src = tempBuffer->audioBuffer.getReadPointer(ch);
                auto* dst = audio.getWritePointer(ch);
                FloatVectorOperations::copy(dst, src, numSamples);
            }
            // MIDI passthrough is already in the buffer
            return;
        }

        // If no chains and no passthrough, produce silence
        if (chains.empty())
        {
            audio.clear();
            midi.clear();
            return;
        }

        // ========================================================================
        // PARALLEL PROCESSING PIPELINE (Thread-safe design)
        // ========================================================================
        // Main thread orchestrates:
        // 1. Distribute inputs to root chains (serial)
        // 2. Process chains level-by-level (parallel within each level, barrier between levels)
        // 3. Wait for all chains to complete (implicit barrier after last level)
        // 4. Collect outputs from terminal chains (serial)
        // 5. Copy to host output (serial)
        //
        // This design ensures no data races:
        // - Each chain has isolated buffers (no shared write state during processing)
        // - Input distribution is serial (main thread only)
        // - Output collection is serial (main thread only)
        // - Inter-chain routing uses per-chain buffers (safe for parallel reads/writes)
        // ========================================================================

        // Step 1: Reset all dependency counters
        for (auto& chain : chains)
            chain->pendingDependencies.store(chain->initialDependencyCount, std::memory_order_relaxed);

        // Step 2: Ensure chain buffers are correctly sized, then clear them
        // If numSamples > buffer capacity, we need to resize (which invalidates pointers,
        // but prepare() will be called again by perform() to refresh them)
        const int numSamples = audio.getNumSamples();
        for (auto& chain : chains)
        {
            auto& chainBuffer = chain->getAudioBuffer();

            // Resize if necessary (rare - only when host changes buffer size)
            if (chainBuffer.getNumSamples() < numSamples)
            {
                chainBuffer.setSize(chainBuffer.getNumChannels(), numSamples, false, false, true);
                // Note: This invalidates cached pointers in RenderOps, but perform()
                // calls prepare() which refreshes them before processing
            }

            // Clear to remove old data from pooled buffer reuse
            chainBuffer.clear();
            chain->getMidiBuffer().clear();
        }

        // Step 3: Distribute input to root-level chains (SERIAL - main thread only)
        // Implement Audio Input node logic: map host input channels to chain input channels
        if (!chainsByLevel.empty() && !chainsByLevel[0].empty())
        {
            for (auto* chain : chainsByLevel[0])
            {
                // Copy channels according to Audio Input node → chain connections
                for (const auto& mapping : inputChannelMap)
                {
                    if (mapping.first.first == chain->subgraphIndex) // This chain
                    {
                        int destChannel = mapping.first.second; // Chain's input channel
                        int sourceChannel = mapping.second;     // Host's output channel

                        if (sourceChannel < audio.getNumChannels()
                            && destChannel < chain->getAudioBuffer().getNumChannels())
                        {
                            const auto* src = audio.getReadPointer(sourceChannel);
                            auto* dst = chain->getAudioBuffer().getWritePointer(destChannel);
                            FloatVectorOperations::copy(dst, src, numSamples);
                        }
                    }
                }

                // Copy MIDI if this chain has MIDI input connections
                // Check if any node in this chain receives MIDI from the MIDI Input node
                bool chainReceivesMidi = (midiInputChains.count(chain->subgraphIndex) > 0);

                if (chainReceivesMidi)
                    chain->getMidiBuffer().addEvents(midi, 0, numSamples, 0);
            }
        }

        // Step 4: Process chains level by level with parallel execution within each level
        // Get thread pool instance (may be null if not initialized)
        auto* pool = atk::AudioThreadPool::getInstance();
        const bool canUseThreadPool = pool && pool->isReady();

        for (int level = 0; level <= maxTopologicalLevel; ++level)
        {
            auto& chainsAtLevel = chainsByLevel[level];
            const int numChainsAtLevel = static_cast<int>(chainsAtLevel.size());

            if (numChainsAtLevel == 0)
                continue; // No chains at this level

            if (numChainsAtLevel == 1 || !canUseThreadPool)
            {
                // Single chain or no thread pool - process directly without parallelization overhead
                for (auto* chain : chainsAtLevel)
                {
                    // The pooled buffer is always float, sized to maxBlockSize
                    // We only process numSamples, so create a view with the correct size
                    AudioBuffer<float> chainBufferView(
                        chain->getAudioBuffer().getArrayOfWritePointers(),
                        chain->getAudioBuffer().getNumChannels(),
                        numSamples
                    );

                    // Process this chain's subgraph with the correctly-sized buffer view
                    chain->sequence->process(chainBufferView, chain->getMidiBuffer(), playHead);

                    // Route this chain's output to all dependent chains
                    for (auto* dependent : chain->dependentChains)
                    {
                        // Find which specific channels are connected between these chains
                        // Only mix the channels that have explicit connections
                        for (const auto& conn : connectionsVec)
                        {
                            // Skip MIDI connections
                            if (conn.source.isMIDI() || conn.destination.isMIDI())
                                continue;

                            // Check if this connection goes from a node in source chain to a node in dest chain
                            bool sourceInChain = subgraphs[chain->subgraphIndex].nodeIDs.count(conn.source.nodeID) > 0;
                            bool destInDependent =
                                subgraphs[dependent->subgraphIndex].nodeIDs.count(conn.destination.nodeID) > 0;

                            if (sourceInChain && destInDependent)
                            {
                                // Valid connection from this chain to dependent chain
                                int srcChannel = conn.source.channelIndex;
                                int dstChannel = conn.destination.channelIndex;

                                if (srcChannel < chain->getAudioBuffer().getNumChannels()
                                    && dstChannel < dependent->getAudioBuffer().getNumChannels())
                                {
                                    const auto* src = chain->getAudioBuffer().getReadPointer(srcChannel);
                                    auto* dst = dependent->getAudioBuffer().getWritePointer(dstChannel);
                                    dependent->inputMixer.mixWithDelay(
                                        static_cast<int>(chain->subgraphIndex),
                                        src,
                                        dst,
                                        numSamples,
                                        dstChannel
                                    );
                                }
                            }
                        }

                        // Copy MIDI only if there's an explicit MIDI connection between these chains
                        if (midiChainConnections.count({chain->subgraphIndex, dependent->subgraphIndex}) > 0)
                            dependent->getMidiBuffer().addEvents(chain->getMidiBuffer(), 0, numSamples, 0);

                        // Decrement dependency counter (atomic - thread-safe)
                        dependent->pendingDependencies.fetch_sub(1, std::memory_order_release);
                    }
                }
            }
            else
            {
                // Multiple chains - parallel execution using thread pool
                // Use pre-allocated barrier for this level (realtime-safe - no allocation)
                auto& barrier = barriers[level];
                if (!barrier)
                {
                    // Fallback to serial processing if barrier wasn't created
                    for (auto* chain : chainsAtLevel)
                        chain->sequence->process(chain->getAudioBuffer(), chain->getMidiBuffer(), playHead);
                    continue;
                }

                // Reconfigure barrier for current block size (no allocation - just updates atomics)
                barrier->configure(numSamples, settings.sampleRate);

                // Prepare jobs for parallel processing
                pool->prepareJobs(barrier);

                // Use pre-allocated job contexts and buffer views (realtime-safe - no allocation)
                // Just update the existing slots with new data - no push_back, no reallocation
                auto& jobs = jobsPerLevel[level];
                auto& bufferViews = bufferViewsPerLevel[level];

                // Fill in job data for this frame (directly into pre-allocated slots)
                for (size_t i = 0; i < chainsAtLevel.size(); ++i)
                {
                    auto* chain = chainsAtLevel[i];

                    // Update buffer view with current chain's buffer (in-place, no allocation)
                    auto& bufferView = bufferViews[i];
                    bufferView = AudioBuffer<float>(
                        chain->getAudioBuffer().getArrayOfWritePointers(),
                        chain->getAudioBuffer().getNumChannels(),
                        numSamples
                    );

                    // Update job context (in-place, no allocation)
                    auto& job = jobs[i];
                    job.chain = chain;
                    job.audioBufferView = &bufferViews[i];
                    job.playHead = playHead;

                    // Add job to thread pool
                    pool->addJob(&ChainProcessingJob::execute, &jobs[i]);
                }

                // Wake up worker threads to start processing
                pool->kickWorkers();

                // Main thread participates in work stealing
                while (pool->tryStealAndExecuteJob())
                {
                    // Keep stealing and executing jobs until none remain
                }

                // Wait for all worker threads to complete (barrier synchronization)
                // Each worker arrives once after processing all its jobs
                barrier->arriveAndWait();

                // All chains at this level have completed - now route outputs to dependents
                // This must be done serially after the barrier to ensure all processing is complete
                for (auto* chain : chainsAtLevel)
                {
                    for (auto* dependent : chain->dependentChains)
                    {
                        // Find which specific channels are connected between these chains
                        // Only mix the channels that have explicit connections
                        for (const auto& conn : connectionsVec)
                        {
                            // Skip MIDI connections
                            if (conn.source.isMIDI() || conn.destination.isMIDI())
                                continue;

                            // Check if this connection goes from a node in source chain to a node in dest chain
                            bool sourceInChain = subgraphs[chain->subgraphIndex].nodeIDs.count(conn.source.nodeID) > 0;
                            bool destInDependent =
                                subgraphs[dependent->subgraphIndex].nodeIDs.count(conn.destination.nodeID) > 0;

                            if (sourceInChain && destInDependent)
                            {
                                // Valid connection from this chain to dependent chain
                                int srcChannel = conn.source.channelIndex;
                                int dstChannel = conn.destination.channelIndex;

                                if (srcChannel < chain->getAudioBuffer().getNumChannels()
                                    && dstChannel < dependent->getAudioBuffer().getNumChannels())
                                {
                                    const auto* src = chain->getAudioBuffer().getReadPointer(srcChannel);
                                    auto* dst = dependent->getAudioBuffer().getWritePointer(dstChannel);
                                    dependent->inputMixer.mixWithDelay(
                                        static_cast<int>(chain->subgraphIndex),
                                        src,
                                        dst,
                                        numSamples,
                                        dstChannel
                                    );
                                }
                            }
                        }

                        // Copy MIDI only if there's an explicit MIDI connection between these chains
                        if (midiChainConnections.count({chain->subgraphIndex, dependent->subgraphIndex}) > 0)
                            dependent->getMidiBuffer().addEvents(chain->getMidiBuffer(), 0, numSamples, 0);

                        // Decrement dependency counter (atomic - thread-safe)
                        dependent->pendingDependencies.fetch_sub(1, std::memory_order_release);
                    }
                }
            }
        }

        // Step 5: Collect outputs from chains that connect to Audio Output node (SERIAL - main thread only)
        // Implement Audio Output node logic: map chain output channels to host output channels
        // CRITICAL: Must be serial to prevent data races when writing to host output buffer

        // For passthrough: save input channels before clearing (if needed)
        std::shared_ptr<ChainBufferPool::PooledBuffer> passthroughBuffer;
        if (!passthroughChannelMap.empty())
        {
            passthroughBuffer = bufferPool.acquireBuffer(numSamples);
            passthroughBuffer->audioBuffer
                .setSize(passthroughBuffer->audioBuffer.getNumChannels(), numSamples, false, false, true);
            passthroughBuffer->audioBuffer.clear();

            // Copy input channels that need passthrough
            for (const auto& mapping : passthroughChannelMap)
            {
                int inputChannel = mapping.first;
                int outputChannel = mapping.second;

                if (inputChannel < audio.getNumChannels()
                    && outputChannel < passthroughBuffer->audioBuffer.getNumChannels())
                {
                    const auto* src = audio.getReadPointer(inputChannel);
                    auto* dst = passthroughBuffer->audioBuffer.getWritePointer(outputChannel);
                    FloatVectorOperations::copy(dst, src, numSamples);
                }
            }
        }

        audio.clear();
        midi.clear();

        for (auto& chain : chains)
        {
            // Process output mappings for this chain
            for (const auto& mapping : outputChannelMap)
            {
                if (mapping.first.first == chain->subgraphIndex) // This chain
                {
                    int sourceChannel = mapping.first.second; // Chain's output channel
                    int destChannel = mapping.second;         // Host's output channel

                    if (sourceChannel < chain->getAudioBuffer().getNumChannels()
                        && destChannel < audio.getNumChannels())
                    {
                        const auto* src = chain->getAudioBuffer().getReadPointer(sourceChannel);
                        auto* dst = audio.getWritePointer(destChannel);

                        // Create unique source ID by combining chain index and channel
                        uint32 sourceId = (uint32(chain->subgraphIndex) << 16) | uint32(sourceChannel);

                        // Use output mixer for delay-compensated mixing
                        outputMixer.mixWithDelay(sourceId, src, dst, numSamples);
                    }
                }
            }

            // Collect MIDI from chains connected to MIDI output
            if (chain->connectsToMidiOutput)
                midi.addEvents(chain->getMidiBuffer(), 0, numSamples, 0);
        }

        // Step 6: Add passthrough audio (direct Input → Output connections)
        if (passthroughBuffer)
        {
            const int numChannels = std::min(passthroughBuffer->audioBuffer.getNumChannels(), audio.getNumChannels());
            for (int ch = 0; ch < numChannels; ++ch)
            {
                const auto* src = passthroughBuffer->audioBuffer.getReadPointer(ch);
                auto* dst = audio.getWritePointer(ch);
                FloatVectorOperations::add(dst, src, numSamples);
            }
        }
    }

    int getLatencySamples() const
    {
        return totalLatency;
    }

    PrepareSettings getSettings() const
    {
        return settings;
    }

    // Check if any subgraph's latency has changed since graph build
    // Plugins can change latency at runtime (adaptive algorithms, lookahead, etc.)
    bool hasLatencyChanged() const
    {
        for (size_t i = 0; i < chains.size() && i < subgraphs.size(); ++i)
        {
            const auto& chain = chains[i];
            const auto& subgraph = subgraphs[i];

            int currentLatencySum = 0;
            for (const auto& nodeID : subgraph.nodeIDs)
            {
                if (auto node = nodes.getNodeForId(nodeID))
                    if (auto* proc = node->getProcessor())
                        currentLatencySum += proc->getLatencySamples();
            }

            if (currentLatencySum != chain->latencySum)
            {
                DBG("[PARALLEL] Latency changed in subgraph "
                    << i
                    << ": expected "
                    << chain->latencySum
                    << ", current "
                    << currentLatencySum);
                return true;
            }
        }
        return false;
    }

private:
    void copyAudioToChain(ChainRenderSequence& chain, const AudioBuffer<float>& source, int numSamples)
    {
        const int numChannels = std::min(chain.getAudioBuffer().getNumChannels(), source.getNumChannels());

        for (int ch = 0; ch < numChannels; ++ch)
        {
            const auto* src = source.getReadPointer(ch);
            auto* dst = chain.getAudioBuffer().getWritePointer(ch);
            FloatVectorOperations::copy(dst, src, numSamples);
        }
    }

    void mixAudioFromChain(const ChainRenderSequence& chain, AudioBuffer<float>& dest, int numSamples)
    {
        const int numChannels = std::min(chain.getAudioBuffer().getNumChannels(), dest.getNumChannels());

        for (int ch = 0; ch < numChannels; ++ch)
        {
            const auto* src = chain.getAudioBuffer().getReadPointer(ch);
            auto* dst = dest.getWritePointer(ch);
            FloatVectorOperations::add(dst, src, numSamples);
        }
    }

    PrepareSettings settings;
    const Nodes& nodes; // Reference to graph nodes for latency checking
    std::vector<std::unique_ptr<ChainRenderSequence>> chains;
    std::vector<std::vector<ChainRenderSequence*>> chainsByLevel;
    int maxTopologicalLevel = 0;
    int totalLatency = 0;

    // Store subgraphs for channel routing lookup during process()
    std::vector<GraphPartitioner::Subgraph> subgraphs;

    // Pre-allocated resources for realtime-safe parallel processing
    // These are allocated during graph rebuild (message thread) and reused during process() (audio thread)
    std::vector<atk::ThreadBarrier::Ptr> barriers;                    // One barrier per topological level
    std::vector<std::vector<ChainProcessingJob>> jobsPerLevel;        // Pre-allocated job contexts per level
    std::vector<std::vector<AudioBuffer<float>>> bufferViewsPerLevel; // Pre-allocated buffer views per level

    // I/O node connection mappings (built during construction, read-only during process)
    // Input: map of (chainIndex, destChannel) -> sourceChannel
    // Output: map of (chainIndex, sourceChannel) -> destChannel
    std::map<std::pair<size_t, int>, int> inputChannelMap;
    std::map<std::pair<size_t, int>, int> outputChannelMap;

    // Direct passthrough connections (Audio Input -> Audio Output with no processors)
    // Maps input channel -> output channel
    std::map<int, int> passthroughChannelMap;

    // Store connections vector for channel routing during process()
    std::vector<Connection> connectionsVec;

    // MIDI input mapping: set of chain indices that should receive MIDI input
    std::set<size_t> midiInputChains;

    // MIDI connection mapping: (sourceChainIdx, destChainIdx) pairs that have MIDI connections
    std::set<std::pair<size_t, size_t>> midiChainConnections;

    NodeID audioInputNodeID;
    NodeID audioOutputNodeID;
    NodeID midiInputNodeID;
    NodeID midiOutputNodeID;

    ChainBufferPool& bufferPool;
    DelayLinePool& delayLinePool;

    // Output delay compensation mixer for final mixing to host
    // Handles delay compensation when multiple chains output to the same host output channel
    DelayCompensatingMixer outputMixer;

    // Global node latencies (nodeID.uid -> accumulated latency samples)
    std::unordered_map<uint32, int> nodeLatencies;
};

//==============================================================================
/*  Holds information about the properties of a graph node at the point it was prepared.

    If the bus layout or latency of a given node changes, the graph should be rebuilt so
    that channel connections are ordered correctly, and the graph's internal delay lines have
    the correct delay.
*/
class NodeAttributes
{
    auto tie() const
    {
        return std::tie(layout, latencySamples);
    }

public:
    AudioProcessor::BusesLayout layout;
    int latencySamples = 0;

    bool operator==(const NodeAttributes& other) const
    {
        return tie() == other.tie();
    }

    bool operator!=(const NodeAttributes& other) const
    {
        return tie() != other.tie();
    }
};

//==============================================================================
/*  Holds information about a particular graph configuration, without sharing ownership of any
    graph nodes. Can be checked for equality with other RenderSequenceSignature instances to see
    whether two graph configurations match.
*/
class RenderSequenceSignature
{
    auto tie() const
    {
        return std::tie(settings, connections, nodes);
    }

public:
    RenderSequenceSignature(const PrepareSettings s, const Nodes& n, const Connections& c)
        : settings(s)
        , connections(c)
        , nodes(getNodeMap(n))
    {
    }

    bool operator==(const RenderSequenceSignature& other) const
    {
        return tie() == other.tie();
    }

    bool operator!=(const RenderSequenceSignature& other) const
    {
        return tie() != other.tie();
    }

private:
    using NodeMap = std::map<AudioProcessorGraphMT::NodeID, NodeAttributes>;

    static NodeMap getNodeMap(const Nodes& n)
    {
        const auto& nodeRefs = n.getNodes();
        NodeMap result;

        for (const auto& node : nodeRefs)
        {
            auto* proc = node->getProcessor();
            result.emplace(node->nodeID, NodeAttributes{proc->getBusesLayout(), proc->getLatencySamples()});
        }

        return result;
    }

    PrepareSettings settings;
    Connections connections;
    NodeMap nodes;
};

//==============================================================================
/*  Facilitates wait-free render-sequence updates.

    Topology updates always happen on the main thread (or synchronised with the main thread).
    After updating the graph, the 'baked' graph is passed to RenderSequenceExchange::set.
    At the top of the audio callback, RenderSequenceExchange::updateAudioThreadState will
    attempt to install the most-recently-baked graph, if there's one waiting.
*/
class RenderSequenceExchange final : private Timer
{
public:
    RenderSequenceExchange()
    {
        startTimer(500);
    }

    ~RenderSequenceExchange() override
    {
        stopTimer();
    }

    void set(std::unique_ptr<ParallelRenderSequence>&& next)
    {
        const SpinLock::ScopedLockType lock(mutex);
        mainThreadState = std::move(next);
        isNew = true;
    }

    /*  Call from the audio thread only. */
    void updateAudioThreadState()
    {
        const SpinLock::ScopedTryLockType lock(mutex);

        if (lock.isLocked() && isNew)
        {
            // Swap pointers rather than assigning to avoid calling delete here
            std::swap(mainThreadState, audioThreadState);
            isNew = false;
        }
    }

    /*  Call from the audio thread only. */
    ParallelRenderSequence* getAudioThreadState() const
    {
        return audioThreadState.get();
    }

private:
    void timerCallback() override
    {
        const SpinLock::ScopedLockType lock(mutex);

        if (!isNew)
            mainThreadState.reset();
    }

    SpinLock mutex;
    std::unique_ptr<ParallelRenderSequence> mainThreadState, audioThreadState;
    bool isNew = false;
};

//==============================================================================
class AudioProcessorGraphMT::Pimpl
{
public:
    explicit Pimpl(AudioProcessorGraphMT& o)
        : owner(&o)
    {
    }

    const auto& getNodes() const
    {
        return nodes.getNodes();
    }

    void clear(UpdateKind updateKind)
    {
        if (getNodes().isEmpty())
            return;

        nodes = Nodes{};
        connections = Connections{};
        nodeStates.clear();
        topologyChanged(updateKind);
    }

    auto getNodeForId(NodeID nodeID) const
    {
        return nodes.getNodeForId(nodeID);
    }

    Node::Ptr addNode(std::unique_ptr<AudioProcessor> newProcessor, std::optional<NodeID> nodeID, UpdateKind updateKind)
    {
        if (newProcessor.get() == owner)
        {
            jassertfalse;
            return nullptr;
        }

        const auto idToUse = nodeID.value_or(NodeID{lastNodeID.uid + 1});

        auto added = nodes.addNode(std::move(newProcessor), idToUse);

        if (added == nullptr)
            return nullptr;

        if (lastNodeID < idToUse)
            lastNodeID = idToUse;

        setParentGraph(added->getProcessor());

        topologyChanged(updateKind);
        return added;
    }

    Node::Ptr removeNode(NodeID nodeID, UpdateKind updateKind)
    {
        connections.disconnectNode(nodeID);
        auto result = nodes.removeNode(nodeID);
        nodeStates.removeNode(nodeID);
        topologyChanged(updateKind);
        return result;
    }

    std::vector<Connection> getConnections() const
    {
        return connections.getConnections();
    }

    bool isConnected(const Connection& c) const
    {
        return connections.isConnected(c);
    }

    bool isConnected(NodeID srcID, NodeID destID) const
    {
        return connections.isConnected(srcID, destID);
    }

    bool isAnInputTo(const Node& src, const Node& dst) const
    {
        return isAnInputTo(src.nodeID, dst.nodeID);
    }

    bool isAnInputTo(NodeID src, NodeID dst) const
    {
        return connections.isAnInputTo(src, dst);
    }

    bool canConnect(const Connection& c) const
    {
        return connections.canConnect(nodes, c);
    }

    bool addConnection(const Connection& c, UpdateKind updateKind)
    {
        if (!connections.addConnection(nodes, c))
            return false;

        jassert(isConnected(c));
        topologyChanged(updateKind);
        return true;
    }

    bool removeConnection(const Connection& c, UpdateKind updateKind)
    {
        if (!connections.removeConnection(c))
            return false;

        topologyChanged(updateKind);
        return true;
    }

    bool disconnectNode(NodeID nodeID, UpdateKind updateKind)
    {
        if (!connections.disconnectNode(nodeID))
            return false;

        topologyChanged(updateKind);
        return true;
    }

    bool isConnectionLegal(const Connection& c) const
    {
        return connections.isConnectionLegal(nodes, c);
    }

    bool removeIllegalConnections(UpdateKind updateKind)
    {
        const auto result = connections.removeIllegalConnections(nodes);
        topologyChanged(updateKind);
        return result;
    }

    //==============================================================================
    void prepareToPlay(double sampleRate, int estimatedSamplesPerBlock)
    {
        owner->setRateAndBufferSizeDetails(sampleRate, estimatedSamplesPerBlock);

        PrepareSettings settings;
        settings.sampleRate = sampleRate;
        settings.blockSize = estimatedSamplesPerBlock;

        nodeStates.setState(settings);

        // Initialize and configure thread pool for parallel processing
        // This happens lazily on first prepareToPlay call
        auto* pool = atk::AudioThreadPool::getInstance();
        if (pool && !pool->isReady())
        {
            // Auto-detect optimal worker count (reserve 1 core for main audio thread, 1 for system)
            const int physicalCores = juce::SystemStats::getNumPhysicalCpus();
            const int numWorkers = juce::jmax(1, physicalCores - 2);

            pool->initialize(numWorkers, 8); // Priority 8 for realtime audio

            DBG("AudioProcessorGraphMT: Initialized thread pool with "
                << numWorkers
                << " workers (CPU cores: "
                << physicalCores
                << ")");
        }

        // Configure thread pool with buffer size and sample rate for adaptive backoff
        if (pool)
            pool->configure(estimatedSamplesPerBlock, sampleRate);

        topologyChanged(UpdateKind::sync);
    }

    void releaseResources()
    {
        nodeStates.setState(nullopt);
        topologyChanged(UpdateKind::sync);
    }

    void rebuild(UpdateKind updateKind)
    {
        if (updateKind == UpdateKind::none)
            return;

        if (updateKind == UpdateKind::sync && MessageManager::getInstance()->isThisTheMessageThread())
            handleAsyncUpdate();
        else
            updater.triggerAsyncUpdate();
    }

    void reset()
    {
        for (auto* n : getNodes())
            n->getProcessor()->reset();
    }

    void setNonRealtime(bool isProcessingNonRealtime)
    {
        for (auto* n : getNodes())
            n->getProcessor()->setNonRealtime(isProcessingNonRealtime);
    }

    void processBlock(AudioBuffer<float>& audio, MidiBuffer& midi, AudioPlayHead* playHead)
    {
        renderSequenceExchange.updateAudioThreadState();

        if (renderSequenceExchange.getAudioThreadState() == nullptr
            && MessageManager::getInstance()->isThisTheMessageThread())
            handleAsyncUpdate();

        if (owner->isNonRealtime())
        {
            while (renderSequenceExchange.getAudioThreadState() == nullptr)
            {
                Thread::sleep(1);
                renderSequenceExchange.updateAudioThreadState();
            }
        }

        auto* state = renderSequenceExchange.getAudioThreadState();

        if (state != nullptr && state->getSettings() == nodeStates.getLastRequestedSettings())
        {
            state->process(audio, midi, playHead);

            // Detect runtime latency changes and trigger rebuild if needed
            if (state->hasLatencyChanged())
                updater.triggerAsyncUpdate();
        }
        else
        {
            audio.clear();
            midi.clear();
        }
    }

    /*  Call from the audio thread only. */
    auto* getAudioThreadState() const
    {
        return renderSequenceExchange.getAudioThreadState();
    }

private:
    void setParentGraph(AudioProcessor* p) const
    {
        if (auto* ioProc = dynamic_cast<AudioGraphIOProcessor*>(p))
            ioProc->setParentGraph(owner);
    }

    void topologyChanged(UpdateKind updateKind)
    {
        owner->sendChangeMessage();
        rebuild(updateKind);
    }

    void handleAsyncUpdate()
    {
        if (const auto newSettings = nodeStates.applySettings(nodes))
        {
            for (const auto node : nodes.getNodes())
                setParentGraph(node->getProcessor());

            const RenderSequenceSignature newSignature(*newSettings, nodes, connections);

            if (std::exchange(lastBuiltSequence, newSignature) != newSignature)
            {
                auto sequence = std::make_unique<ParallelRenderSequence>(
                    *newSettings,
                    *owner,
                    nodes,
                    connections,
                    bufferPool,
                    delayLinePool
                );
                owner->setLatencySamples(sequence->getLatencySamples());
                renderSequenceExchange.set(std::move(sequence));
            }
        }
        else
        {
            lastBuiltSequence.reset();
            renderSequenceExchange.set(nullptr);
        }
    }

    AudioProcessorGraphMT* owner = nullptr;
    Nodes nodes;
    Connections connections;
    NodeStates nodeStates;
    RenderSequenceExchange renderSequenceExchange;
    ChainBufferPool bufferPool;  // Persistent buffer pool for reusing chain buffers across rebuilds
    DelayLinePool delayLinePool; // Persistent delay line pool for delay compensation across rebuilds
    NodeID lastNodeID;
    std::optional<RenderSequenceSignature> lastBuiltSequence;
    LockingAsyncUpdater updater{[this] { handleAsyncUpdate(); }};
};

//==============================================================================
AudioProcessorGraphMT::AudioProcessorGraphMT()
    : pimpl(std::make_unique<Pimpl>(*this))
{
}

AudioProcessorGraphMT::~AudioProcessorGraphMT() = default;

const String AudioProcessorGraphMT::getName() const
{
    return "Audio Graph";
}

bool AudioProcessorGraphMT::supportsDoublePrecisionProcessing() const
{
    return false;
}

double AudioProcessorGraphMT::getTailLengthSeconds() const
{
    return 0;
}

bool AudioProcessorGraphMT::acceptsMidi() const
{
    return true;
}

bool AudioProcessorGraphMT::producesMidi() const
{
    return true;
}

void AudioProcessorGraphMT::getStateInformation(MemoryBlock&)
{
}

void AudioProcessorGraphMT::setStateInformation(const void*, int)
{
}

void AudioProcessorGraphMT::processBlock(AudioBuffer<float>& audio, MidiBuffer& midi)
{
    if (!midi.isEmpty())
        DBG("[AudioProcessorGraphMT::processBlock] Received MIDI: " << midi.getNumEvents() << " events");

    return pimpl->processBlock(audio, midi, getPlayHead());
}

std::vector<AudioProcessorGraphMT::Connection> AudioProcessorGraphMT::getConnections() const
{
    return pimpl->getConnections();
}

bool AudioProcessorGraphMT::addConnection(const Connection& c, UpdateKind updateKind)
{
    return pimpl->addConnection(c, updateKind);
}

bool AudioProcessorGraphMT::removeConnection(const Connection& c, UpdateKind updateKind)
{
    return pimpl->removeConnection(c, updateKind);
}

void AudioProcessorGraphMT::prepareToPlay(double sampleRate, int estimatedSamplesPerBlock)
{
    return pimpl->prepareToPlay(sampleRate, estimatedSamplesPerBlock);
}

void AudioProcessorGraphMT::clear(UpdateKind updateKind)
{
    return pimpl->clear(updateKind);
}

const ReferenceCountedArray<AudioProcessorGraphMT::Node>& AudioProcessorGraphMT::getNodes() const noexcept
{
    return pimpl->getNodes();
}

AudioProcessorGraphMT::Node* AudioProcessorGraphMT::getNodeForId(NodeID x) const
{
    return pimpl->getNodeForId(x).get();
}

bool AudioProcessorGraphMT::disconnectNode(NodeID nodeID, UpdateKind updateKind)
{
    return pimpl->disconnectNode(nodeID, updateKind);
}

void AudioProcessorGraphMT::releaseResources()
{
    return pimpl->releaseResources();
}

bool AudioProcessorGraphMT::removeIllegalConnections(UpdateKind updateKind)
{
    return pimpl->removeIllegalConnections(updateKind);
}

void AudioProcessorGraphMT::rebuild()
{
    return pimpl->rebuild(UpdateKind::sync);
}

void AudioProcessorGraphMT::reset()
{
    return pimpl->reset();
}

bool AudioProcessorGraphMT::canConnect(const Connection& c) const
{
    return pimpl->canConnect(c);
}

bool AudioProcessorGraphMT::isConnected(const Connection& c) const noexcept
{
    return pimpl->isConnected(c);
}

bool AudioProcessorGraphMT::isConnected(NodeID a, NodeID b) const noexcept
{
    return pimpl->isConnected(a, b);
}

bool AudioProcessorGraphMT::isConnectionLegal(const Connection& c) const
{
    return pimpl->isConnectionLegal(c);
}

bool AudioProcessorGraphMT::isAnInputTo(const Node& source, const Node& destination) const noexcept
{
    return pimpl->isAnInputTo(source, destination);
}

bool AudioProcessorGraphMT::isAnInputTo(NodeID source, NodeID destination) const noexcept
{
    return pimpl->isAnInputTo(source, destination);
}

AudioProcessorGraphMT::Node::Ptr AudioProcessorGraphMT::addNode(
    std::unique_ptr<AudioProcessor> newProcessor,
    std::optional<NodeID> nodeId,
    UpdateKind updateKind
)
{
    return pimpl->addNode(std::move(newProcessor), nodeId, updateKind);
}

void AudioProcessorGraphMT::setNonRealtime(bool isProcessingNonRealtime) noexcept
{
    AudioProcessor::setNonRealtime(isProcessingNonRealtime);
    pimpl->setNonRealtime(isProcessingNonRealtime);
}

AudioProcessorGraphMT::Node::Ptr AudioProcessorGraphMT::removeNode(NodeID nodeID, UpdateKind updateKind)
{
    return pimpl->removeNode(nodeID, updateKind);
}

AudioProcessorGraphMT::Node::Ptr AudioProcessorGraphMT::removeNode(Node* node, UpdateKind updateKind)
{
    if (node != nullptr)
        return removeNode(node->nodeID, updateKind);

    jassertfalse;
    return {};
}

//==============================================================================
AudioProcessorGraphMT::AudioGraphIOProcessor::AudioGraphIOProcessor(const IODeviceType deviceType)
    : type(deviceType)
{
}

AudioProcessorGraphMT::AudioGraphIOProcessor::~AudioGraphIOProcessor() = default;

const String AudioProcessorGraphMT::AudioGraphIOProcessor::getName() const
{
    switch (type)
    {
    case audioOutputNode:
        return "Audio Output";
    case audioInputNode:
        return "Audio Input";
    case midiOutputNode:
        return "MIDI Output";
    case midiInputNode:
        return "MIDI Input";
    default:
        break;
    }

    return {};
}

void AudioProcessorGraphMT::AudioGraphIOProcessor::fillInPluginDescription(PluginDescription& d) const
{
    d.name = getName();
    d.category = "I/O devices";
    d.pluginFormatName = "Internal";
    d.manufacturerName = "JUCE";
    d.version = "1.0";
    d.isInstrument = false;

    d.deprecatedUid = d.uniqueId = d.name.hashCode();

    d.numInputChannels = getTotalNumInputChannels();

    if (type == audioOutputNode && graph != nullptr)
        d.numInputChannels = graph->getTotalNumInputChannels();

    d.numOutputChannels = getTotalNumOutputChannels();

    if (type == audioInputNode && graph != nullptr)
        d.numOutputChannels = graph->getTotalNumOutputChannels();
}

void AudioProcessorGraphMT::AudioGraphIOProcessor::prepareToPlay(double, int)
{
    jassert(graph != nullptr);
}

void AudioProcessorGraphMT::AudioGraphIOProcessor::releaseResources()
{
}

bool AudioProcessorGraphMT::AudioGraphIOProcessor::supportsDoublePrecisionProcessing() const
{
    return false;
}

void AudioProcessorGraphMT::AudioGraphIOProcessor::processBlock(AudioBuffer<float>&, MidiBuffer&)
{
    // The graph should never call this!
    jassertfalse;
}

double AudioProcessorGraphMT::AudioGraphIOProcessor::getTailLengthSeconds() const
{
    return 0;
}

bool AudioProcessorGraphMT::AudioGraphIOProcessor::acceptsMidi() const
{
    return type == midiOutputNode;
}

bool AudioProcessorGraphMT::AudioGraphIOProcessor::producesMidi() const
{
    return type == midiInputNode;
}

bool AudioProcessorGraphMT::AudioGraphIOProcessor::isInput() const noexcept
{
    return type == audioInputNode || type == midiInputNode;
}

bool AudioProcessorGraphMT::AudioGraphIOProcessor::isOutput() const noexcept
{
    return type == audioOutputNode || type == midiOutputNode;
}

bool AudioProcessorGraphMT::AudioGraphIOProcessor::hasEditor() const
{
    return false;
}

AudioProcessorEditor* AudioProcessorGraphMT::AudioGraphIOProcessor::createEditor()
{
    return nullptr;
}

int AudioProcessorGraphMT::AudioGraphIOProcessor::getNumPrograms()
{
    return 0;
}

int AudioProcessorGraphMT::AudioGraphIOProcessor::getCurrentProgram()
{
    return 0;
}

void AudioProcessorGraphMT::AudioGraphIOProcessor::setCurrentProgram(int)
{
}

const String AudioProcessorGraphMT::AudioGraphIOProcessor::getProgramName(int)
{
    return {};
}

void AudioProcessorGraphMT::AudioGraphIOProcessor::changeProgramName(int, const String&)
{
}

void AudioProcessorGraphMT::AudioGraphIOProcessor::getStateInformation(MemoryBlock&)
{
}

void AudioProcessorGraphMT::AudioGraphIOProcessor::setStateInformation(const void*, int)
{
}

void AudioProcessorGraphMT::AudioGraphIOProcessor::setParentGraph(AudioProcessorGraphMT* const newGraph)
{
    graph = newGraph;

    if (graph == nullptr)
        return;

    setPlayConfigDetails(
        type == audioOutputNode ? newGraph->getTotalNumOutputChannels() : 0,
        type == audioInputNode ? newGraph->getTotalNumInputChannels() : 0,
        getSampleRate(),
        getBlockSize()
    );

    updateHostDisplay();
}

//==============================================================================
//==============================================================================
#if JUCE_UNIT_TESTS

class AudioProcessorGraphTests final : public UnitTest
{
public:
    AudioProcessorGraphTests()
        : UnitTest("AudioProcessorGraphMT", UnitTestCategories::audioProcessors)
    {
    }

    void runTest() override
    {
        const ScopedJuceInitialiser_GUI scope;

        const auto midiChannel = AudioProcessorGraphMT::midiChannelIndex;

        beginTest("isConnected returns true when two nodes are connected");
        {
            AudioProcessorGraphMT graph;
            const auto nodeA = graph.addNode(BasicProcessor::make({}, MidiIn::no, MidiOut::yes))->nodeID;
            const auto nodeB = graph.addNode(BasicProcessor::make({}, MidiIn::yes, MidiOut::no))->nodeID;

            expect(graph.canConnect({
                {nodeA, midiChannel},
                {nodeB, midiChannel}
            }));
            expect(!graph.canConnect({
                {nodeB, midiChannel},
                {nodeA, midiChannel}
            }));
            expect(!graph.canConnect({
                {nodeA, midiChannel},
                {nodeA, midiChannel}
            }));
            expect(!graph.canConnect({
                {nodeB, midiChannel},
                {nodeB, midiChannel}
            }));

            expect(graph.getConnections().empty());
            expect(!graph.isConnected({
                {nodeA, midiChannel},
                {nodeB, midiChannel}
            }));
            expect(!graph.isConnected(nodeA, nodeB));

            expect(graph.addConnection({
                {nodeA, midiChannel},
                {nodeB, midiChannel}
            }));

            expect(graph.getConnections().size() == 1);
            expect(graph.isConnected({
                {nodeA, midiChannel},
                {nodeB, midiChannel}
            }));
            expect(graph.isConnected(nodeA, nodeB));

            expect(graph.disconnectNode(nodeA));

            expect(graph.getConnections().empty());
            expect(!graph.isConnected({
                {nodeA, midiChannel},
                {nodeB, midiChannel}
            }));
            expect(!graph.isConnected(nodeA, nodeB));
        }

        beginTest("graph lookups work with a large number of connections");
        {
            AudioProcessorGraphMT graph;

            std::vector<AudioProcessorGraphMT::NodeID> nodeIDs;

            constexpr auto numNodes = 100;

            for (auto i = 0; i < numNodes; ++i)
            {
                nodeIDs.push_back(
                    graph
                        .addNode(BasicProcessor::make(BasicProcessor::getStereoProperties(), MidiIn::yes, MidiOut::yes))
                        ->nodeID
                );
            }

            for (auto it = nodeIDs.begin(); it != std::prev(nodeIDs.end()); ++it)
            {
                expect(graph.addConnection({
                    {it[0], 0},
                    {it[1], 0}
                }));
                expect(graph.addConnection({
                    {it[0], 1},
                    {it[1], 1}
                }));
            }

            // Check whether isConnected reports correct results when called
            // with both connections and nodes
            for (auto it = nodeIDs.begin(); it != std::prev(nodeIDs.end()); ++it)
            {
                expect(graph.isConnected({
                    {it[0], 0},
                    {it[1], 0}
                }));
                expect(graph.isConnected({
                    {it[0], 1},
                    {it[1], 1}
                }));
                expect(graph.isConnected(it[0], it[1]));
            }

            const auto& nodes = graph.getNodes();

            expect(!graph.isAnInputTo(*nodes[0], *nodes[0]));

            // Check whether isAnInputTo behaves correctly for a non-cyclic graph
            for (auto it = std::next(nodes.begin()); it != std::prev(nodes.end()); ++it)
            {
                expect(!graph.isAnInputTo(**it, **it));

                expect(graph.isAnInputTo(*nodes[0], **it));
                expect(!graph.isAnInputTo(**it, *nodes[0]));

                expect(graph.isAnInputTo(**it, *nodes[nodes.size() - 1]));
                expect(!graph.isAnInputTo(*nodes[nodes.size() - 1], **it));
            }

            // Make the graph cyclic
            graph.addConnection({
                { nodeIDs.back(), 0},
                {nodeIDs.front(), 0}
            });
            graph.addConnection({
                { nodeIDs.back(), 1},
                {nodeIDs.front(), 1}
            });

            // Check whether isAnInputTo behaves correctly for a cyclic graph
            for (const auto* node : graph.getNodes())
            {
                expect(graph.isAnInputTo(*node, *node));

                expect(graph.isAnInputTo(*nodes[0], *node));
                expect(graph.isAnInputTo(*node, *nodes[0]));

                expect(graph.isAnInputTo(*node, *nodes[nodes.size() - 1]));
                expect(graph.isAnInputTo(*nodes[nodes.size() - 1], *node));
            }
        }

        beginTest("rebuilding the graph recalculates overall latency");
        {
            AudioProcessorGraphMT graph;

            const auto nodeA =
                graph.addNode(BasicProcessor::make(BasicProcessor::getStereoProperties(), MidiIn::no, MidiOut::no))
                    ->nodeID;
            const auto nodeB =
                graph.addNode(BasicProcessor::make(BasicProcessor::getStereoProperties(), MidiIn::no, MidiOut::no))
                    ->nodeID;
            const auto final =
                graph.addNode(BasicProcessor::make(BasicProcessor::getInputOnlyProperties(), MidiIn::no, MidiOut::no))
                    ->nodeID;

            expect(graph.addConnection({
                {nodeA, 0},
                {nodeB, 0}
            }));
            expect(graph.addConnection({
                {nodeA, 1},
                {nodeB, 1}
            }));
            expect(graph.addConnection({
                {nodeB, 0},
                {final, 0}
            }));
            expect(graph.addConnection({
                {nodeB, 1},
                {final, 1}
            }));

            expect(graph.getLatencySamples() == 0);

            // Graph isn't built, latency is 0 if prepareToPlay hasn't been called yet
            const auto nodeALatency = 100;
            graph.getNodeForId(nodeA)->getProcessor()->setLatencySamples(nodeALatency);
            graph.rebuild();
            expect(graph.getLatencySamples() == 0);

            graph.prepareToPlay(44100, 512);

            expect(graph.getLatencySamples() == nodeALatency);

            const auto nodeBLatency = 200;
            graph.getNodeForId(nodeB)->getProcessor()->setLatencySamples(nodeBLatency);
            graph.rebuild();
            expect(graph.getLatencySamples() == nodeALatency + nodeBLatency);

            const auto finalLatency = 300;
            graph.getNodeForId(final)->getProcessor()->setLatencySamples(finalLatency);
            graph.rebuild();
            expect(graph.getLatencySamples() == nodeALatency + nodeBLatency + finalLatency);
        }

        beginTest("nodes use double precision if supported");
        {
            AudioProcessorGraphMT graph;
            constexpr auto blockSize = 512;
            AudioBuffer<float> bufferFloat(2, blockSize);
            AudioBuffer<double> bufferDouble(2, blockSize);
            MidiBuffer midi;

            auto processorOwner = BasicProcessor::make(BasicProcessor::getStereoProperties(), MidiIn::no, MidiOut::no);
            auto* processor = processorOwner.get();
            graph.addNode(std::move(processorOwner));

            // Process in single-precision
            {
                graph.setProcessingPrecision(AudioProcessor::singlePrecision);
                graph.prepareToPlay(44100.0, blockSize);

                graph.processBlock(bufferFloat, midi);
                expect(processor->getProcessingPrecision() == AudioProcessor::singlePrecision);
                expect(processor->getLastBlockPrecision() == AudioProcessor::singlePrecision);

                graph.releaseResources();
            }

            // Process in double-precision
            {
                graph.setProcessingPrecision(AudioProcessor::doublePrecision);
                graph.prepareToPlay(44100.0, blockSize);

                graph.processBlock(bufferDouble, midi);
                expect(processor->getProcessingPrecision() == AudioProcessor::doublePrecision);
                expect(processor->getLastBlockPrecision() == AudioProcessor::doublePrecision);

                graph.releaseResources();
            }

            // Process in double-precision when node only supports single-precision
            {
                processor->setSupportsDoublePrecisionProcessing(false);

                graph.setProcessingPrecision(AudioProcessor::doublePrecision);
                graph.prepareToPlay(44100.0, blockSize);

                graph.processBlock(bufferDouble, midi);
                expect(processor->getProcessingPrecision() == AudioProcessor::singlePrecision);
                expect(processor->getLastBlockPrecision() == AudioProcessor::singlePrecision);

                graph.releaseResources();
            }

            // It's not possible for the node to *only* support double-precision.
            // It's also not possible to prepare the graph in single-precision mode, and then
            // to set an individual node into double-precision mode. This would require calling
            // prepareToPlay() on an individual node after preparing the graph as a whole, which is
            // not a supported usage pattern.
        }

        beginTest(
            "When a delayed channel is used as an input to multiple nodes, the delay is applied appropriately for each "
            "node"
        );
        {
            AudioProcessorGraphMT graph;
            graph.setBusesLayout({{AudioChannelSet::stereo()}, {AudioChannelSet::mono()}});

            const auto nodeA =
                graph.addNode(BasicProcessor::make(BasicProcessor::getStereoInMonoOut(), MidiIn::no, MidiOut::no));
            const auto nodeB =
                graph.addNode(BasicProcessor::make(BasicProcessor::getStereoInMonoOut(), MidiIn::no, MidiOut::no));
            const auto nodeC =
                graph.addNode(BasicProcessor::make(BasicProcessor::getStereoInMonoOut(), MidiIn::no, MidiOut::no));
            const auto input = graph.addNode(
                std::make_unique<AudioProcessorGraphMT::AudioGraphIOProcessor>(
                    AudioProcessorGraphMT::AudioGraphIOProcessor::IODeviceType::audioInputNode
                )
            );
            const auto output = graph.addNode(
                std::make_unique<AudioProcessorGraphMT::AudioGraphIOProcessor>(
                    AudioProcessorGraphMT::AudioGraphIOProcessor::IODeviceType::audioOutputNode
                )
            );

            constexpr auto latencySamples = 2;
            nodeA->getProcessor()->setLatencySamples(latencySamples);

            // [input 0    1]   0 and 1 denote input/output channels
            //        |    |
            //        |    |
            // [nodeA 0 1] |    nodeA has latency applied
            //        |   /|
            //        |  / |
            // [nodeB 0 1] |    each node sums all input channels onto the first output channel
            //        |   /
            //        |  /
            // [nodeC 0 1]
            //        |
            //        |
            //   [out 0]

            expect(graph.addConnection({
                {input->nodeID, 0},
                {nodeA->nodeID, 0}
            }));
            expect(graph.addConnection({
                {input->nodeID, 1},
                {nodeB->nodeID, 1}
            }));
            expect(graph.addConnection({
                {input->nodeID, 1},
                {nodeC->nodeID, 1}
            }));

            expect(graph.addConnection({
                {nodeA->nodeID, 0},
                {nodeB->nodeID, 0}
            }));
            expect(graph.addConnection({
                {nodeB->nodeID, 0},
                {nodeC->nodeID, 0}
            }));

            expect(graph.addConnection({
                { nodeC->nodeID, 0},
                {output->nodeID, 0}
            }));

            graph.rebuild();

            constexpr auto blockSize = 128;
            graph.prepareToPlay(44100.0, blockSize);
            expect(graph.getLatencySamples() == latencySamples);

            AudioBuffer<float> audio(2, blockSize);
            audio.clear();
            audio.setSample(1, 0, 1.0f);

            MidiBuffer midi;
            graph.processBlock(audio, midi);

            // The impulse should arrive at nodes B and C simultaneously, so the end result should
            // be a double-amplitude impulse with the latency of node A applied

            for (auto i = 0; i < blockSize; ++i)
            {
                const auto expected = i == latencySamples ? 2.0f : 0.0f;
                expect(exactlyEqual(audio.getSample(0, i), expected));
            }
        }

        beginTest("large render sequence can be built");
        {
            AudioProcessorGraphMT graph;

            std::vector<AudioProcessorGraphMT::NodeID> nodeIDs;

            constexpr auto numNodes = 1000;
            constexpr auto numChannels = 100;

            for (auto i = 0; i < numNodes; ++i)
            {
                nodeIDs.push_back(graph
                                      .addNode(
                                          BasicProcessor::make(
                                              BasicProcessor::getMultichannelProperties(numChannels),
                                              MidiIn::yes,
                                              MidiOut::yes
                                          )
                                      )
                                      ->nodeID);
            }

            for (auto it = nodeIDs.begin(); it != std::prev(nodeIDs.end()); ++it)
                for (auto channel = 0; channel < numChannels; ++channel)
                    expect(graph.addConnection({
                        {it[0], channel},
                        {it[1], channel}
                    }));

            const auto b = std::chrono::steady_clock::now();
            graph.prepareToPlay(44100.0, 512);
            const auto e = std::chrono::steady_clock::now();
            const auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(e - b).count();

            // No test here, but older versions of the graph would take forever to complete building
            // this graph, so we just want to make sure that we finish the test without timing out.
            DBG("render sequence built in " + String(duration) + " ms");
        }
    }

private:
    enum class MidiIn
    {
        no,
        yes
    };
    enum class MidiOut
    {
        no,
        yes
    };

    class BasicProcessor final : public AudioProcessor
    {
    public:
        explicit BasicProcessor(const BusesProperties& layout, MidiIn mIn, MidiOut mOut)
            : AudioProcessor(layout)
            , midiIn(mIn)
            , midiOut(mOut)
        {
        }

        const String getName() const override
        {
            return "Basic Processor";
        }

        double getTailLengthSeconds() const override
        {
            return {};
        }

        bool acceptsMidi() const override
        {
            return midiIn == MidiIn ::yes;
        }

        bool producesMidi() const override
        {
            return midiOut == MidiOut::yes;
        }

        AudioProcessorEditor* createEditor() override
        {
            return {};
        }

        bool hasEditor() const override
        {
            return {};
        }

        int getNumPrograms() override
        {
            return 1;
        }

        int getCurrentProgram() override
        {
            return {};
        }

        void setCurrentProgram(int) override
        {
        }

        const String getProgramName(int) override
        {
            return {};
        }

        void changeProgramName(int, const String&) override
        {
        }

        void getStateInformation(MemoryBlock&) override
        {
        }

        void setStateInformation(const void*, int) override
        {
        }

        void prepareToPlay(double, int) override
        {
        }

        void releaseResources() override
        {
        }

        bool supportsDoublePrecisionProcessing() const override
        {
            return doublePrecisionSupported;
        }

        bool isMidiEffect() const override
        {
            return {};
        }

        void reset() override
        {
        }

        void setNonRealtime(bool) noexcept override
        {
        }

        void processBlock(AudioBuffer<float>& audio, MidiBuffer&) override
        {
            blockPrecision = singlePrecision;

            for (auto i = 1; i < audio.getNumChannels(); ++i)
                audio.addFrom(0, 0, audio.getReadPointer(i), audio.getNumSamples());
        }

        void processBlock(AudioBuffer<double>& audio, MidiBuffer&) override
        {
            blockPrecision = doublePrecision;

            for (auto i = 1; i < audio.getNumChannels(); ++i)
                audio.addFrom(0, 0, audio.getReadPointer(i), audio.getNumSamples());
        }

        static std::unique_ptr<BasicProcessor> make(const BusesProperties& layout, MidiIn midiIn, MidiOut midiOut)
        {
            return std::make_unique<BasicProcessor>(layout, midiIn, midiOut);
        }

        static BusesProperties getInputOnlyProperties()
        {
            return BusesProperties().withInput("in", AudioChannelSet::stereo());
        }

        static BusesProperties getStereoProperties()
        {
            return BusesProperties()
                .withInput("in", AudioChannelSet::stereo())
                .withOutput("out", AudioChannelSet::stereo());
        }

        static BusesProperties getStereoInMonoOut()
        {
            return BusesProperties()
                .withInput("in", AudioChannelSet::stereo())
                .withOutput("out", AudioChannelSet::mono());
        }

        static BusesProperties getMultichannelProperties(int numChannels)
        {
            return BusesProperties()
                .withInput("in", AudioChannelSet::discreteChannels(numChannels))
                .withOutput("out", AudioChannelSet::discreteChannels(numChannels));
        }

        void setSupportsDoublePrecisionProcessing(bool x)
        {
            doublePrecisionSupported = x;
        }

        ProcessingPrecision getLastBlockPrecision() const
        {
            return blockPrecision;
        }

    private:
        MidiIn midiIn;
        MidiOut midiOut;
        ProcessingPrecision blockPrecision = ProcessingPrecision(-1); // initially invalid
        bool doublePrecisionSupported = true;
    };
};

static AudioProcessorGraphTests audioProcessorGraphTests;

#endif

} // namespace atk
