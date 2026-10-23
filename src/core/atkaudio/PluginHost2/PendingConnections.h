#pragma once

#include "../AudioProcessorGraphMT/AudioProcessorGraphMT.h"

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

    void addOrDefer(atk::AudioProcessorGraphMT& graph, const Connection& connection)
    {
        if (!graph.addConnection(connection) && bothNodesExist(graph, connection) && !graph.isConnected(connection))
            connections.push_back(connection);
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
    static bool bothNodesExist(const atk::AudioProcessorGraphMT& graph, const Connection& c)
    {
        return graph.getNodeForId(c.source.nodeID) != nullptr && graph.getNodeForId(c.destination.nodeID) != nullptr;
    }

    std::vector<Connection> connections;
};
