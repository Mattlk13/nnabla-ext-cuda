// Copyright (c) 2017 Sony Corporation. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef __NBLA_CUDA_PREFETCH_HPP__
#define __NBLA_CUDA_PREFETCH_HPP__

#include <vector>
#include <unordered_map>
#include <numeric>

#include <cuda_runtime.h>

#include <nbla/cuda/defs.hpp>
#include <nbla/computation_graph/function.hpp>
#include <nbla/computation_graph/variable.hpp>
#include <nbla/synced_array.hpp>
#include <nbla/cuda/cuda.hpp>
#include <nbla/cpu.hpp>


namespace nbla {

using std::vector;
using std::unordered_map;
using std::string;
using std::accumulate;

/** A class which manages GPU memory usage and schedules swap in/out
    throughout network computation.
    
    If your GPU memory is insufficient to train a large model,
    SwapInOutScheduler enables you to compute it fast and effectively
    by memory swapping.

    This implements an algorithm to prefetch arrays used in a future function
    and to swap out arrays already used. The swapped-out arrays are swapped in 
    in the same way as prefetch. This class schedules the timing of prefetch
    (swap in) and swap out for fast computation of a network.

    This scheduler records the order of arrays usage in the first iteration of
    train a model. After the next iteration, the arrays are prefetched following
    the order. That is why users should not change the order of computation
    under the management of this scheduler for computation speed. For example, 
    the disorder and speed-down happens when logging some array's values 
    every ten iteraetion in a scheduling block.

    A scheduler takes the size of GPU memory which you want to manage in the initializer.
    For example, when your GPU memory is up to 4 GB, the initialization is
    @code
    SwapInOutScheduler scheduler(cpu_ctx, 4e9);
    @endcode
    If out-of--memory error occurs in this case, the reduction of the size from 4e9 to
    e.g. 3.5e9 could solve the problem.

    This scheduler can be used easily by enclosing a training code between
    SwapInOutScheduler::start_scheduling() and SwapInOutScheduler::end_scheduling(),
    and setting the callback functions into the arguments of forward, backward, and update.
    For example, in an iteration,
    @code
    shceduler.start_scheduling();
    loss->forward(false, true, nullptr,
                  [&](const CgFunctionPtr &ptr) { scheduler.pre_function_callback(ptr); },
                  [&](const CgFunctionPtr &ptr) { scheduler.post_function_callback(ptr); });
    loss->variable()->grad()->fill(1.0);
    loss->backward(nullptr, true, {},
                   [&](const CgFunctionPtr &ptr) { scheduler.pre_function_callback(ptr); },
                   [&](const CgFunctionPtr &ptr) { scheduler.post_function_callback(ptr); });
    adam->update([&]() { swap_in_out_scheduler.pre_update_callback(); },
                 [&]() { swap_in_out_scheduler.post_update_callback(); });
    scheduler.end_scheduling();
    @endcode
*/
class SwapInOutScheduler {
  // Recorded tags of get/cast/clear
  enum class RecTag {GET, CAST, CAST_WRITE_ONLY, CLEAR};

  // Recorded information for get/cast/clear
  struct RecType {
    const RecTag tag;
    SyncedArrayPtr saptr;
    dtypes dtype;
    const Context ctx;
    bool preclear; // If true, the saptr can be cleared after this record.
    bool swapped_out;
    size_t swapped_out_bytes;
  };

  const Context host_ctx; // Host context for swap-out

  // The maximum size of usable GPU memory [byte]
  const size_t max_bytes_swap_in;  // for swap-in
  const size_t max_bytes_swap_out; // for waiting to swap-out

  // The used size of GPU memory [bytes]
  size_t used_bytes_swap_in = 0;  // for swap-in
  size_t used_bytes_swap_out = 0; // for waiting to swap-out

  // The recorded order of get/cast/clear in the first iteration
  vector<RecType> rec_order;
  // The order of get/cast/clear different from recorded order in current iteration
  vector<RecType> wrong_ordered;

  /* The scheduled swap-in/out are managed as a sliding queue throughout 
     the recorded order. The head of the queue goes ahead when swap-in (prefetch).
     The tail of the queue moves on when waiting to finish swap-out 
     and to release memory.
     
                      rec_tail               rec_use_idx         rec_head
    rec_order --|-----[-------|---------|----*-----|----------|--]-----|-----
                                          function_idx          head_function_idx

    -      : A hyphen is a record of get/cast/clear.
    |----| : An interval is the records used in a function.
    |      : A vertical line is one of functtion_ends.
    [  ]   : A queue.
  */
  size_t rec_use_idx = 0;  // pointing the get/cast/clear just used in a function.
  int rec_head = 0;        // pointing the next record to swap-in (prefetch).
  int rec_tail = 0;        // pointing the next record to wait for swap-out
  size_t function_idx = 0; // pointing the current function in the recorded order.
  // The intervals in the recorded order which used in a function 
  vector<size_t> rec_function_ends = {0}; // 0 is set for convinience.
                                          // function_idx==0 points this virtual 
                                          // 0-th function.

