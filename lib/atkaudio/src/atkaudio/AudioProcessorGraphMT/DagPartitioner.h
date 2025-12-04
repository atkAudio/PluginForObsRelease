#pragma once

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace atk
{

/**
 * DAG partitioner for extracting parallelizable subgraphs.
 *
 * Rules:
 * 1) Node exists only once in the graph
 * 2) Node exists in only one subgraph
 * 3) All subgraphs are unique
 * 4) Graph traced from output endpoints towards inputs
 * 5) Every node with NO output connections is an endpoint
 * 6) If no other node outputs to this node, it's an input
 * 7) If node outputs to >1 nodes, it's a split point (subgraph endpoint)
 * 8) If node receives input from >1 nodes, it's a join point (new subgraph starts)
 *
 * A subgraph is a linear chain where each node has exactly 1 input and 1 output (except boundaries).
 */
template <typename NodeIDType>
class DagPartitioner
{
public:
    struct Node
    {
        NodeIDType id;
        std::vector<NodeIDType> outputsTo;
        std::vector<NodeIDType> inputsFrom;

        explicit Node(NodeIDType nodeId = NodeIDType())
            : id(nodeId)
        {
        }

        void clear()
        {
            outputsTo.clear();
            inputsFrom.clear();
        }
    };

    struct Subgraph
    {
        std::vector<NodeIDType> nodeIDs;
        std::vector<size_t> dependsOn;
        std::vector<size_t> dependents;
        int topologicalLevel = 0;

        void clear()
        {
            nodeIDs.clear();
            dependsOn.clear();
            dependents.clear();
            topologicalLevel = 0;
        }
    };

    class ThreadPool
    {
    public:
        explicit ThreadPool(size_t numThreads = 0)
        {
            size_t threads = numThreads > 0 ? numThreads : std::thread::hardware_concurrency();
            threads = std::max(size_t(1), std::min(threads, size_t(8))); // Cap at 8 threads

            for (size_t i = 0; i < threads; ++i)
            {
                workers.emplace_back(
                    [this]
                    {
                        while (true)
                        {
                            std::function<void()> task;
                            {
                                std::unique_lock<std::mutex> lock(queueMutex);
                                condition.wait(lock, [this] { return stop || !tasks.empty(); });

                                if (stop && tasks.empty())
                                    return;

                                if (!tasks.empty())
                                {
                                    task = std::move(tasks.front());
                                    tasks.pop_front();
                                }
                            }

                            if (task)
                            {
                                ++activeTasks;
                                task();
                                --activeTasks;
                            }
                        }
                    }
                );
            }
        }

        ~ThreadPool()
        {
            {
                std::unique_lock<std::mutex> lock(queueMutex);
                stop = true;
            }
            condition.notify_all();
            for (auto& worker : workers)
                if (worker.joinable())
                    worker.join();
        }

        template <class F>
        void enqueue(F&& f)
        {
            {
                std::unique_lock<std::mutex> lock(queueMutex);
                tasks.emplace_back(std::forward<F>(f));
            }
            condition.notify_one();
        }

        void wait()
        {
            // Wait for all tasks to be consumed and completed
            while (true)
            {
                std::unique_lock<std::mutex> lock(queueMutex);
                if (tasks.empty() && activeTasks == 0)
                    return;
                lock.unlock();
                std::this_thread::yield();
            }
        }

        size_t numThreads() const
        {
            return workers.size();
        }

    private:
        std::vector<std::thread> workers;
        std::deque<std::function<void()>> tasks;
        std::mutex queueMutex;
        std::condition_variable condition;
        std::atomic<bool> stop{false};
        std::atomic<int> activeTasks{0};
    };

    DagPartitioner()
        : parallelThreshold(SIZE_MAX) // Default: always use single-threaded (benchmarks show it's faster)
    {
    }

    void setParallelThreshold(size_t threshold)
    {
        parallelThreshold = threshold;
    }

    std::vector<Subgraph>
    extractSubgraphs(const std::map<NodeIDType, Node>& nodes, const std::vector<NodeIDType>& excludeNodeIDs = {})
    {
        subgraphs.clear();
        visited.clear();

        if (nodes.empty())
            return subgraphs;

        // Build exclude set for fast lookup
        excludeSet.clear();
        for (const auto& nodeId : excludeNodeIDs)
            excludeSet.push_back(nodeId);

        // Find all endpoints (nodes where subgraphs end)
        // Rule 5 & 7: numOutputs != 1 means it's an endpoint
        // Also: if all outputs go to excluded nodes, it's an endpoint
        endpoints.clear();
        for (const auto& [nodeId, node] : nodes)
        {
            if (isExcluded(nodeId))
                continue;

            // Count non-excluded outputs
            size_t nonExcludedOutputs = 0;
            for (const auto& outputId : node.outputsTo)
                if (!isExcluded(outputId))
                    ++nonExcludedOutputs;

            if (nonExcludedOutputs != 1) // 0 outputs or >1 outputs = endpoint
                endpoints.push_back(nodeId);
        }

        // Decide whether to parallelize based on graph size
        bool useParallel = nodes.size() > parallelThreshold && endpoints.size() > 1;

        if (useParallel)
        {
            // Parallel tracing: each endpoint becomes a task
            traceBackwardsParallel(endpoints, nodes);
        }
        else
        {
            // Sequential tracing for small graphs
            currentSubgraph.clear();
            for (const auto& endpointId : endpoints)
                if (!isVisited(endpointId))
                    traceBackwards(endpointId, nodes);
        }

        // Handle remaining unvisited nodes with inputs (side-effect processors)
        for (const auto& [nodeId, node] : nodes)
            if (!isExcluded(nodeId) && !isVisited(nodeId) && !node.inputsFrom.empty())
            {
                currentSubgraph.clear();
                traceBackwards(nodeId, nodes);
            }

        // Handle orphan nodes (no connections)
        for (const auto& [nodeId, node] : nodes)
        {
            if (!isExcluded(nodeId) && !isVisited(nodeId))
            {
                Subgraph sg;
                sg.nodeIDs.push_back(nodeId);
                std::lock_guard<std::mutex> lock(subgraphsMutex);
                subgraphs.push_back(std::move(sg));
                visited.push_back(nodeId);
            }
        }

        return subgraphs;
    }

    void buildSubgraphDependencies(
        std::vector<Subgraph>& subgraphs,
        const std::map<NodeIDType, Node>& nodes,
        size_t numWorkers = SIZE_MAX
    )
    {
        if (subgraphs.empty())
            return;

        // Clear existing dependency info
        for (auto& sg : subgraphs)
        {
            sg.dependsOn.clear();
            sg.dependents.clear();
            sg.topologicalLevel = 0;
        }

        // Build inter-subgraph dependencies
        for (size_t i = 0; i < subgraphs.size(); ++i)
        {
            for (size_t j = 0; j < subgraphs.size(); ++j)
            {
                if (i == j)
                    continue;

                // Check if any node in subgraph i outputs to any node in subgraph j
                bool hasConnection = false;
                for (const auto& nodeIdI : subgraphs[i].nodeIDs)
                {
                    auto nodeIt = nodes.find(nodeIdI);
                    if (nodeIt == nodes.end())
                        continue;

                    for (const auto& outputNodeId : nodeIt->second.outputsTo)
                    {
                        if (contains(subgraphs[j].nodeIDs, outputNodeId))
                        {
                            hasConnection = true;
                            break;
                        }
                    }
                    if (hasConnection)
                        break;
                }

                if (hasConnection)
                {
                    subgraphs[j].dependsOn.push_back(i);
                    subgraphs[i].dependents.push_back(j);
                }
            }
        }

        // ALAP (As-Late-As-Possible) scheduling:
        // Assign each subgraph to the latest level where all dependents can still be satisfied
        // This minimizes buffering by running things just-in-time

        // Step 1: Find max depth (longest path from any source to any sink)
        // First do ASAP to find the critical path length
        levelAssigned.clear();
        levelAssigned.resize(subgraphs.size(), false);

        bool changed = true;
        while (changed)
        {
            changed = false;

            for (size_t i = 0; i < subgraphs.size(); ++i)
            {
                if (levelAssigned[i])
                    continue;

                // Check if all dependencies are assigned
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
                    changed = true;
                }
            }
        }

        // Handle any remaining unassigned (cycles)
        int maxLevel = 0;
        for (const auto& sg : subgraphs)
            maxLevel = std::max(maxLevel, sg.topologicalLevel);

        for (size_t i = 0; i < subgraphs.size(); ++i)
        {
            if (!levelAssigned[i])
            {
                subgraphs[i].topologicalLevel = maxLevel + 1;
                levelAssigned[i] = true;
            }
        }

        // Recompute max level
        maxLevel = 0;
        for (const auto& sg : subgraphs)
            maxLevel = std::max(maxLevel, sg.topologicalLevel);

        // Step 2: ALAP - push subgraphs as late as possible
        // Work backwards from sinks: each subgraph goes to (min dependent level - 1)
        // Sinks stay at maxLevel
        levelAssigned.assign(subgraphs.size(), false);

        // First assign sinks (no dependents) to maxLevel
        for (size_t i = 0; i < subgraphs.size(); ++i)
        {
            if (subgraphs[i].dependents.empty())
            {
                subgraphs[i].topologicalLevel = maxLevel;
                levelAssigned[i] = true;
            }
        }

        // Work backwards: assign each subgraph to (min dependent level - 1)
        changed = true;
        while (changed)
        {
            changed = false;

            for (size_t i = 0; i < subgraphs.size(); ++i)
            {
                if (levelAssigned[i])
                    continue;

                // Check if all dependents are assigned
                bool canAssign = true;
                int minDepLevel = std::numeric_limits<int>::max();

                for (auto depIdx : subgraphs[i].dependents)
                {
                    if (!levelAssigned[depIdx])
                    {
                        canAssign = false;
                        break;
                    }
                    minDepLevel = std::min(minDepLevel, subgraphs[depIdx].topologicalLevel);
                }

                if (canAssign)
                {
                    // ALAP: assign to level just before earliest dependent
                    subgraphs[i].topologicalLevel = minDepLevel - 1;
                    levelAssigned[i] = true;
                    changed = true;
                }
            }
        }

        // Handle any remaining unassigned (shouldn't happen, but safety)
        for (size_t i = 0; i < subgraphs.size(); ++i)
        {
            if (!levelAssigned[i])
            {
                subgraphs[i].topologicalLevel = 0;
                levelAssigned[i] = true;
            }
        }

        // Worker-aware load balancing (only if numWorkers is a reasonable limit)
        if (numWorkers == 0 || numWorkers == SIZE_MAX)
            return;

        // For each level (from last to first), if over capacity, pull subgraphs to earlier levels
        // Special case: at level 0, we push to later levels instead (can't go negative)
        int stabilityLimit = static_cast<int>(subgraphs.size()) * 2 + maxLevel + 10;
        int iterations = 0;

        for (int level = maxLevel; level >= 0 && iterations < stabilityLimit; --level, ++iterations)
        {
            // Collect subgraphs at this level
            levelIndices.clear();
            for (size_t i = 0; i < subgraphs.size(); ++i)
                if (subgraphs[i].topologicalLevel == level)
                    levelIndices.push_back(i);

            // If at or under capacity, nothing to do
            if (levelIndices.size() <= numWorkers)
                continue;

            // Calculate slack for each subgraph at this level
            // Slack = current level - max(dependency levels) - 1
            // Higher slack means more room to move earlier
            // Source subgraphs (no dependencies) have slack = level (can go all the way to 0)
            slackValues.clear();
            slackValues.reserve(levelIndices.size());

            for (size_t idx : levelIndices)
            {
                int slack;
                if (subgraphs[idx].dependsOn.empty())
                {
                    // Source subgraph - can be pulled to level 0
                    slack = subgraphs[idx].topologicalLevel;
                }
                else
                {
                    int maxDependencyLevel = -1;
                    for (auto depIdx : subgraphs[idx].dependsOn)
                        maxDependencyLevel = std::max(maxDependencyLevel, subgraphs[depIdx].topologicalLevel);

                    slack = subgraphs[idx].topologicalLevel - maxDependencyLevel - 1;
                }
                slackValues.push_back({idx, slack});
            }

            // Sort by slack (descending) - keep subgraphs with less slack at current level
            // Move subgraphs with more slack to earlier levels
            std::sort(
                slackValues.begin(),
                slackValues.end(),
                [](const auto& a, const auto& b) { return a.second > b.second; }
            );

            // At level 0, we can't pull earlier - topology is at its limit
            // Subgraphs here have dependents at level 1, so they must stay
            if (level == 0)
                continue;

            // Move excess subgraphs to earlier level (if they have slack)
            for (size_t i = 0; i < slackValues.size() - numWorkers; ++i)
            {
                size_t idx = slackValues[i].first;
                int slack = slackValues[i].second;

                if (slack > 0)
                {
                    // Pull to previous level
                    subgraphs[idx].topologicalLevel = level - 1;
                }
                // If slack == 0, can't move - dependency is at previous level
            }
        }
    }

private:
    // Preallocated containers reused across calls
    std::vector<NodeIDType> visited;
    std::vector<NodeIDType> currentSubgraph;
    std::vector<NodeIDType> endpoints;
    std::vector<NodeIDType> excludeSet;
    std::vector<Subgraph> subgraphs;
    std::vector<bool> levelAssigned;
    std::vector<size_t> toRemove;
    std::vector<size_t> levelIndices;
    std::vector<std::pair<size_t, int>> slackValues;

    // Thread synchronization
    std::mutex visitedMutex;
    std::mutex subgraphsMutex;

    // Reusable thread pool (lazy initialized)
    std::unique_ptr<ThreadPool> threadPool;

    // Parallelization threshold
    size_t parallelThreshold;

    /**
     * @brief Parallel backward tracing from multiple endpoints.
     *
     * Each endpoint is traced as a separate task in the thread pool.
     * Thread-safe visited tracking prevents duplicate work.
     */
    void traceBackwardsParallel(const std::vector<NodeIDType>& endpointList, const std::map<NodeIDType, Node>& nodes)
    {
        // Lazy initialize thread pool (reused across calls)
        if (!threadPool)
            threadPool = std::make_unique<ThreadPool>();

        // Submit each endpoint as a task
        for (const auto& endpointId : endpointList)
        {
            threadPool->enqueue(
                [this, endpointId, &nodes]
                {
                    // Check visited with lock
                    {
                        std::lock_guard<std::mutex> lock(visitedMutex);
                        if (isVisited(endpointId))
                            return;
                    }

                    // Thread-local subgraph accumulator
                    std::vector<NodeIDType> localSubgraph;

                    // Trace from this endpoint
                    traceBackwardsThreadSafe(endpointId, nodes, localSubgraph);
                }
            );
        }

        // Wait for all tasks to complete
        threadPool->wait();
    }

    /**
     * @brief Thread-safe backward tracing for parallel execution.
     *
     * Uses thread-local accumulation and locks only for shared state.
     */
    void traceBackwardsThreadSafe(
        NodeIDType nodeId,
        const std::map<NodeIDType, Node>& nodes,
        std::vector<NodeIDType>& localSubgraph
    )
    {
        // Check if already visited (with lock)
        {
            std::lock_guard<std::mutex> lock(visitedMutex);
            if (isVisited(nodeId))
            {
                // Finalize local subgraph if not empty
                if (!localSubgraph.empty())
                {
                    Subgraph sg;
                    sg.nodeIDs = localSubgraph;
                    std::lock_guard<std::mutex> sgLock(subgraphsMutex);
                    subgraphs.push_back(std::move(sg));
                }
                return;
            }
        }

        // Check if excluded
        if (isExcluded(nodeId))
        {
            if (!localSubgraph.empty())
            {
                Subgraph sg;
                sg.nodeIDs = localSubgraph;
                std::lock_guard<std::mutex> lock(subgraphsMutex);
                subgraphs.push_back(std::move(sg));
            }
            return;
        }

        auto nodeIt = nodes.find(nodeId);
        if (nodeIt == nodes.end())
            return;

        const Node& node = nodeIt->second;
        size_t numInputs = node.inputsFrom.size();

        // Rule 8: numInputs != 1 means new subgraph starts here
        if (numInputs != 1)
        {
            // Finalize previous subgraph
            if (!localSubgraph.empty())
            {
                Subgraph sg;
                sg.nodeIDs = localSubgraph;
                std::lock_guard<std::mutex> lock(subgraphsMutex);
                subgraphs.push_back(std::move(sg));
                localSubgraph.clear();
            }

            // Create single-node subgraph
            {
                std::lock_guard<std::mutex> lock(visitedMutex);
                if (!isVisited(nodeId))
                {
                    visited.push_back(nodeId);
                    Subgraph sg;
                    sg.nodeIDs.push_back(nodeId);
                    std::lock_guard<std::mutex> sgLock(subgraphsMutex);
                    subgraphs.push_back(std::move(sg));
                }
            }

            // Continue tracing each input path separately (recursively in same thread)
            for (const auto& inputId : node.inputsFrom)
            {
                std::vector<NodeIDType> newLocalSubgraph;
                traceBackwardsThreadSafe(inputId, nodes, newLocalSubgraph);
            }
            return;
        }

        // Simple linear node - add to local subgraph
        {
            std::lock_guard<std::mutex> lock(visitedMutex);
            if (!isVisited(nodeId))
            {
                localSubgraph.push_back(nodeId);
                visited.push_back(nodeId);
            }
            else
            {
                // Already visited by another thread - finalize current subgraph
                if (!localSubgraph.empty())
                {
                    Subgraph sg;
                    sg.nodeIDs = localSubgraph;
                    std::lock_guard<std::mutex> sgLock(subgraphsMutex);
                    subgraphs.push_back(std::move(sg));
                }
                return;
            }
        }

        if (numInputs == 1)
        {
            // Continue upstream
            traceBackwardsThreadSafe(node.inputsFrom[0], nodes, localSubgraph);
        }
        else // numInputs == 0
        {
            // Reached input - finalize
            if (!localSubgraph.empty())
            {
                Subgraph sg;
                sg.nodeIDs = localSubgraph;
                std::lock_guard<std::mutex> lock(subgraphsMutex);
                subgraphs.push_back(std::move(sg));
            }
        }
    }

    // Helper: check if node is in visited list (NOT thread-safe, use with lock)
    bool isVisited(const NodeIDType& nodeId) const
    {
        return std::find(visited.begin(), visited.end(), nodeId) != visited.end();
    }

    // Helper: check if node is in exclude list
    bool isExcluded(const NodeIDType& nodeId) const
    {
        return std::find(excludeSet.begin(), excludeSet.end(), nodeId) != excludeSet.end();
    }

    // Helper: check if vector contains element
    bool contains(const std::vector<NodeIDType>& vec, const NodeIDType& nodeId) const
    {
        return std::find(vec.begin(), vec.end(), nodeId) != vec.end();
    }

    // Helper: remove element from vector
    void removeElement(std::vector<size_t>& vec, size_t value)
    {
        vec.erase(std::remove(vec.begin(), vec.end(), value), vec.end());
    }

    /**
     * @brief Trace backwards from an endpoint, collecting nodes into subgraphs.
     *
     * Creates new subgraph when:
     * - Current node has != 1 input (join point or start)
     * - Hit an excluded node
     * - Hit already visited node
     */
    void traceBackwards(NodeIDType nodeId, const std::map<NodeIDType, Node>& nodes)
    {
        // Already visited - finalize and stop
        if (isVisited(nodeId))
        {
            finalizeSubgraph();
            return;
        }

        // Hit excluded node - finalize and stop
        if (isExcluded(nodeId))
        {
            finalizeSubgraph();
            return;
        }

        auto nodeIt = nodes.find(nodeId);
        if (nodeIt == nodes.end())
            return;

        const Node& node = nodeIt->second;
        size_t numInputs = node.inputsFrom.size();

        // Rule 8: numInputs != 1 means new subgraph starts here
        if (numInputs != 1)
        {
            // Finalize previous subgraph
            finalizeSubgraph();

            // Create new subgraph with this node
            currentSubgraph.clear();
            currentSubgraph.push_back(nodeId);
            visited.push_back(nodeId);
            finalizeSubgraph();

            // Continue tracing each input path separately
            for (const auto& inputId : node.inputsFrom)
            {
                currentSubgraph.clear();
                traceBackwards(inputId, nodes);
            }
            return;
        }

        // Simple linear node - add to current subgraph and continue
        currentSubgraph.push_back(nodeId);
        visited.push_back(nodeId);

        if (numInputs == 1)
        {
            // Continue upstream
            traceBackwards(node.inputsFrom[0], nodes);
        }
        else // numInputs == 0
        {
            // Reached input - finalize
            finalizeSubgraph();
        }
    }

    /**
     * @brief Finalize current subgraph and add to results.
     */
    void finalizeSubgraph()
    {
        if (currentSubgraph.empty())
            return;

        Subgraph sg;
        sg.nodeIDs = currentSubgraph;

        subgraphs.push_back(std::move(sg));
        currentSubgraph.clear();
    }
};

} // namespace atk
