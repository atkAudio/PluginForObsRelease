#pragma once

#include "AudioProcessorGraphMT.h"
#include "DagPartitioner.h"
#include <map>
#include <vector>

namespace atk
{

/**
 * Extracts subgraphs from AudioProcessorGraphMT for parallel processing.
 */
class SubgraphExtractor
{
public:
    using NodeID = AudioProcessorGraphMT::NodeID;
    using Node = AudioProcessorGraphMT::Node;
    using Connection = AudioProcessorGraphMT::Connection;

    struct Subgraph
    {
        std::vector<NodeID> nodeIDs;
        std::vector<NodeID> inputNodeIDs;
        std::vector<NodeID> outputNodeIDs;
        std::vector<Connection> connections;
        std::vector<size_t> dependsOn;
        std::vector<size_t> dependents;
        int topologicalLevel = 0;
    };

    SubgraphExtractor() = default;

    void setParallelThreshold(size_t threshold)
    {
        partitioner.setParallelThreshold(threshold);
    }

    std::vector<Subgraph> extractUniversalParallelization(atk::AudioProcessorGraphMT& graph)
    {
        subgraphs.clear();
        auto& nodes = graph.getNodes();

        // Get all connections (audio AND MIDI)
        connections.clear();
        connections = graph.getConnections();

        if (nodes.isEmpty())
            return subgraphs;

        // Identify input and output boundary nodes
        inputNodes.clear();
        outputNodes.clear();
        dagNodes.clear();

        for (const auto& node : nodes)
        {
            // Skip OBS Output nodes - they will be processed sequentially on main thread
            // to avoid deadlock with nested PluginHost2 MT
            if (node->getProcessor() && node->getProcessor()->getName() == "OBS Output")
                continue;

            // Create DAG node
            dagNodes.emplace(node->nodeID, DagPartitioner<NodeID>::Node(node->nodeID));

            // Track boundary I/O nodes
            if (auto* ioProc = dynamic_cast<atk::AudioProcessorGraphMT::AudioGraphIOProcessor*>(node->getProcessor()))
            {
                if (ioProc->getType() == atk::AudioProcessorGraphMT::AudioGraphIOProcessor::audioInputNode
                    || ioProc->getType() == atk::AudioProcessorGraphMT::AudioGraphIOProcessor::midiInputNode)
                {
                    inputNodes.push_back(node->nodeID);
                }
                else if (ioProc->getType() == atk::AudioProcessorGraphMT::AudioGraphIOProcessor::audioOutputNode
                         || ioProc->getType() == atk::AudioProcessorGraphMT::AudioGraphIOProcessor::midiOutputNode)
                {
                    outputNodes.push_back(node->nodeID);
                }
            }
        }

        // Build DAG node connectivity from JUCE connections
        // Only add unique node connections (not per-channel duplicates)
        for (const auto& conn : connections)
        {
            auto srcIt = dagNodes.find(conn.source.nodeID);
            auto dstIt = dagNodes.find(conn.destination.nodeID);

            if (srcIt != dagNodes.end())
            {
                auto& outputs = srcIt->second.outputsTo;
                if (std::find(outputs.begin(), outputs.end(), conn.destination.nodeID) == outputs.end())
                    outputs.push_back(conn.destination.nodeID);
            }

            if (dstIt != dagNodes.end())
            {
                auto& inputs = dstIt->second.inputsFrom;
                if (std::find(inputs.begin(), inputs.end(), conn.source.nodeID) == inputs.end())
                    inputs.push_back(conn.source.nodeID);
            }
        }

        // Use DagPartitioner to extract subgraphs (exclude I/O nodes)
        std::vector<NodeID> excludeNodes;
        excludeNodes.insert(excludeNodes.end(), inputNodes.begin(), inputNodes.end());
        excludeNodes.insert(excludeNodes.end(), outputNodes.begin(), outputNodes.end());

        auto dagSubgraphs = partitioner.extractSubgraphs(dagNodes, excludeNodes);

        // Convert DAG subgraphs back to audio-specific subgraphs
        for (const auto& dagSg : dagSubgraphs)
        {
            Subgraph sg;
            sg.nodeIDs = dagSg.nodeIDs;
            sg.dependsOn = dagSg.dependsOn;
            sg.dependents = dagSg.dependents;
            sg.topologicalLevel = dagSg.topologicalLevel;

            // Mark which I/O nodes connect to this subgraph
            for (const auto& nodeId : sg.nodeIDs)
            {
                auto dagNodeIt = dagNodes.find(nodeId);
                if (dagNodeIt == dagNodes.end())
                    continue;

                // Check outputs to I/O nodes
                for (const auto& outputId : dagNodeIt->second.outputsTo)
                    if (std::find(outputNodes.begin(), outputNodes.end(), outputId) != outputNodes.end())
                        if (std::find(sg.outputNodeIDs.begin(), sg.outputNodeIDs.end(), outputId)
                            == sg.outputNodeIDs.end())
                            sg.outputNodeIDs.push_back(outputId);

                // Check inputs from I/O nodes
                for (const auto& inputId : dagNodeIt->second.inputsFrom)
                    if (std::find(inputNodes.begin(), inputNodes.end(), inputId) != inputNodes.end())
                        if (std::find(sg.inputNodeIDs.begin(), sg.inputNodeIDs.end(), inputId) == sg.inputNodeIDs.end())
                            sg.inputNodeIDs.push_back(inputId);
            }

            // Add JUCE-specific connection information
            for (const auto& conn : connections)
                if (std::find(sg.nodeIDs.begin(), sg.nodeIDs.end(), conn.source.nodeID) != sg.nodeIDs.end()
                    && std::find(sg.nodeIDs.begin(), sg.nodeIDs.end(), conn.destination.nodeID) != sg.nodeIDs.end())
                    sg.connections.push_back(conn);

            subgraphs.push_back(std::move(sg));
        }

        return subgraphs;
    }

