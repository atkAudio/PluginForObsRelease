#include "AudioProcessorGraphMT.h"

#include "SubgraphExtractor.h"
#include "RealtimeThreadPool.h"
#include "DependencyTaskGraph.h"

#include <juce_dsp/juce_dsp.h>

// Pre-allocated buffer configuration for chain processing
#define CHAIN_MAX_CHANNELS 64 // Maximum audio channels per chain

#define AUDIO_INPUT_SOURCE_ID (UINT32_MAX - 1)
using namespace juce;

namespace atk
{

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
#ifdef ATK_DEBUG
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
#endif

        if (!canConnect(n, c))
        {
#ifdef ATK_DEBUG
            DBG("  canConnect returned FALSE");
            Logger::writeToLog("  canConnect returned FALSE");
#endif
            return false;
        }

        sourcesForDestination[c.destination].insert(c.source);

#ifdef ATK_DEBUG
        String countMsg = "  Connection added. Total connections: " + String(getConnections().size());
        DBG(countMsg);
        Logger::writeToLog(countMsg);
#endif
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
        const std::vector<NodeID>& nodeFilter,
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

    static Array<Node*>
    createOrderedNodeList(const Nodes& n, const Connections& c, const std::vector<NodeID>* nodeFilter)
    {
        Array<Node*> result;

        std::map<NodeID, std::set<NodeID>> nodeParents;

        for (auto& node : n.getNodes())
        {
            const auto nodeID = node->nodeID;

            // Skip I/O nodes - we handle input/output externally in perform()
            if (dynamic_cast<AudioProcessorGraphMT::AudioGraphIOProcessor*>(node->getProcessor()))
                continue;

            // Skip nodes not in filter (if filter is provided)
            if (nodeFilter && std::find(nodeFilter->begin(), nodeFilter->end(), nodeID) == nodeFilter->end())
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

    // Filtered constructor for chains - handles proper channel routing between nodes
    // Supports 1-to-1, 1-to-many, and many-to-1 channel mappings via copy/add ops
    RenderSequenceBuilder(
        const Nodes& n,
        const Connections& c,
        GraphRenderSequence& sequence,
        const std::vector<NodeID>& nodeFilter,
        const std::unordered_map<uint32, int>& globalDelays
    )
        : orderedNodes(createOrderedNodeList(n, c, &nodeFilter))
    {
        // For cross-subgraph delay compensation:
        // - Use globalDelays to initialize delays map with accumulated latencies from OTHER subgraphs
        // - Within the subgraph, delays accumulate naturally as we process nodes in order
        delays = globalDelays;

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

        // Reserve one extra channel as temp buffer for routing operations
        // This avoids overwriting source data when dest channel == source channel of another connection
        const int tempBufferChannel = maxChannelsNeeded;
        maxChannelsNeeded += 1;

        // Track which channel holds output from each node's output channel
        // Key: {nodeID, channelIndex}, Value: buffer channel index
        std::map<std::pair<uint32, int>, int> outputChannelMap;

        // Second pass: create rendering ops with proper channel routing
        int midiBufferIndex = 0;

        for (int i = 0; i < orderedNodes.size(); ++i)
        {
            auto* node = orderedNodes.getUnchecked(i);
            auto& processor = *node->getProcessor();
            auto numIns = processor.getTotalNumInputChannels();
            auto numOuts = processor.getTotalNumOutputChannels();
            auto totalChans = jmax(numIns, numOuts);

            // Build routing plan for this node's inputs
            // For each input channel, collect sources and determine routing ops needed
            struct InputRouting
            {
                int destChannel;
                std::vector<std::pair<int, int>> internalSources; // (sourceNodeID, bufferChannel)
                bool hasExternalSource = false;                   // From other chains or audio input
            };

            std::vector<InputRouting> routingPlan;

            for (int destCh = 0; destCh < numIns; ++destCh)
            {
                NodeAndChannel destPin{node->nodeID, destCh};
                auto sources = c.getSourcesForDestination(destPin);

                InputRouting routing;
                routing.destChannel = destCh;

                for (const auto& src : sources)
                {
                    // Check if source is from a node in our chain
                    bool isInternalSource = std::find_if(
                                                orderedNodes.begin(),
                                                orderedNodes.end(),
                                                [&](const Node* n) { return n->nodeID == src.nodeID; }
                                            )
                                         != orderedNodes.end();

                    if (isInternalSource)
                    {
                        auto key = std::make_pair(src.nodeID.uid, src.channelIndex);
                        auto it = outputChannelMap.find(key);
                        if (it != outputChannelMap.end())
                            routing.internalSources.push_back({src.nodeID.uid, it->second});
                    }
                    else
                    {
                        // External source (from other chains, audio input, etc.)
                        // These are handled by DelayCompensatingMixer which puts data in the buffer
                        routing.hasExternalSource = true;
                    }
                }

                routingPlan.push_back(routing);
            }

            // Execute routing: handle copy/add operations
            // We need to be careful about order to avoid overwriting sources
            // Strategy: first copy to temp buffer if needed, then do final routing

            // Identify channels that are both source and dest (need temp buffer)
            std::set<int> sourceChannels;
            std::set<int> destChannels;
            for (const auto& routing : routingPlan)
            {
                destChannels.insert(routing.destChannel);
                for (const auto& [nodeId, srcCh] : routing.internalSources)
                    sourceChannels.insert(srcCh);
            }

            // Find conflicts: dest channels that are also sources for OTHER dest channels
            std::map<int, int> channelRemapping; // original channel -> temp channel

            for (const auto& routing : routingPlan)
            {
                for (const auto& [nodeId, srcCh] : routing.internalSources)
                {
                    // If source channel will be overwritten by a different dest channel
                    if (destChannels.count(srcCh) && srcCh != routing.destChannel)
                    {
                        // Check if any earlier routing overwrites this source
                        for (const auto& otherRouting : routingPlan)
                        {
                            if (otherRouting.destChannel == srcCh && otherRouting.destChannel != routing.destChannel)
                            {
                                // Need to save this source to temp buffer first
                                if (channelRemapping.find(srcCh) == channelRemapping.end())
                                {
                                    sequence.addCopyChannelOp(srcCh, tempBufferChannel);
                                    channelRemapping[srcCh] = tempBufferChannel;
                                }
                                break;
                            }
                        }
                    }
                }
            }

            // Now execute the routing
            for (const auto& routing : routingPlan)
            {
                int destCh = routing.destChannel;

                if (routing.internalSources.empty() && !routing.hasExternalSource)
                {
                    // No sources at all (neither internal nor external) - clear the channel
                    sequence.addClearChannelOp(destCh);
                }
                else if (!routing.internalSources.empty())
                {
                    // Has internal sources - need to route them
                    bool first = true;
                    bool needToPreserveExternal = routing.hasExternalSource;

                    for (const auto& [nodeId, srcCh] : routing.internalSources)
                    {
                        // Use remapped channel if source was saved to temp
                        int actualSrcCh = srcCh;
                        auto remapIt = channelRemapping.find(srcCh);
                        if (remapIt != channelRemapping.end())
                            actualSrcCh = remapIt->second;

                        if (first && !needToPreserveExternal)
                        {
                            // First source and no external: copy (or skip if same channel)
                            if (actualSrcCh != destCh)
                                sequence.addCopyChannelOp(actualSrcCh, destCh);
                            first = false;
                        }
                        else
                        {
                            // Additional sources OR has external: add to mix
                            sequence.addAddChannelOp(actualSrcCh, destCh);
                        }
                    }
                }
                // else: only external sources - buffer already has data from mixer, don't touch
            }

            // Direct channel mapping for processOp: channel 0→0, 1→1, 2→2, etc.
            Array<int> audioChannelsToUse;
            for (int ch = 0; ch < totalChans; ++ch)
                audioChannelsToUse.add(ch);

            const auto thisNodeLatency = getInputLatencyForNode(c, node->nodeID) + processor.getLatencySamples();
            delays[node->nodeID.uid] = thisNodeLatency;
            totalLatency = jmax(totalLatency, thisNodeLatency);

            sequence.addProcessOp(node, audioChannelsToUse, totalChans, midiBufferIndex);

            // Update output channel map: this node's outputs are now in their respective channels
            for (int outCh = 0; outCh < numOuts; ++outCh)
                outputChannelMap[{node->nodeID.uid, outCh}] = outCh;
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
        const std::vector<NodeID>& nodeFilter,
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
        const std::vector<NodeID>& nodeFilter,
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

    int getNumChannelsNeeded() const
    {
        return sequence.sequence.numBuffersNeeded;
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
// Buffer pool for reusing chain buffers across graph rebuilds.
class ChainBufferPool
{
public:
    struct PooledBuffer
    {
        AudioBuffer<float> audioBuffer;
        MidiBuffer midiBuffer;

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
        for (auto& buffer : buffers)
        {
            if (buffer.use_count() == 1)
            {
                if (buffer->audioBuffer.getNumSamples() != blockSize)
                    buffer->resize(blockSize);
                return buffer;
            }
        }

        auto newBuffer = std::make_shared<PooledBuffer>(blockSize);
        buffers.push_back(newBuffer);
        return newBuffer;
    }

    size_t getPoolSize() const
    {
        return buffers.size();
    }

    void preallocate(size_t count, int blockSize)
    {
        buffers.reserve(count);
        for (size_t i = 0; i < count; ++i)
            buffers.push_back(std::make_shared<PooledBuffer>(blockSize));
    }

    void cleanupUnused()
    {
        buffers.erase(
            std::remove_if(
                buffers.begin(),
                buffers.end(),
                [](const std::shared_ptr<PooledBuffer>& buf) { return buf.use_count() == 1; }
            ),
            buffers.end()
        );
    }

private:
    std::vector<std::shared_ptr<PooledBuffer>> buffers;
};

//==============================================================================
// Pool for persistent delay lines that survive graph rebuilds.
class DelayLinePool
{
public:
    static constexpr int MAX_DELAY_SAMPLES = 1024 * 1024;

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
    };

    std::shared_ptr<PooledDelayLine>
    acquireDelayLine(const DelayLineKey& key, int delayNeeded, double sampleRate, uint32 blockSize, int numChannels)
    {
        jassert(delayNeeded <= MAX_DELAY_SAMPLES);

        auto it = delayLines.find(key);
        if (it != delayLines.end())
        {
            it->second->delayAmount.store(delayNeeded);
            return it->second;
        }

        auto pooledLine = std::make_shared<PooledDelayLine>();
        pooledLine->delayLine.prepare(juce::dsp::ProcessSpec{sampleRate, blockSize, static_cast<uint32>(numChannels)});
        pooledLine->delayLine.reset();
        pooledLine->delayLine.setMaximumDelayInSamples(MAX_DELAY_SAMPLES);
        pooledLine->delayAmount.store(delayNeeded);
        delayLines[key] = pooledLine;
        return pooledLine;
    }

    void cleanupUnused()
    {
        for (auto it = delayLines.begin(); it != delayLines.end();)
            if (it->second.use_count() == 1)
                it = delayLines.erase(it);
            else
                ++it;
    }

    size_t getPoolSize() const
    {
        return delayLines.size();
    }

private:
    std::unordered_map<DelayLineKey, std::shared_ptr<PooledDelayLine>, DelayLineKeyHash> delayLines;
};

//==============================================================================
// Parallel render sequence that partitions the graph into independent chains.
class ParallelRenderSequence
{
public:
    using NodeID = AudioProcessorGraphMT::NodeID;
    using Node = AudioProcessorGraphMT::Node;
    using Connection = AudioProcessorGraphMT::Connection;

    struct ChainRenderSequence;

    // Applies delay compensation when mixing sources into a destination.
    class DelayCompensatingMixer
    {
    public:
        explicit DelayCompensatingMixer(uint32 destId_, DelayLinePool* pool_)
            : destId(destId_)
            , pool(pool_)
        {
        }

        DelayCompensatingMixer(const DelayCompensatingMixer&) = delete;
        DelayCompensatingMixer& operator=(const DelayCompensatingMixer&) = delete;
        DelayCompensatingMixer(DelayCompensatingMixer&&) = delete;
        DelayCompensatingMixer& operator=(DelayCompensatingMixer&&) = delete;

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
            delayLines[sourceId] =
                pool->acquireDelayLine({sourceId, destId}, delayNeeded, sampleRate, blockSize, numChannels);
        }

        void mixWithDelay(uint32 sourceId, const float* src, float* dst, int numSamples, int channel = 0)
        {
            auto it = delayLines.find(sourceId);
            if (it == delayLines.end())
            {
                FloatVectorOperations::add(dst, src, numSamples);
                return;
            }

            auto& pooledLine = it->second;
            auto& delayLine = pooledLine->delayLine;
            int delay = pooledLine->delayAmount.load();

            for (int i = 0; i < numSamples; ++i)
            {
                delayLine.pushSample(channel, src[i]);
                dst[i] += delayLine.popSample(channel, static_cast<float>(delay));
            }
        }

    private:
        uint32 destId;
        DelayLinePool* pool;
        std::unordered_map<uint32, std::shared_ptr<DelayLinePool::PooledDelayLine>> delayLines;
    };

    //==========================================================================
    // Routes chain outputs to host output and OBS nodes with delay compensation.
    class OutputRouter
    {
    public:
        enum class SourceType : uint8_t
        {
            Chain = 0,
            Passthrough = 1
        };

        struct Route
        {
            SourceType type;
            size_t sourceIndex;
            int sourceChannel;
            int destChannel;
            uint32 obsNodeId = 0;
        };

        explicit OutputRouter(DelayLinePool* pool)
            : mixer(UINT32_MAX, pool)
        {
        }

        void clear()
        {
            hostOutputRoutes.clear();
            obsNodes.clear();
        }

        void addChainToHostRoute(
            size_t chainIndex,
            int sourceChannel,
            int destChannel,
            int chainLatency,
            int totalLatency,
            double sampleRate,
            uint32 blockSize
        )
        {
            uint32 sourceId = makeSourceId(SourceType::Chain, chainIndex, sourceChannel);

            if (registeredSources.insert(sourceId).second)
                mixer.registerSource(sourceId, chainLatency, totalLatency, sampleRate, blockSize);

            hostOutputRoutes.push_back({SourceType::Chain, chainIndex, sourceChannel, destChannel, 0});
        }

        void addPassthroughRoute(
            size_t passthroughIndex,
            int inputChannel,
            int outputChannel,
            int totalLatency,
            double sampleRate,
            uint32 blockSize
        )
        {
            uint32 sourceId = makeSourceId(SourceType::Passthrough, passthroughIndex, inputChannel);

            if (registeredSources.insert(sourceId).second)
                mixer.registerSource(sourceId, 0, totalLatency, sampleRate, blockSize);

            hostOutputRoutes.push_back({SourceType::Passthrough, passthroughIndex, inputChannel, outputChannel, 0});
        }

        size_t addObsNode(Node::Ptr node, std::shared_ptr<ChainBufferPool::PooledBuffer> buffer)
        {
            size_t index = obsNodes.size();
            obsNodes.push_back({node, buffer, {}, {}});
            return index;
        }

        size_t getObsNodeCount() const
        {
            return obsNodes.size();
        }

        void addChainToObsRoute(size_t obsNodeIndex, NodeID sourceNodeID, int sourceChannel, int destChannel)
        {
            if (obsNodeIndex < obsNodes.size())
                obsNodes[obsNodeIndex].chainInputConnections.push_back({sourceNodeID, sourceChannel, destChannel});
        }

        void addInputToObsRoute(size_t obsNodeIndex, int hostInputChannel, int nodeInputChannel)
        {
            if (obsNodeIndex < obsNodes.size())
                obsNodes[obsNodeIndex].directInputConnections.push_back({hostInputChannel, nodeInputChannel});
        }

        void routeAllOutputs(
            AudioBuffer<float>& hostOutput,
            MidiBuffer& hostMidi,
            const AudioBuffer<float>& savedInput,
            const std::vector<std::unique_ptr<ChainRenderSequence>>& chains,
            const std::unordered_map<uint32, ChainRenderSequence*>& nodeToChainMap,
            int numSamples
        )
        {
            hostOutput.clear();
            hostMidi.clear();

            for (const auto& route : hostOutputRoutes)
            {
                const float* src = nullptr;

                if (route.type == SourceType::Chain)
                {
                    if (route.sourceIndex < chains.size())
                    {
                        const auto& chain = chains[route.sourceIndex];
                        if (route.sourceChannel < chain->getAudioBuffer().getNumChannels())
                            src = chain->getAudioBuffer().getReadPointer(route.sourceChannel);
                    }
                }
                else if (route.type == SourceType::Passthrough)
                {
                    if (route.sourceChannel < savedInput.getNumChannels())
                        src = savedInput.getReadPointer(route.sourceChannel);
                }

                if (src && route.destChannel < hostOutput.getNumChannels())
                {
                    uint32 sourceId = makeSourceId(route.type, route.sourceIndex, route.sourceChannel);
                    float* dst = hostOutput.getWritePointer(route.destChannel);
                    mixer.mixWithDelay(sourceId, src, dst, numSamples);
                }
            }

            for (const auto& chain : chains)
                if (chain->connectsToMidiOutput)
                    hostMidi.addEvents(chain->getMidiBuffer(), 0, numSamples, 0);

            for (auto& obsNode : obsNodes)
            {
                auto& nodeBuffer = obsNode.buffer->audioBuffer;
                nodeBuffer.setSize(nodeBuffer.getNumChannels(), numSamples, false, false, true);
                nodeBuffer.clear();

                for (const auto& [hostInputChannel, nodeInputChannel] : obsNode.directInputConnections)
                {
                    if (hostInputChannel < savedInput.getNumChannels()
                        && nodeInputChannel < nodeBuffer.getNumChannels())
                    {
                        FloatVectorOperations::add(
                            nodeBuffer.getWritePointer(nodeInputChannel),
                            savedInput.getReadPointer(hostInputChannel),
                            numSamples
                        );
                    }
                }

                for (const auto& [sourceNodeID, sourceChannel, destChannel] : obsNode.chainInputConnections)
                {
                    auto it = nodeToChainMap.find(sourceNodeID.uid);
                    if (it != nodeToChainMap.end() && destChannel < nodeBuffer.getNumChannels())
                    {
                        auto* sourceChain = it->second;
                        if (sourceChannel < sourceChain->getAudioBuffer().getNumChannels())
                        {
                            FloatVectorOperations::add(
                                nodeBuffer.getWritePointer(destChannel),
                                sourceChain->getAudioBuffer().getReadPointer(sourceChannel),
                                numSamples
                            );
                        }
                    }
                }

                // Process the OBS Output node
                if (auto* proc = obsNode.node->getProcessor())
                {
                    MidiBuffer emptyMidi;
                    proc->processBlock(nodeBuffer, emptyMidi);
                }
            }
        }

        void routePassthroughOnly(AudioBuffer<float>& hostOutput, const AudioBuffer<float>& savedInput, int numSamples)
        {
            hostOutput.clear();

            for (const auto& route : hostOutputRoutes)
            {
                if (route.type == SourceType::Passthrough)
                {
                    if (route.sourceChannel < savedInput.getNumChannels()
                        && route.destChannel < hostOutput.getNumChannels())
                    {
                        FloatVectorOperations::add(
                            hostOutput.getWritePointer(route.destChannel),
                            savedInput.getReadPointer(route.sourceChannel),
                            numSamples
                        );
                    }
                }
            }

            for (auto& obsNode : obsNodes)
            {
                auto& nodeBuffer = obsNode.buffer->audioBuffer;
                nodeBuffer.setSize(nodeBuffer.getNumChannels(), numSamples, false, false, true);
                nodeBuffer.clear();

                for (const auto& [hostInputChannel, nodeInputChannel] : obsNode.directInputConnections)
                {
                    if (hostInputChannel < savedInput.getNumChannels()
                        && nodeInputChannel < nodeBuffer.getNumChannels())
                    {
                        FloatVectorOperations::add(
                            nodeBuffer.getWritePointer(nodeInputChannel),
                            savedInput.getReadPointer(hostInputChannel),
                            numSamples
                        );
                    }
                }

                if (auto* proc = obsNode.node->getProcessor())
                {
                    MidiBuffer emptyMidi;
                    proc->processBlock(nodeBuffer, emptyMidi);
                }
            }
        }

    private:
        static uint32 makeSourceId(SourceType type, size_t index, int channel)
        {
            return (uint32(type) << 30) | ((uint32(index) & 0x3FFFFF) << 8) | (uint32(channel) & 0xFF);
        }

        DelayCompensatingMixer mixer;
        std::vector<Route> hostOutputRoutes;
        std::set<uint32> registeredSources;

        struct ObsNodeData
        {
            Node::Ptr node;
            std::shared_ptr<ChainBufferPool::PooledBuffer> buffer;
            std::vector<std::tuple<NodeID, int, int>> chainInputConnections;
            std::vector<std::pair<int, int>> directInputConnections;
        };

        std::vector<ObsNodeData> obsNodes;
    };

    // Each chain represents an independent subgraph that can execute in parallel
    struct ChainRenderSequence
    {
        std::unique_ptr<RenderSequence> sequence;
        int chainLatency = 0;       // Internal latency (sum of processors in this chain)
        int accumulatedLatency = 0; // max(source chain latencies) + chainLatency
        int latencySum = 0;         // For runtime change detection
        int topologicalLevel = 0;
        size_t subgraphIndex = 0;
        bool connectsToOutput = false;
        bool connectsToMidiOutput = false;

        std::shared_ptr<ChainBufferPool::PooledBuffer> pooledBuffer;

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

        std::atomic<int> pendingDependencies{0};
        int initialDependencyCount = 0;
        std::vector<ChainRenderSequence*> dependentChains;
        std::vector<ChainRenderSequence*> sourceChains;

        AudioPlayHead* cachedPlayHead = nullptr;
        class ParallelRenderSequence* parentSequence = nullptr;
        DelayCompensatingMixer inputMixer;

        ChainRenderSequence(uint32 chainId, DelayLinePool* pool)
            : inputMixer(chainId, pool)
        {
        }

        ~ChainRenderSequence() = default;

        ChainRenderSequence(const ChainRenderSequence&) = delete;
        ChainRenderSequence& operator=(const ChainRenderSequence&) = delete;
        ChainRenderSequence(ChainRenderSequence&&) = delete;
        ChainRenderSequence& operator=(ChainRenderSequence&&) = delete;
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
        , outputRouter(&delayPool)
    {
        // Helper to check if a vector contains a value
        auto contains = [](const auto& vec, const auto& val)
        { return std::find(vec.begin(), vec.end(), val) != vec.end(); };

        // Extract parallel subgraphs
        SubgraphExtractor extractor;
        subgraphs = extractor.extractUniversalParallelization(graph);
        connectionsVec = c.getConnections();

        // Get worker count for load-balanced level assignment
        // Use numWorkers + 1 because the main thread also processes jobs
        auto* audioPool = atk::RealtimeThreadPool::getInstance();
        const size_t totalWorkers = audioPool ? static_cast<size_t>(audioPool->getNumWorkers() + 1) : SIZE_MAX;
        extractor.buildSubgraphDependencies(subgraphs, connectionsVec, totalWorkers);

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
        // Supports 1-to-many and many-to-1 routing
        for (const auto& conn : connectionsVec)
            if (conn.source.nodeID == audioInputNodeID && conn.destination.nodeID == audioOutputNodeID)
                passthroughConnections.push_back({conn.source.channelIndex, conn.destination.channelIndex});

        // Collect OBS Output nodes for sequential processing (to avoid deadlock with nested PluginHost2 MT)
        // Must happen before early return for empty subgraphs
        for (const auto& node : n.getNodes())
        {
            if (!node || !node->getProcessor() || node->getProcessor()->getName() != "OBS Output")
                continue;

            auto buffer = bufferPool.acquireBuffer(s.blockSize);
            size_t obsNodeIndex = outputRouter.addObsNode(node, buffer);

            // Scan connections to find inputs to this OBS Output node
            for (const auto& conn : connectionsVec)
            {
                if (conn.destination.nodeID != node->nodeID)
                    continue;

                if (conn.source.nodeID == audioInputNodeID)
                {
                    // Direct: Audio Input -> OBS Output
                    outputRouter
                        .addInputToObsRoute(obsNodeIndex, conn.source.channelIndex, conn.destination.channelIndex);
                }
                else
                {
                    // From processor node -> OBS Output
                    outputRouter.addChainToObsRoute(
                        obsNodeIndex,
                        conn.source.nodeID,
                        conn.source.channelIndex,
                        conn.destination.channelIndex
                    );
                }
            }
        }

        // Pre-allocate buffer pool to avoid allocation in audio thread
        // Need: 1 for savedInput + subgraphs.size() for chains + OBS nodes already acquired above
        // Add extra margin for safety
        const size_t numBuffersNeeded = 1 + subgraphs.size() + outputRouter.getObsNodeCount() + 2;
        bufferPool.preallocate(numBuffersNeeded, s.blockSize);

        // Pre-allocate savedInput buffer for process() - must be done after preallocate
        savedInputBuffer = bufferPool.acquireBuffer(s.blockSize);

        if (subgraphs.empty())
        {
            // Register passthrough routes even when there are no chains
            // totalLatency = 0 when there are no chains
            for (size_t i = 0; i < passthroughConnections.size(); ++i)
            {
                const auto& [inputCh, outputCh] = passthroughConnections[i];
                outputRouter.addPassthroughRoute(i, inputCh, outputCh, 0, s.sampleRate, s.blockSize);
            }
            return;
        }

        // Build filtered RenderSequences for all subgraphs
        chains.reserve(subgraphs.size());
        maxTopologicalLevel = 0;

        for (size_t i = 0; i < subgraphs.size(); ++i)
        {
            const auto& subgraph = subgraphs[i];
            auto chain = std::make_unique<ChainRenderSequence>(static_cast<uint32>(i), &delayLinePool);

            chain->pooledBuffer = bufferPool.acquireBuffer(s.blockSize);

            // Build RenderSequence - pass empty delays since we calculate accumulated latency at chain level
            static const std::unordered_map<uint32, int> emptyDelays;
            chain->sequence =
                std::make_unique<RenderSequence>(s, n, c, subgraph.nodeIDs, emptyDelays, chain->getAudioBuffer());
            chain->chainLatency = chain->sequence->getLatencySamples();
            chain->topologicalLevel = subgraph.topologicalLevel;
            chain->subgraphIndex = i;

            for (const auto& nodeID : subgraph.nodeIDs)
                if (auto node = n.getNodeForId(nodeID))
                    if (auto* proc = node->getProcessor())
                        chain->latencySum += proc->getLatencySamples();

            for (const auto& conn : connectionsVec)
            {
                if (contains(subgraph.nodeIDs, conn.source.nodeID))
                {
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

            chain->initialDependencyCount = static_cast<int>(subgraph.dependsOn.size());
            chain->pendingDependencies.store(chain->initialDependencyCount, std::memory_order_relaxed);

            maxTopologicalLevel = std::max(maxTopologicalLevel, chain->topologicalLevel);

            chains.push_back(std::move(chain));
        }

        for (size_t i = 0; i < subgraphs.size(); ++i)
        {
            chains[i]->parentSequence = this;
            for (size_t dependentIdx : subgraphs[i].dependents)
                if (dependentIdx < chains.size())
                    chains[i]->dependentChains.push_back(chains[dependentIdx].get());

            for (size_t sourceIdx : subgraphs[i].dependsOn)
                if (sourceIdx < chains.size())
                    chains[i]->sourceChains.push_back(chains[sourceIdx].get());
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
                    if (contains(subgraphs[i].nodeIDs, conn.source.nodeID))
                    {
                        sourceChainIdx = i;
                        break;
                    }
                }

                // Find which chain contains dest node
                size_t destChainIdx = SIZE_MAX;
                for (size_t i = 0; i < subgraphs.size(); ++i)
                {
                    if (contains(subgraphs[i].nodeIDs, conn.destination.nodeID))
                    {
                        destChainIdx = i;
                        break;
                    }
                }

                // If both found and different chains, record the MIDI connection
                if (sourceChainIdx != SIZE_MAX && destChainIdx != SIZE_MAX)
                    midiChainConnections.insert({sourceChainIdx, destChainIdx});
            }
        }

        // Build NodeID -> Chain map for efficient OBS Output routing
        nodeToChainMap.clear();
        for (auto& chain : chains)
        {
            // Use the chain's stored subgraphIndex to ensure correct mapping
            if (chain->subgraphIndex < subgraphs.size())
                for (const auto& nodeID : subgraphs[chain->subgraphIndex].nodeIDs)
                    nodeToChainMap[nodeID.uid] = chain.get();
        }

        // Calculate accumulated latency for each chain in topological order
        // accumulatedLatency = max(source chain accumulated latencies) + own chainLatency
        // Calculate accumulated latency per chain in topological order
        for (int level = 0; level <= maxTopologicalLevel; ++level)
        {
            for (auto& chain : chains)
            {
                if (chain->topologicalLevel != level)
                    continue;

                int maxSourceLatency = 0;
                for (const auto* sourceChain : chain->sourceChains)
                    maxSourceLatency = std::max(maxSourceLatency, sourceChain->accumulatedLatency);

                chain->accumulatedLatency = maxSourceLatency + chain->chainLatency;
            }
        }

        // Register input mixers for delay compensation
        for (auto& chain : chains)
        {
            int maxInputLatency = 0;
            for (const auto* sourceChain : chain->sourceChains)
                maxInputLatency = std::max(maxInputLatency, sourceChain->accumulatedLatency);

            bool hasAudioInputConnection = false;
            for (const auto& conn : connectionsVec)
            {
                if (conn.source.nodeID == audioInputNodeID
                    && contains(subgraphs[chain->subgraphIndex].nodeIDs, conn.destination.nodeID))
                {
                    hasAudioInputConnection = true;
                    break;
                }
            }

            if (hasAudioInputConnection)
            {
                chain->inputMixer.registerSource(
                    AUDIO_INPUT_SOURCE_ID,
                    0,
                    maxInputLatency,
                    s.sampleRate,
                    s.blockSize,
                    chain->sequence->getNumChannelsNeeded()
                );
            }

            for (const auto* sourceChain : chain->sourceChains)
            {
                chain->inputMixer.registerSource(
                    static_cast<int>(sourceChain->subgraphIndex),
                    sourceChain->accumulatedLatency,
                    maxInputLatency,
                    s.sampleRate,
                    s.blockSize,
                    chain->sequence->getNumChannelsNeeded()
                );
            }
        }

        totalLatency = 0;
        for (const auto& chain : chains)
            if (chain->connectsToOutput)
                totalLatency = std::max(totalLatency, chain->accumulatedLatency);

        chainsByLevel.resize(maxTopologicalLevel + 1);
        for (auto& chain : chains)
            chainsByLevel[chain->topologicalLevel].push_back(chain.get());

        for (const auto& conn : connectionsVec)
        {
            if (conn.source.nodeID == audioInputNodeID)
            {
                for (size_t i = 0; i < subgraphs.size(); ++i)
                {
                    if (contains(subgraphs[i].nodeIDs, conn.destination.nodeID))
                    {
                        inputChannelMap[{i, conn.destination.channelIndex}] = conn.source.channelIndex;
                        break;
                    }
                }
            }
        }

        // Build MIDI input mappings: MIDI Input node → chains
        for (const auto& conn : connectionsVec)
        {
            if (conn.source.nodeID == midiInputNodeID)
            {
                for (size_t i = 0; i < subgraphs.size(); ++i)
                {
                    if (contains(subgraphs[i].nodeIDs, conn.destination.nodeID))
                    {
                        midiInputChains.insert(i);
                        break;
                    }
                }
            }
        }

        // Build output channel mappings: chains → Audio Output node
        // Supports 1-to-many routing (one source channel to multiple output channels)
        for (const auto& conn : connectionsVec)
        {
            if (conn.destination.nodeID == audioOutputNodeID)
            {
                for (size_t i = 0; i < subgraphs.size(); ++i)
                {
                    if (contains(subgraphs[i].nodeIDs, conn.source.nodeID))
                    {
                        int chainLatency = (i < chains.size()) ? chains[i]->accumulatedLatency : 0;
                        outputRouter.addChainToHostRoute(
                            i,
                            conn.source.channelIndex,
                            conn.destination.channelIndex,
                            chainLatency,
                            totalLatency,
                            s.sampleRate,
                            s.blockSize
                        );
                        break;
                    }
                }
            }
        }

        // Register passthrough routes (Audio Input → Audio Output with no processors)
        for (size_t i = 0; i < passthroughConnections.size(); ++i)
        {
            const auto& [inputCh, outputCh] = passthroughConnections[i];
            outputRouter.addPassthroughRoute(i, inputCh, outputCh, totalLatency, s.sampleRate, s.blockSize);
        }

        auto* threadPool = atk::RealtimeThreadPool::getInstance();
        const int numWorkers = threadPool ? threadPool->getNumWorkers() : 0;

        useDependencyMode = false;
        if (threadPool && threadPool->isReady())
        {
            taskGraph.clear();
            taskGraph.reserve(chains.size());
            chainToTaskIndex.clear();
            chainToTaskIndex.reserve(chains.size());

            for (size_t i = 0; i < chains.size(); ++i)
            {
                auto* chain = chains[i].get();
                size_t taskIdx = taskGraph.addTask(chain, &executeChainTask, 0);
                chainToTaskIndex.push_back(taskIdx);
            }

            for (size_t i = 0; i < subgraphs.size(); ++i)
            {
                for (size_t dependsOnIdx : subgraphs[i].dependsOn)
                    if (dependsOnIdx < chainToTaskIndex.size() && i < chainToTaskIndex.size())
                        taskGraph.addDependency(chainToTaskIndex[i], chainToTaskIndex[dependsOnIdx]);
            }

            // Dependency mode: tasks route their inputs before processing
            useDependencyMode = true;
        }
    }

    // Route audio/MIDI from a source chain to a destination chain
    // Called by the destination chain's task - safe because only one task writes to destChain's buffer
    void routeFromSourceToChain(ChainRenderSequence* sourceChain, ChainRenderSequence* destChain, int numSamples)
    {
        auto contains = [](const auto& vec, const auto& val)
        { return std::find(vec.begin(), vec.end(), val) != vec.end(); };

        for (const auto& conn : connectionsVec)
        {
            if (conn.source.isMIDI() || conn.destination.isMIDI())
                continue;

            bool sourceInChain = contains(subgraphs[sourceChain->subgraphIndex].nodeIDs, conn.source.nodeID);
            bool destInTarget = contains(subgraphs[destChain->subgraphIndex].nodeIDs, conn.destination.nodeID);

            if (sourceInChain && destInTarget)
            {
                int srcChannel = conn.source.channelIndex;
                int dstChannel = conn.destination.channelIndex;

                if (srcChannel < sourceChain->getAudioBuffer().getNumChannels()
                    && dstChannel < destChain->getAudioBuffer().getNumChannels())
                {
                    const auto* src = sourceChain->getAudioBuffer().getReadPointer(srcChannel);
                    auto* dst = destChain->getAudioBuffer().getWritePointer(dstChannel);
                    destChain->inputMixer
                        .mixWithDelay(static_cast<int>(sourceChain->subgraphIndex), src, dst, numSamples, dstChannel);
                }
            }
        }

        if (midiChainConnections.count({sourceChain->subgraphIndex, destChain->subgraphIndex}) > 0)
            destChain->getMidiBuffer().addEvents(sourceChain->getMidiBuffer(), 0, numSamples, 0);
    }

    // Dependency task: Route inputs from completed source chains, then process
    // This is safe because the dependency graph ensures all source chains have completed
    // before this task executes, so we're the only writer to our input buffer.
    static void executeChainTask(void* userData)
    {
        auto* chain = static_cast<ChainRenderSequence*>(userData);
        if (!chain || !chain->sequence || !chain->parentSequence)
            return;

        auto* parent = chain->parentSequence;
        int numSamples = parent->cachedNumSamples;

        // Route inputs from all source chains that feed into this chain
        // Safe: source chains are guaranteed complete (dependency graph), and only this
        // task writes to this chain's buffer (each chain has exactly one task)
        for (auto* sourceChain : chain->sourceChains)
            parent->routeFromSourceToChain(sourceChain, chain, numSamples);

        AudioBuffer<float> chainBufferView(
            chain->getAudioBuffer().getArrayOfWritePointers(),
            chain->getAudioBuffer().getNumChannels(),
            numSamples
        );

        chain->sequence->process(chainBufferView, chain->getMidiBuffer(), chain->cachedPlayHead);
    }

    void process(AudioBuffer<float>& audio, MidiBuffer& midi, AudioPlayHead* playHead)
    {
        // Helper to check if a vector contains a value
        auto contains = [](const auto& vec, const auto& val)
        { return std::find(vec.begin(), vec.end(), val) != vec.end(); };

        const int numSamples = audio.getNumSamples();

        // Use pre-allocated buffer for saving input
        auto& savedInput = savedInputBuffer->audioBuffer;
        if (savedInput.getNumSamples() < numSamples)
            savedInput.setSize(savedInput.getNumChannels(), numSamples, false, false, true);

        const int numInputChannels = std::min(audio.getNumChannels(), savedInput.getNumChannels());
        for (int ch = 0; ch < numInputChannels; ++ch)
        {
            const auto* src = audio.getReadPointer(ch);
            auto* dst = savedInput.getWritePointer(ch);
            FloatVectorOperations::copy(dst, src, numSamples);
        }

        if (chains.empty())
        {
            midi.clear();
            // No chains = no latency, use simplified passthrough-only routing
            outputRouter.routePassthroughOnly(audio, savedInput, numSamples);
            return;
        }

        for (auto& chain : chains)
            chain->pendingDependencies.store(chain->initialDependencyCount, std::memory_order_relaxed);

        for (auto& chain : chains)
        {
            auto& chainBuffer = chain->getAudioBuffer();

            if (chainBuffer.getNumSamples() < numSamples)
                chainBuffer.setSize(chainBuffer.getNumChannels(), numSamples, false, false, true);

            chainBuffer.clear();
            chain->getMidiBuffer().clear();
        }

        for (auto& chain : chains)
        {
            for (const auto& mapping : inputChannelMap)
            {
                if (mapping.first.first == chain->subgraphIndex)
                {
                    int destChannel = mapping.first.second;
                    int sourceChannel = mapping.second;

                    if (sourceChannel < savedInput.getNumChannels()
                        && destChannel < chain->getAudioBuffer().getNumChannels())
                    {
                        const auto* src = savedInput.getReadPointer(sourceChannel);
                        auto* dst = chain->getAudioBuffer().getWritePointer(destChannel);
                        chain->inputMixer.mixWithDelay(AUDIO_INPUT_SOURCE_ID, src, dst, numSamples, destChannel);
                    }
                }
            }

            bool chainReceivesMidi = (midiInputChains.count(chain->subgraphIndex) > 0);
            if (chainReceivesMidi)
                chain->getMidiBuffer().addEvents(midi, 0, numSamples, 0);
        }

        auto* pool = atk::RealtimeThreadPool::getInstance();
        const bool isWorkerThread = pool && pool->isCalledFromWorkerThread();
        const bool canUseThreadPool = pool && pool->isReady() && !isWorkerThread;

        if (useDependencyMode && canUseThreadPool)
        {
            // Dependency-based parallel execution:
            // Each task routes its own inputs from source chains, then processes.
            // This is safe because the dependency graph guarantees source chains
            // complete before dependent chains start, and each chain's buffer is
            // only written to by its own task.

            for (auto& chain : chains)
                chain->cachedPlayHead = playHead;

            cachedNumSamples = numSamples;

            // Execute all chains respecting dependencies
            // Input routing happens inside each task before processing
            pool->executeDependencyGraph(&taskGraph);
        }
        else
        {
            // Serial fallback: process chains level by level
            for (int level = 0; level <= maxTopologicalLevel; ++level)
            {
                auto& chainsAtLevel = chainsByLevel[level];
                if (chainsAtLevel.empty())
                    continue;

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
                            bool sourceInChain = contains(subgraphs[chain->subgraphIndex].nodeIDs, conn.source.nodeID);
                            bool destInDependent =
                                contains(subgraphs[dependent->subgraphIndex].nodeIDs, conn.destination.nodeID);

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

        // Step 5: Route all outputs through unified OutputRouter
        // Handles chains → Audio Output, passthrough → Audio Output, and OBS Output nodes
        outputRouter.routeAllOutputs(audio, midi, savedInput, chains, nodeToChainMap, numSamples);
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
#ifdef ATK_DEBUG
                DBG("[PARALLEL] Latency changed in subgraph "
                    << i
                    << ": expected "
                    << chain->latencySum
                    << ", current "
                    << currentLatencySum);
#endif
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
    std::vector<SubgraphExtractor::Subgraph> subgraphs;

    // ========================================================================
    // DEPENDENCY-BASED EXECUTION MODE (preferred for realtime)
    // ========================================================================
    // Task graph is owned by this sequence, not by the global thread pool.
    // This allows safe graph rebuilds without affecting in-flight execution.
    DependencyTaskGraph taskGraph;
    bool useDependencyMode = false;
    std::vector<size_t> chainToTaskIndex; // Maps chain index -> task graph task index
    int cachedNumSamples = 0;             // Cached for dependency mode routing

    // I/O node connection mappings (built during construction, read-only during process)
    // Input: map of (chainIndex, destChannel) -> sourceChannel
    std::map<std::pair<size_t, int>, int> inputChannelMap;

    // Direct passthrough connections (Audio Input -> Audio Output with no processors)
    // Supports 1-to-many and many-to-1 routing: vector of (inputChannel, outputChannel) pairs
    std::vector<std::pair<int, int>> passthroughConnections;

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

    // Unified output router: handles all output routing with delay compensation
    // - Chains → Audio Output (1-to-1, 1-to-many)
    // - Passthrough → Audio Output
    // - Chains/Input → OBS Output nodes
    OutputRouter outputRouter;

    // Pre-allocated buffer for saving input audio (avoids allocation in audio thread)
    std::shared_ptr<ChainBufferPool::PooledBuffer> savedInputBuffer;

    // Map from NodeID to chain pointer for efficient OBS Output routing
    std::unordered_map<uint32, ChainRenderSequence*> nodeToChainMap;
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
    RenderSequenceExchange() = default;

    ~RenderSequenceExchange() override
    {
        stopTimer();
    }

    void setCleanupCallback(std::function<void()> callback)
    {
        cleanupCallback = std::move(callback);
        startTimer(500);
    }

    void set(std::unique_ptr<ParallelRenderSequence>&& next)
    {
        // Move old state out while holding lock, destroy outside to avoid blocking audio thread
        std::unique_ptr<ParallelRenderSequence> toDestroy;

        {
            const SpinLock::ScopedLockType lock(mutex);
            toDestroy = std::move(mainThreadState);
            mainThreadState = std::move(next);
            isNew = true;
        }

        // Destroy outside lock - this releases resources back to pools
        toDestroy.reset();
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
        // Move the old sequence out while holding the lock, then destroy outside lock
        std::unique_ptr<ParallelRenderSequence> toDestroy;

        {
            const SpinLock::ScopedLockType lock(mutex);
            if (!isNew)
                toDestroy = std::move(mainThreadState);
        }

        toDestroy.reset();

        if (cleanupCallback)
            cleanupCallback();
    }

    SpinLock mutex;
    std::unique_ptr<ParallelRenderSequence> mainThreadState, audioThreadState;
    std::function<void()> cleanupCallback;
    bool isNew = false;
};

//==============================================================================
class AudioProcessorGraphMT::Pimpl
{
public:
    explicit Pimpl(AudioProcessorGraphMT& o)
        : owner(&o)
    {
        renderSequenceExchange.setCleanupCallback(
            [this]
            {
                bufferPool.cleanupUnused();
                delayLinePool.cleanupUnused();
            }
        );
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
        auto* pool = atk::RealtimeThreadPool::getInstance();
        if (pool && !pool->isReady())
            pool->initialize();

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
        // Clean up unused resources before building new sequence.
        // This happens on message thread during graph rebuild, not during audio processing.
        bufferPool.cleanupUnused();
        delayLinePool.cleanupUnused();

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
    ChainBufferPool bufferPool;  // Persistent buffer pool for reusing chain buffers across rebuilds
    DelayLinePool delayLinePool; // Persistent delay line pool for delay compensation across rebuilds
    // renderSequenceExchange must be declared AFTER pools so it's destroyed FIRST,
    // ensuring pools are still valid when timer callback accesses them during shutdown
    RenderSequenceExchange renderSequenceExchange;
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
#ifdef ATK_DEBUG
    if (!midi.isEmpty())
        DBG("[AudioProcessorGraphMT::processBlock] Received MIDI: " << midi.getNumEvents() << " events");
#endif

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
