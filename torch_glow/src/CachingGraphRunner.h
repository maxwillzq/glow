/**
 * Copyright (c) Glow Contributors. See CONTRIBUTORS file.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef GLOW_TORCH_GLOW_SRC_CACHINGGRAPHRUNNER_H
#define GLOW_TORCH_GLOW_SRC_CACHINGGRAPHRUNNER_H

#include "PyTorchModelLoader.h"
#include "glow/Runtime/HostManager/HostManager.h"
#include "glow/Support/TensorPool.h"

#include <torch/csrc/jit/ir/ir.h>
#include <torch/csrc/jit/serialization/import.h>

#include <shared_mutex>

namespace glow {

/// For a given PyTorch JIT graph, this class is responsible for maintaining a
/// mapping from PyTorch input information to Glow Function used to run that
/// graph in Glow.
class CachingGraphRunner {
  /// Information that is stored per-Glow graph for running it using
  /// HostManager.
  struct PerGlowGraphInfo {
    /// Input and output placeholders to the Glow function.
    std::vector<glow::Placeholder *> inputPlaceholders;
    std::vector<glow::Placeholder *> outputPlaceholders;

    /// Name of the Glow function maintained by HostManager for this subgraph.
    std::string functionName;
  };

  /// The PyTorch JIT Graph that this CachingGraphRunner caches Glow functions
  /// for.
  std::shared_ptr<torch::jit::Graph> graph_;

  /// GraphExecutor used to execute graph_ on PyTorch for debugging purposes.
  torch::jit::GraphExecutor ptGraphExecutor_;

  /// The HostManager used to store and run Glow graphs.
  std::shared_ptr<runtime::HostManager> hostManager_;

  /// The Backend associated with the HostManager
  const Backend &backend_;

  /// Tensorpool to host padded tensors
  TensorPool tensorPool_;

  /// Mapping from hash of PyTorch inputs to PerGlowGraphInfo for the Glow
  /// function that will run inputs matching that hash.
  std::unordered_map<size_t, std::shared_ptr<PerGlowGraphInfo>>
      perGlowGraphInfoMap_;

  /// Indicate which type will propagate to output.
  /// It is supposely to be the correct PyTorch ScalarType
  /// in the corresponding JIT node for each output
  /// placeholder from Glow graph.
  /// Use for quantization int8/uint8 rescale.
  std::vector<c10::ScalarType> outputCorrectType_;

  /// Mutex that protects numTraces_ and mergedTraceContext_.
  std::mutex tracesMutex_;

  /// The number of runs traced
  size_t numTraces_{0};

  /// TraceContext used to aggregate traces from runs before dumping them
  /// in groups to file.
  std::unique_ptr<TraceContext> mergedTraceContext_;

  /// Lock for concurrent accessing to perGlowGraphInfoMap_.
  std::shared_timed_mutex graphInfoMapMutex;

  /// The number of times any Glow graph managed by this CachingGraphRunner has
  /// been run.
  std::atomic<size_t> numRuns_{0};

  /// The maximum size of input tensors, to allocate the zerolength tensor
  size_t maxSeqLength_ = 1;

  /// Pre-allocated tensor for zero-length tensors, to avoid repeated zero
  /// paddings during ExecutionContext building
  glow::Tensor zeroLengthSequence_;

  /// Given a PyTorch input stack \p stack, this generates a hash from the
  /// values on the stack and checks to see if a matching function was loaded
  /// previously. If a matching function was loaded previously then its cached
  /// info is returned immediately. Otherwise this loads the
  /// subgraph into the owned HostManager, creates a PerGlowGraphInfo which is
  /// cached for the given inputs, and then \returns this PerGlowGraphInfo.
  Expected<std::shared_ptr<PerGlowGraphInfo>>
  loadImpl(torch::jit::Stack &stack, TraceContext *traceContext);

  /// Given a PerGlowGraphInfo \p info for a subgraph that was previously
  /// loaded, this runs the Glow function that corresponds to that
  /// PerGlowGraphInfo in the shape of the inputs with the given \p stack with
  /// the given ExecutionContext \p ctx.
  Error runImpl(const PerGlowGraphInfo &info, torch::jit::Stack &stack,
                std::unique_ptr<ExecutionContext> &ctx);

  /// Run the graph_ on \p stack on using ptGraphExecutor_. This is for
  /// debugging purposes only.
  void runOnJit(torch::jit::Stack &stack);

  /// Given a \p stack of inputs, computes the hash for the inputs on the stack.
  size_t computeGraphHash(const c10::ArrayRef<c10::IValue> inputs) const;

  /// Store the settings that were used to create the JIT subgraph that this
  /// CachingGraphRunner owns.
  PyTorchLoaderSettings settings_;

  /// Given a TraceContext \p traceContext, aggregate it with prvious
  /// TraceContexts and if enough have been aggregated according to settings_
  /// then dump them to file. If flush is true then dump aggregated traces to
  /// file no matter what.
  void aggregateAndDumpTraces(TraceContext *traceContext, bool flush = false);

public:
  CachingGraphRunner(std::shared_ptr<torch::jit::Graph> graph,
                     std::shared_ptr<runtime::HostManager> hostManager,
                     const char *backendName, PyTorchLoaderSettings settings);

  ~CachingGraphRunner();

  /// Given a PyTorch Stack \p stack of inputs, run he stored PyTorch graph on
  /// those inputs. If this is the first time this PyTorch graph has been run
  /// with inputs matching the hash of those on the stack then this first loads
  /// it as a Glow Function and compiles. \returns error of failure.
  Error run(torch::jit::Stack &stack);

  /// The Glow Function should've already been created. Returns an error if not.
  Error runOnly(torch::jit::Stack &stack);

  // Warm up the cache by compiling a Glow function for the inputs in \p stack.
  Error warmCache(const std::vector<InputMeta> &inputMeta);

  const PyTorchLoaderSettings &getSettings() const;
};

} // namespace glow

#endif // GLOW_TORCH_GLOW_SRC_CACHINGGRAPHRUNNER_H
