# Parallel Audio Processing

This folder contains the multi-threaded audio processing implementation for PluginHost2's AudioProcessorGraph.

## Overview

The parallel processing system enables concurrent execution of independent audio processing chains within an AudioProcessorGraph, maximizing CPU utilization on multi-core systems while maintaining sample-accurate timing through automatic delay compensation.

## Architecture

```
AudioProcessorGraphMT (inherits AudioProcessorGraph)
    └── composed with AudioProcessorGraphMT_Impl (friend access)
            └── uses AudioGraphAnalysis::SubgraphExtractor
```

## Core Components

### AudioProcessorGraphMT.h

- **Purpose**: Public-facing API that IS an AudioProcessorGraph with optional parallel capabilities
- **Pattern**: Inheritance (IS-A) + Composition (HAS-A) + Friend Access
- **Key Methods**:
  - `setUseAudioProcessorGraphMT(bool)` - Toggle parallel processing on/off
  - `isUsingAudioProcessorGraphMT()` - Query current processing mode
  - `getNumIndependentSubgraphs()` - Get number of parallelizable chains
  - `updateTopology()` - Rebuild parallel processing structure after graph changes
- **Usage**: Drop-in replacement for `AudioProcessorGraph`

### AudioProcessorGraphMT_Impl.h

- **Purpose**: Internal parallel processing engine
- **Components**:
  - `ThreadBarrier` - Reference-counted synchronization primitive
  - `SubgraphWorkerThread` - Persistent realtime-priority worker threads
  - `SubgraphDelayCompensation` - Per-subgraph latency alignment
- **Pattern**: Friend class to AudioProcessorGraphMT (prevents recursion)

### AudioGraphAnalysis.h

- **Purpose**: Graph topology analysis and subgraph extraction
- **Classes**:
  - `AudioGraphTopology` - Topological sorting using Kahn's algorithm
  - `SubgraphExtractor` - Identifies independent parallel processing chains
- **Algorithms**: Dependency analysis, level assignment, connectivity detection

### AudioGraphAnalysisExamples.h

- **Purpose**: Example code demonstrating usage patterns
- **Contents**: Sample code for topology analysis and parallel processing setup

### ParallelGraphIntegration.h

- **Purpose**: Helper functions for integrating parallel processing
- **Contents**: Convenience utilities for common integration tasks

## Key Design Decisions

### 1. Inheritance over Composition

**Why**: AudioProcessorGraphMT INHERITS from AudioProcessorGraph

- Ensures true type compatibility (IS-A relationship)
- Maintains all AudioProcessor virtual methods
- Preserves UI integration (input/output pins visible)
- Allows seamless drop-in replacement

### 2. Friend Access Pattern

**Why**: Prevents infinite recursion in lifecycle methods

- AudioProcessorGraphMT declares AudioProcessorGraphMT_Impl as friend
- Allows direct access to private members to avoid delegating back to graph
- Critical for `prepareToPlay()` and `releaseResources()` implementations

### 3. Manual Node Processing

**Why**: JUCE's RenderSequence is private and non-extensible

- Extracts independent subgraphs manually
- Processes each subgraph on dedicated worker thread
- Bypasses RenderSequence entirely for parallel chains
- Falls back to base class for sequential processing

### 4. Persistent Worker Threads

**Why**: Realtime-safe execution without thread creation overhead

- Worker threads created during `prepareToPlay()`
- Kept alive with condition variable synchronization
- Realtime priority 8 (high priority for time-critical audio processing, range 0-10 where default is 5)
- Destroyed only in `releaseResources()`

### 5. Automatic Delay Compensation

**Why**: Different subgraphs may have different latencies

- Measures plugin-reported latency per subgraph
- Aligns all subgraphs using `dsp::DelayLine`
- Sample-accurate phase coherence
- Automatically updates when topology changes

## Integration

### In PluginGraph.h

```cpp
#include "../AudioProcessorGraphMT/AudioProcessorGraphMT.h"

class PluginGraph {
    AudioProcessorGraphMT graph;  // Changed from AudioProcessorGraph
    
    // Call after any graph modification:
    void addPlugin(...) {
        // ... add nodes ...
        graph.updateTopology();
    }
};
```

### In GraphEditorPanel.h

```cpp
#include "../AudioProcessorGraphMT/AudioProcessorGraphMT_Impl.h"

// Access parallel processing info:
auto* parallelGraph = dynamic_cast<AudioProcessorGraphMT*>(&graph);
if (parallelGraph) {
    int numSubgraphs = parallelGraph->getNumIndependentSubgraphs();
    parallelGraph->setUseAudioProcessorGraphMT(true);
}
```

## Performance Characteristics

### When Parallel Processing Helps

- Multiple independent plugin chains
- CPU-intensive plugins (reverbs, convolution, synthesis)
- Multi-core systems (4+ cores recommended)
- Graphs with clear parallel structure (e.g., multiple send effects)

### When Sequential is Better

- Single linear chain
- Low-latency requirements with simple processing
- Graphs with high interconnectivity
- Systems with few CPU cores (< 4)

### Overhead

- Per-frame: ~10-50 μs synchronization overhead
- Per-topology change: ~1-5 ms for subgraph extraction
- Memory: ~16 KB per worker thread + per-node buffers

## Testing Recommendations

1. **Graph Complexity**: Test with graphs of 5-50 nodes
2. **Parallel Structure**: Create multiple independent send chains
3. **CPU Load**: Use CPU-intensive plugins to see benefits
4. **Latency**: Verify delay compensation with analyzer plugins
5. **Realtime Safety**: Run with JUCE's `JUCE_CHECK_MEMORY_LEAKS` enabled
6. **Thread Safety**: Test rapid topology changes during playback

## Documentation

See the `docs/` subfolder for detailed documentation:

- `Parallel_Processing_Architecture.md` - Comprehensive architecture guide
- `Multithreaded_AudioGraph_Design.md` - Design decisions and rationale
- `Independent_Subgraph_Processing.md` - Subgraph extraction algorithm
- `Delay_Compensation_Implementation.md` - Latency alignment details
- `Parallel_Processing_Implementation.md` - Implementation walkthrough
- `Parallel_Processing_Visual_Guide.md` - Diagrams and visual explanations
- `Parallel_Processing_Conclusion.md` - Summary and future improvements

## Future Enhancements

- [ ] SIMD optimization for buffer operations
- [ ] Work stealing between threads for load balancing
- [ ] Adaptive thread pool sizing based on graph complexity
- [ ] GPU acceleration for compatible effects
- [ ] Performance profiling integration
- [ ] Automatic CPU core affinity tuning

## License

Same as parent project (see LICENSE files in repository root).
