#pragma once

#include "AudioProcessorGraphMT.h"
#include "DagPartitioner.h"
#include <map>
#include <vector>

namespace atk
{

/**
 * @brief Extracts subgraphs from a JUCE AudioProcessorGraphMT for parallel processing.
 *
 * This class bridges between JUCE's audio graph types and the general-purpose
 * DagPartitioner. It handles audio-specific concerns like:
 * - Converting JUCE Node/Connection types to generic DAG nodes/links
 * - Identifying audio and MIDI I/O nodes
 * - Preserving connection information in subgraphs
 */
class SubgraphExtractor
{
public:
    using NodeID = AudioProcessorGraphMT::NodeID;
    using Node = AudioProcessorGraphMT::Node;
    using Connection = AudioProcessorGraphMT::Connection;

    /**
     * @brief Represents a subgraph of audio processors.
     *
     * Contains JUCE-specific information about nodes and connections
     * in addition to the partitioning information from DagPartitioner.
     */
    struct Subgraph
    {
        std::vector<NodeID> nodeIDs;         // Audio processor nodes in this subgraph
        std::vector<NodeID> inputNodeIDs;    // Input I/O nodes feeding this subgraph
        std::vector<NodeID> outputNodeIDs;   // Output I/O nodes this subgraph feeds to
        std::vector<Connection> connections; // JUCE connections within this subgraph
        std::vector<size_t> dependsOn;       // Indices of subgraphs this one depends on
        std::vector<size_t> dependents;      // Indices of subgraphs that depend on this one
        int topologicalLevel = 0;            // Level in dependency hierarchy
    };

    SubgraphExtractor() = default;

    /**
     * @brief Set parallelization threshold for DagPartitioner.
     * @param threshold Minimum number of nodes to enable parallel processing (0 = always parallel, default = 20)
     */
    void setParallelThreshold(size_t threshold)
    {
        partitioner.setParallelThreshold(threshold);
    }

    /**
     * @brief Extract subgraphs from an AudioProcessorGraphMT for parallel processing.
     *
     * This method:
     * 1. Identifies all audio/MIDI I/O nodes
     * 2. Converts the audio graph to generic DAG representation
     * 3. Uses DagPartitioner to extract parallelizable subgraphs
     * 4. Converts results back to audio-specific subgraph representation
     *
     * Both audio AND MIDI connections create dependencies - a node must wait for all inputs.
     * I/O nodes (audio and MIDI) are excluded from subgraphs as they're handled externally.
     *
     * @param graph The AudioProcessorGraphMT to extract subgraphs from
     * @return Vector of extracted subgraphs with audio-specific information
     */
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
        for (const auto& conn : connections)
        {
            auto srcIt = dagNodes.find(conn.source.nodeID);
            auto dstIt = dagNodes.find(conn.destination.nodeID);

            if (srcIt != dagNodes.end())
                srcIt->second.outputsTo.push_back(conn.destination.nodeID);

            if (dstIt != dagNodes.end())
                dstIt->second.inputsFrom.push_back(conn.source.nodeID);
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

    /**
     * @brief Build dependency relationships between subgraphs and assign topological levels.
     *
     * This method analyzes connections between subgraphs to determine which can run in parallel.
     * Handles both audio AND MIDI connections when determining dependencies.
     *
     * @param subgraphs Vector of subgraphs (will be modified in-place)
     * @param connections Vector of all JUCE connections in the graph
     */
    void buildSubgraphDependencies(std::vector<Subgraph>& subgraphs, const std::vector<Connection>& connections)
    {
        if (subgraphs.empty())
            return;

        // Build DAG nodes from connections (if not already built)
        dagNodes.clear();
        for (const auto& conn : connections)
        {
            // Ensure both nodes exist
            dagNodes.try_emplace(conn.source.nodeID, conn.source.nodeID);
            dagNodes.try_emplace(conn.destination.nodeID, conn.destination.nodeID);

            // Build connectivity
            dagNodes[conn.source.nodeID].outputsTo.push_back(conn.destination.nodeID);
            dagNodes[conn.destination.nodeID].inputsFrom.push_back(conn.source.nodeID);
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

        // Use DagPartitioner to build dependencies
        partitioner.buildSubgraphDependencies(dagSubgraphs, dagNodes);

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
