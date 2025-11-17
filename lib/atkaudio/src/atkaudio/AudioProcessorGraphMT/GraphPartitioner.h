#pragma once

#include "AudioProcessorGraphMT.h"
#include <map>
#include <set>
#include <vector>
#include <algorithm>

namespace atk
{

class GraphPartitioner
{
public:
    using NodeID = AudioProcessorGraphMT::NodeID;
    using Node = AudioProcessorGraphMT::Node;
    using Connection = AudioProcessorGraphMT::Connection;

    struct Subgraph
    {
        std::set<NodeID> nodeIDs;
        std::set<NodeID> inputNodeIDs;
        std::set<NodeID> outputNodeIDs;
        std::vector<Connection> connections;
        std::set<size_t> dependsOn;
        std::set<size_t> dependents;
        int topologicalLevel = 0;
    };

    GraphPartitioner() = default;

    std::vector<Subgraph> extractUniversalParallelization(atk::AudioProcessorGraphMT& graph)
    {
        subgraphs.clear();
        auto& nodes = graph.getNodes();

        // Reuse preallocated connections vector
        connections.clear();
        connections = graph.getConnections();

        if (nodes.isEmpty())
            return subgraphs;

        outputs.clear();
        inputs.clear();

        // Build connectivity graph using ALL connections (audio AND MIDI)
        // Both types create dependencies - a node must wait for all its inputs
        for (const auto& conn : connections)
        {
            outputs[conn.source.nodeID].insert(conn.destination.nodeID);
            inputs[conn.destination.nodeID].insert(conn.source.nodeID);
        }

        inputNodes.clear();
        outputNodes.clear();

        // Track ALL I/O nodes (audio and MIDI) so they're excluded from subgraphs
        // I/O nodes are handled externally in the parallel processing pipeline
        for (const auto& node : nodes)
        {
            if (auto* ioProc = dynamic_cast<atk::AudioProcessorGraphMT::AudioGraphIOProcessor*>(node->getProcessor()))
            {
                if (ioProc->getType() == atk::AudioProcessorGraphMT::AudioGraphIOProcessor::audioInputNode
                    || ioProc->getType() == atk::AudioProcessorGraphMT::AudioGraphIOProcessor::midiInputNode)
                {
                    inputNodes.insert(node->nodeID);
                }
                else if (ioProc->getType() == atk::AudioProcessorGraphMT::AudioGraphIOProcessor::audioOutputNode
                         || ioProc->getType() == atk::AudioProcessorGraphMT::AudioGraphIOProcessor::midiOutputNode)
                {
                    outputNodes.insert(node->nodeID);
                }
            }
        }

        visited.clear();
        currentSubgraph.clear();

        // First, trace backwards from nodes connected to output (standard flow)
        for (const auto& outputNodeID : outputNodes)
        {
            for (const auto& inputToOutput : inputs[outputNodeID])
                if (visited.count(inputToOutput) == 0)
                    tracePath(inputToOutput);
        }

        // Second, trace any remaining unvisited nodes that have inputs
        // This handles plugins that process audio for metering but don't connect to output
        // We trace them in the same backwards manner to maintain proper chain structure
        for (const auto& node : nodes)
        {
            if (inputNodes.count(node->nodeID) == 0
                && outputNodes.count(node->nodeID) == 0
                && visited.count(node->nodeID) == 0
                && !inputs[node->nodeID].empty()) // Only nodes that have inputs
            {
                tracePath(node->nodeID);
            }
        }

        // Finally, create single-node subgraphs for any remaining orphan nodes
        // These are plugins with no connections at all, but still need to be processed
        // (e.g., generators, test tone plugins, or plugins doing internal processing)
        for (const auto& node : nodes)
        {
            if (inputNodes.count(node->nodeID) == 0
                && outputNodes.count(node->nodeID) == 0
                && visited.count(node->nodeID) == 0)
            {
                // Create a discrete single-node subgraph
                currentSubgraph.clear();
                currentSubgraph.insert(node->nodeID);
                visited.insert(node->nodeID);
                finalizeSubgraph();
            }
        }

        return subgraphs;
    }

