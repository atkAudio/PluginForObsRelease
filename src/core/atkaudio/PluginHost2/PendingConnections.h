#pragma once

#include "../AudioProcessorGraphMT/AudioProcessorGraphMT.h"
#include "InternalPlugins/ParentSource.h"

#include <algorithm>
#include <vector>

// Saved connections whose pins don't exist yet, e.g. I/O node channels before the audio device
// has (re)opened with its final channel count. Kept and retried instead of being dropped.
class PendingConnections
{
public:
    using Connection = atk::AudioProcessorGraphMT::Connection;

    void clear()
    {
        connections.clear();
    }

    const std::vector<Connection>& get() const
    {
        return connections;
    }

    bool remove(const Connection& connection)
    {
        auto previous = connections.size();
        connections.erase(std::remove(connections.begin(), connections.end(), connection), connections.end());
        return previous != connections.size();
    }

    void disconnectNode(atk::AudioProcessorGraphMT::NodeID id)
    {
        connections.erase(
            std::remove_if(
                connections.begin(),
                connections.end(),
                [id](const Connection& connection)
                { return connection.source.nodeID == id || connection.destination.nodeID == id; }
            ),
            connections.end()
        );
    }

    bool addOrDefer(atk::AudioProcessorGraphMT& graph, const Connection& connection)
    {
        if (graph.isConnected(connection) || graph.addConnection(connection))
        {
            remove(connection);
            return true;
        }
        if (!bothNodesExist(graph, connection) || !canDefer(graph, connection))
            return false;
        if (std::find(connections.begin(), connections.end(), connection) == connections.end())
            connections.push_back(connection);
        return true;
    }

    // Message thread only.
    void retry(atk::AudioProcessorGraphMT& graph)
    {
        if (connections.empty())
            return;

        // Synchronous rebuild makes I/O nodes pick up the graph's current channel counts.
        graph.rebuild();

        connections.erase(
            std::remove_if(
                connections.begin(),
                connections.end(),
                [&graph](const Connection& c)
                { return !bothNodesExist(graph, c) || graph.isConnected(c) || graph.addConnection(c); }
            ),
            connections.end()
        );
    }

private:
    static bool canDefer(atk::AudioProcessorGraphMT& graph, const Connection& connection)
    {
        auto* source = graph.getNodeForId(connection.source.nodeID)->getProcessor();
        auto* destination = graph.getNodeForId(connection.destination.nodeID)->getProcessor();
        auto sourceChannel = connection.source.channelIndex;
        auto destinationChannel = connection.destination.channelIndex;
        if (sourceChannel < 0 || destinationChannel < 0 || connection.source.nodeID == connection.destination.nodeID)
            return false;
        auto midi = atk::AudioProcessorGraphMT::midiChannelIndex;
        if (sourceChannel == midi || destinationChannel == midi)
            return false;
        auto* sourceIo = dynamic_cast<atk::AudioProcessorGraphMT::AudioGraphIOProcessor*>(source);
        auto* destinationIo = dynamic_cast<atk::AudioProcessorGraphMT::AudioGraphIOProcessor*>(destination);
        auto* sourceParent = dynamic_cast<atk::ParentSourceProcessor*>(source);
        auto* destinationParent = dynamic_cast<atk::ParentSourceProcessor*>(destination);
        bool sourceValid = sourceChannel < source->getTotalNumOutputChannels();
        bool destinationValid = destinationChannel < destination->getTotalNumInputChannels();
        bool sourceDeferred =
            !sourceValid
            && ((sourceIo != nullptr
                 && sourceIo->getType() == atk::AudioProcessorGraphMT::AudioGraphIOProcessor::audioInputNode)
                || (sourceParent != nullptr && sourceParent->getRole() == atk::ParentSourceProcessor::input));
        bool destinationDeferred =
            !destinationValid
            && ((destinationIo != nullptr
                 && destinationIo->getType() == atk::AudioProcessorGraphMT::AudioGraphIOProcessor::audioOutputNode)
                || (destinationParent != nullptr
                    && destinationParent->getRole() == atk::ParentSourceProcessor::output));
        return (sourceValid || sourceDeferred)
            && (destinationValid || destinationDeferred)
            && (sourceDeferred || destinationDeferred);
    }

    static bool bothNodesExist(const atk::AudioProcessorGraphMT& graph, const Connection& c)
    {
        return graph.getNodeForId(c.source.nodeID) != nullptr && graph.getNodeForId(c.destination.nodeID) != nullptr;
    }

    std::vector<Connection> connections;
};
