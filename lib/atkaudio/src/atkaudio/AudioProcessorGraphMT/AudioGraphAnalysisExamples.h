#pragma once

#include "AudioGraphAnalysis.h"
#include <juce_audio_processors/juce_audio_processors.h>

/**
    Example usage of AudioGraphAnalysis tools.

    This demonstrates how to use the analysis tools to understand
    and optimize AudioProcessorGraph structures.
*/

namespace AudioGraphAnalysisExamples
{
using namespace juce;

/**
    Example 1: Basic graph analysis
*/
inline void analyzeGraphExample(AudioProcessorGraph& graph)
{
    // Get comprehensive metrics
    auto metrics = GraphAnalyzer::analyzeGraph(graph);

    // Display metrics
    DBG(metrics.getDescription());

    // Get optimization suggestions
    auto suggestions = GraphAnalyzer::getOptimizationSuggestions(metrics);
    DBG("Optimization Suggestions:");
    DBG(suggestions);

    // Check if graph could theoretically be parallelized
    if (metrics.isParallelizable)
    {
        DBG("This graph has " << metrics.independentChainCount << " independent chains.");
        DBG("Note: JUCE processes sequentially by design.");
    }
}

/**
    Example 2: Visualize topological levels
*/
inline void visualizeTopologyExample(AudioProcessorGraph& graph)
{
    AudioGraphTopology topology;
    auto levels = topology.computeLevels(graph);

    DBG("Graph has " << levels.size() << " topological levels:");

    for (size_t i = 0; i < levels.size(); ++i)
    {
        DBG("Level " << i << ": " << levels[i].size() << " nodes");

        // List nodes at this level
        for (auto* node : levels[i])
        {
            auto* proc = node->getProcessor();
            DBG("  - " << proc->getName());
        }
    }
}

/**
    Example 3: Identify independent chains
*/
inline void identifyChainsExample(AudioProcessorGraph& graph)
{
    auto subgraphs = GraphPartitioner::extractIndependentSubgraphs(graph);

    if (subgraphs.empty())
    {
        DBG("No independent chains found - graph must be processed sequentially.");
        return;
    }

    DBG("Found " << subgraphs.size() << " independent chains:");

    for (size_t i = 0; i < subgraphs.size(); ++i)
    {
        auto& subgraph = subgraphs[i];

        DBG("Chain " << i << ":");
        DBG("  Nodes: " << subgraph.nodeIDs.size());
        DBG("  Connections: " << subgraph.connections.size());
        DBG("  Estimated latency: " << subgraph.estimatedLatency << " samples");

        // List nodes in this chain
        for (auto nodeID : subgraph.nodeIDs)
        {
            if (auto* node = graph.getNodeForId(nodeID))
            {
                auto* proc = node->getProcessor();
                DBG("    - " << proc->getName());
            }
        }
    }

    DBG("\nNote: While these chains are independent, JUCE's AudioProcessorGraph");
    DBG("processes them sequentially. For true parallelism, use multiple");
    DBG("AudioProcessorGraph instances (one per track/bus).");
}

/**
    Example 4: Performance analysis
*/
inline void analyzePerformanceCharacteristics(AudioProcessorGraph& graph)
{
    auto metrics = GraphAnalyzer::analyzeGraph(graph);

    // Estimate processing characteristics
    DBG("Performance Analysis:");

    if (metrics.parallelismFactor < 1.5f)
    {
        DBG("  Type: Sequential pipeline");
        DBG("  Processing: One plugin at a time (efficient for this structure)");
    }
    else if (metrics.parallelismFactor < 3.0f)
    {
        DBG("  Type: Moderately parallel");
        DBG("  Processing: Some branching, mostly sequential");
    }
    else
    {
        DBG("  Type: Highly parallel");
        DBG("  Processing: Multiple independent branches");
        DBG("  Note: Consider splitting into separate tracks for real parallelism");
    }

    // Complexity assessment
    if (metrics.averageConnectionsPerNode > 4.0f)
    {
        DBG("  Complexity: High (complex routing)");
        DBG("  Suggestion: May benefit from simplification");
    }
    else if (metrics.averageConnectionsPerNode > 2.0f)
    {
        DBG("  Complexity: Moderate");
    }
    else
    {
        DBG("  Complexity: Low (simple chain)");
    }
}

/**
    Example 5: Integration into UI

    This shows how you might use these tools to provide
    feedback to users in your audio application.
*/
inline String getGraphStatusForUI(AudioProcessorGraph& graph)
{
    auto metrics = GraphAnalyzer::analyzeGraph(graph);

    String status;
    status << metrics.totalNodes << " plugin";
    if (metrics.totalNodes != 1)
        status << "s";

    status << ", " << metrics.totalConnections << " connection";
    if (metrics.totalConnections != 1)
        status << "s";

    if (metrics.isParallelizable)
        status << "\n(" << metrics.independentChainCount << " independent chains detected)";

    return status;
}
} // namespace AudioGraphAnalysisExamples