    void buildSubgraphDependencies(
        std::vector<Subgraph>& subgraphs,
        const std::vector<Connection>& connections,
        size_t numWorkers = SIZE_MAX
    )
    {
        if (subgraphs.empty())
            return;

        // Build DAG nodes from connections (if not already built)
        // Only add unique node connections (not per-channel duplicates)
        dagNodes.clear();
        for (const auto& conn : connections)
        {
            // Ensure both nodes exist
            dagNodes.try_emplace(conn.source.nodeID, conn.source.nodeID);
            dagNodes.try_emplace(conn.destination.nodeID, conn.destination.nodeID);

            // Build connectivity (deduplicated)
            auto& outputs = dagNodes[conn.source.nodeID].outputsTo;
            if (std::find(outputs.begin(), outputs.end(), conn.destination.nodeID) == outputs.end())
                outputs.push_back(conn.destination.nodeID);

            auto& inputs = dagNodes[conn.destination.nodeID].inputsFrom;
            if (std::find(inputs.begin(), inputs.end(), conn.source.nodeID) == inputs.end())
                inputs.push_back(conn.source.nodeID);
        }

        // Convert audio subgraphs to DAG subgraphs (reuse container)
        dagSubgraphs.clear();
        for (const auto& sg : subgraphs)
        {
            DagPartitioner<NodeID>::Subgraph dagSg;
            dagSg.nodeIDs = sg.nodeIDs;
            dagSg.dependsOn = sg.dependsOn;
            dagSg.dependents = sg.dependents;
            dagSg.topologicalLevel = sg.topologicalLevel;
            dagSubgraphs.push_back(std::move(dagSg));
        }

        // Use DagPartitioner to build dependencies with worker-aware load balancing
        partitioner.buildSubgraphDependencies(dagSubgraphs, dagNodes, numWorkers);

        // Copy results back to audio subgraphs
        for (size_t i = 0; i < subgraphs.size(); ++i)
        {
            subgraphs[i].dependsOn = dagSubgraphs[i].dependsOn;
            subgraphs[i].dependents = dagSubgraphs[i].dependents;
            subgraphs[i].topologicalLevel = dagSubgraphs[i].topologicalLevel;
        }
    }

private:
    // Preallocated containers reused across analysis calls
    DagPartitioner<NodeID> partitioner;
    std::vector<Connection> connections;
    std::vector<Subgraph> subgraphs;
    std::vector<DagPartitioner<NodeID>::Subgraph> dagSubgraphs;
    std::map<NodeID, DagPartitioner<NodeID>::Node> dagNodes;
    std::vector<NodeID> inputNodes;
    std::vector<NodeID> outputNodes;
};

} // namespace atk