    void buildSubgraphDependencies(std::vector<Subgraph>& subgraphs, const std::vector<Connection>& connections)
    {
        if (subgraphs.empty())
            return;

        for (auto& sg : subgraphs)
        {
            sg.dependsOn.clear();
            sg.dependents.clear();
            sg.topologicalLevel = 0;
        }

        for (size_t i = 0; i < subgraphs.size(); ++i)
        {
            for (size_t j = 0; j < subgraphs.size(); ++j)
            {
                if (i == j)
                    continue;

                // Consider ALL connections (audio AND MIDI) for subgraph dependencies
                for (const auto& conn : connections)
                {
                    bool sourceInI = subgraphs[i].nodeIDs.count(conn.source.nodeID) > 0;
                    bool destInJ = subgraphs[j].nodeIDs.count(conn.destination.nodeID) > 0;

                    if (sourceInI && destInJ)
                    {
                        subgraphs[j].dependsOn.insert(i);
                        subgraphs[i].dependents.insert(j);
                        break;
                    }
                }
            }
        }

        // Reuse preallocated levelAssigned vector
        levelAssigned.clear();
        levelAssigned.resize(subgraphs.size(), false);
        int currentLevel = 0;

        while (true)
        {
            bool assignedAny = false;

            for (size_t i = 0; i < subgraphs.size(); ++i)
            {
                if (levelAssigned[i])
                    continue;

                bool canAssign = true;
                int maxDepLevel = -1;

                for (auto depIdx : subgraphs[i].dependsOn)
                {
                    if (!levelAssigned[depIdx])
                    {
                        canAssign = false;
                        break;
                    }
                    maxDepLevel = std::max(maxDepLevel, subgraphs[depIdx].topologicalLevel);
                }

                if (canAssign)
                {
                    subgraphs[i].topologicalLevel = maxDepLevel + 1;
                    levelAssigned[i] = true;
                    assignedAny = true;
                }
            }

            if (!assignedAny)
            {
                // Handle feedback loops: assign remaining subgraphs to current level
                // This breaks cycles by treating them as happening "at the same time"
                for (size_t i = 0; i < subgraphs.size(); ++i)
                {
                    if (!levelAssigned[i])
                    {
                        subgraphs[i].topologicalLevel = currentLevel;
                        levelAssigned[i] = true;

                        // Remove cyclic dependencies to prevent deadlock in parallel execution
                        toRemove.clear();
                        for (auto depIdx : subgraphs[i].dependsOn)
                            if (!levelAssigned[depIdx] || subgraphs[depIdx].topologicalLevel >= currentLevel)
                                toRemove.insert(depIdx);
                        for (auto idx : toRemove)
                        {
                            subgraphs[i].dependsOn.erase(idx);
                            subgraphs[idx].dependents.erase(i);
                        }
                    }
                }
                break;
            }

            ++currentLevel;

            if (currentLevel > static_cast<int>(subgraphs.size()))
                break;
        }
    }

private:
    // Preallocated containers reused across analysis calls
    std::map<NodeID, std::set<NodeID>> outputs;
    std::map<NodeID, std::set<NodeID>> inputs;
    std::set<NodeID> inputNodes;
    std::set<NodeID> outputNodes;
    std::set<NodeID> visited;
    std::set<NodeID> currentSubgraph;
    std::vector<Connection> connections;
    std::vector<Subgraph> subgraphs;
    std::vector<bool> levelAssigned;
    std::set<size_t> toRemove; // Reused in cycle breaking

    void tracePath(NodeID nodeID)
    {
        // Already visited - stop before this node
        if (visited.count(nodeID) > 0)
        {
            finalizeSubgraph();
            return;
        }

        // Hit an I/O node - stop here and finalize
        if (inputNodes.count(nodeID) > 0 || outputNodes.count(nodeID) > 0)
        {
            finalizeSubgraph();
            return;
        }

        bool hasMultipleOutputs = outputs[nodeID].size() > 1;
        bool hasMultipleInputs = inputs[nodeID].size() > 1;

        // Multiple inputs = JOIN point (startpoint for new subgraph in forward flow)
        if (hasMultipleInputs)
        {
            // Finalize any current subgraph before the join
            finalizeSubgraph();

            // Create a new subgraph for the join node itself
            currentSubgraph.clear();
            currentSubgraph.insert(nodeID);
            visited.insert(nodeID);
            finalizeSubgraph();

            // Continue tracing each input path separately
            for (const auto& inputID : inputs[nodeID])
            {
                currentSubgraph.clear();
                tracePath(inputID);
            }
            return;
        }

        // Multiple outputs = FORK point (startpoint when backtracing)
        if (hasMultipleOutputs)
        {
            // Finalize any current subgraph before the fork
            finalizeSubgraph();

            // Create a new subgraph for the fork node itself
            currentSubgraph.clear();
            currentSubgraph.insert(nodeID);
            visited.insert(nodeID);
            finalizeSubgraph();

            // Continue tracing upstream (single input)
            if (inputs[nodeID].size() == 1)
            {
                currentSubgraph.clear();
                tracePath(*inputs[nodeID].begin());
            }
            return;
        }

        // Simple node - add to current subgraph and continue upstream
        currentSubgraph.insert(nodeID);
        visited.insert(nodeID);

        if (inputs[nodeID].size() == 1)
            tracePath(*inputs[nodeID].begin());
        else if (inputs[nodeID].empty())
            finalizeSubgraph();
    }

    void finalizeSubgraph()
    {
        if (currentSubgraph.empty())
            return;

        Subgraph sg;
        sg.nodeIDs = currentSubgraph;

        for (const auto& conn : connections)
        {
            if (sg.nodeIDs.count(conn.source.nodeID) > 0 && outputNodes.count(conn.destination.nodeID) > 0)
                sg.outputNodeIDs.insert(conn.destination.nodeID);

            if (sg.nodeIDs.count(conn.destination.nodeID) > 0 && inputNodes.count(conn.source.nodeID) > 0)
                sg.inputNodeIDs.insert(conn.source.nodeID);

            if (sg.nodeIDs.count(conn.source.nodeID) > 0 && sg.nodeIDs.count(conn.destination.nodeID) > 0)
                sg.connections.push_back(conn);
        }

        subgraphs.push_back(std::move(sg));
        currentSubgraph.clear();
    }
};

} // namespace atk