                                          /* This counts the number of same arrays in the queue.
     If count of an array > 0, no need to fetch the same array again.
     If count of an array > 1, no need to swap out the array because it is
     planed to be used soon in the queue.
  */
  unordered_map<SyncedArrayPtr, unordered_map<dtypes, int>> swap_in_counts;

  int accumulate_counts(const unordered_map<dtypes, int>& count_map) {
    return accumulate(count_map.begin(), count_map.end(), 0,
      [](int value, const unordered_map<dtypes, int>::value_type& p)
        { return value + p.second; });
  }

  /* Remember arrays which are pre-cleared by SwapInOutScheduler in order to
     check the error by the unpredicted insertion of get/cast in the recorded order
     after clear.
  */
  unordered_map<SyncedArrayPtr, bool> precleared;

  // This is a switch separating the first iteration and others.
  bool first_iter = true;

  // Array classes are used to distinguish an array on GPU or CPU.
  vector<string> cpu_array_classes = SingletonManager::get<Cpu>()->array_classes();
  vector<string> cuda_array_classes = SingletonManager::get<Cuda>()->array_classes();

public:
  /** Constructor.

  @param h_ctx Host context used as the destination of swap-out.
  @param bytes Maximum GPU memory size managed by this class [bytes].
  */
  NBLA_CUDA_API SwapInOutScheduler(const Context &h_ctx,
                                   const size_t bytes);

  /** Destructor.
  */
  NBLA_CUDA_API ~SwapInOutScheduler();

  /** This initialize the scheduler and start the management of GPU memory 
      in this iteration.
  */
  NBLA_CUDA_API void start_scheduling();

  /** This finalize the scheduler and end the management of GPU memory 
      in this iteration.
  */
  NBLA_CUDA_API void end_scheduling();

  /** To use the scheduler, this callback must be set in the pre-function-hook
      arguments of forward and backward function.
  */
  NBLA_CUDA_API void pre_function_callback(const CgFunctionPtr &ptr);

  /** To use the scheduler, this callback must be set in the post-function-hook
      arguments of forward and backward function.
  */
  NBLA_CUDA_API void post_function_callback(const CgFunctionPtr &ptr);

  /** To use the scheduler, this callback must be set in the pre-update-hook
      argument of the update method of a solver.
  */
  NBLA_CUDA_API void pre_update_callback();

  /** To use the scheduler, this callback must be set in the post-update-hook
      argument of the update method of a solver.
  */
  NBLA_CUDA_API void post_update_callback();

private:
  // Common implementations of pre-function and pre-update callbacks
  void pre_callback_impl();

  /* Pre callback function is separated the two part.
     1. The post-process of the previous function.
     2. The pre-process of the next function.

     In this implementation, post callback function does not perform 1.
     That is because NNabla calls clear of inputs and outputs between
     post and pre callback function. it is easier to deal with the clear
     at the begining of pre callback function.
  */
  void proc_for_prev_func(); // 1.
  void proc_for_next_func(); // 2.


  // SyncedArray callback function to record get/cast/clear in the first iteration.
  void synced_array_callback_recorder(SyncedArrayPtr saptr,
                                      const SyncedArrayCallbackTag func_name,
                                      const dtypes dtype,                                           
                                      const Context &ctx,
                                      const bool write_only);

  // SyncedArray callback function to trace get/cast/clear after the first iteration.
  void synced_array_callback_tracer(SyncedArrayPtr saptr,
                                    const SyncedArrayCallbackTag func_name,
                                    const dtypes dtype,                                         
                                    const Context &ctx,
                                    const bool write_only);

  // Setter of SyncedArray callback function according to the number of iterations.
  void set_synced_array_callback();

  // Remover of SyncedArray callback function.
  void unset_synced_array_callback();

  void swap_in(); // swap in (prefetch)
  void swap_out(); // swap out
  void swap_out_impl(); // a subroutine of swap_out in charge of swap-out.
  void wait_swap_out(RecType& r); // a subroutine of swap_out in charge of
                                  // waiting for swap-out and releasing GPU memory.

  void init(); // Initialization subroutine of start_scheduling
  void finalize(); // Finalization subroutine of the end_scheduling
  void schedule_preclear(); // Subroutine to schedule preclear of end_scheduling

  /* Utilities
  */
  // Converter to RecTag
  RecTag get_tag(const SyncedArrayCallbackTag func_name, const bool write_only);

  // Checker of CPU array class 
  inline bool is_cpu_array(const string array_class) {
    return std::find(cpu_array_classes.begin(),
                     cpu_array_classes.end(),
                     array_class) != cpu_array_classes.end();
  }

  // Checker of GPU array class 
  inline bool is_gpu_array(const string array_class) {
    return std::find(cuda_array_classes.begin(), 
                     cuda_array_classes.end(), 
                     array_class) != cuda_array_classes.end();
  }
};
}
#endif
