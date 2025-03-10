// Copyright 2020 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_HEAP_CPPGC_JS_CPP_HEAP_H_
#define V8_HEAP_CPPGC_JS_CPP_HEAP_H_

#if CPPGC_IS_STANDALONE
static_assert(
    false, "V8 targets can not be built with cppgc_is_standalone set to true.");
#endif

#include "include/v8-callbacks.h"
#include "include/v8-cppgc.h"
#include "include/v8-metrics.h"
#include "src/base/flags.h"
#include "src/base/macros.h"
#include "src/base/optional.h"
#include "src/heap/cppgc-js/cross-heap-remembered-set.h"
#include "src/heap/cppgc/heap-base.h"
#include "src/heap/cppgc/marker.h"
#include "src/heap/cppgc/stats-collector.h"
#include "src/logging/metrics.h"
#include "src/objects/js-objects.h"

namespace v8 {

class Isolate;

namespace internal {

class CppMarkingState;
class MinorGCHeapGrowing;

// A C++ heap implementation used with V8 to implement unified heap.
class V8_EXPORT_PRIVATE CppHeap final
    : public cppgc::internal::HeapBase,
      public v8::CppHeap,
      public cppgc::internal::StatsCollector::AllocationObserver,
      public cppgc::internal::GarbageCollector {
 public:
  enum GarbageCollectionFlagValues : uint8_t {
    kNoFlags = 0,
    kReduceMemory = 1 << 1,
    kForced = 1 << 2,
  };

  using GarbageCollectionFlags = base::Flags<GarbageCollectionFlagValues>;
  using StackState = cppgc::internal::StackState;
  using CollectionType = cppgc::internal::CollectionType;

  class MetricRecorderAdapter final : public cppgc::internal::MetricRecorder {
   public:
    static constexpr int kMaxBatchedEvents = 16;

    explicit MetricRecorderAdapter(CppHeap& cpp_heap) : cpp_heap_(cpp_heap) {}

    void AddMainThreadEvent(const GCCycle& cppgc_event) final;
    void AddMainThreadEvent(const MainThreadIncrementalMark& cppgc_event) final;
    void AddMainThreadEvent(
        const MainThreadIncrementalSweep& cppgc_event) final;

    void FlushBatchedIncrementalEvents();

    // The following methods are only used for reporting nested cpp events
    // through V8. Standalone events are reported directly.
    bool FullGCMetricsReportPending() const;
    bool YoungGCMetricsReportPending() const;

    const base::Optional<cppgc::internal::MetricRecorder::GCCycle>
    ExtractLastFullGcEvent();
    const base::Optional<cppgc::internal::MetricRecorder::GCCycle>
    ExtractLastYoungGcEvent();
    const base::Optional<
        cppgc::internal::MetricRecorder::MainThreadIncrementalMark>
    ExtractLastIncrementalMarkEvent();

    void ClearCachedEvents();

   private:
    Isolate* GetIsolate() const;

    v8::metrics::Recorder::ContextId GetContextId() const;

    CppHeap& cpp_heap_;
    v8::metrics::GarbageCollectionFullMainThreadBatchedIncrementalMark
        incremental_mark_batched_events_;
    v8::metrics::GarbageCollectionFullMainThreadBatchedIncrementalSweep
        incremental_sweep_batched_events_;
    base::Optional<cppgc::internal::MetricRecorder::GCCycle>
        last_full_gc_event_;
    base::Optional<cppgc::internal::MetricRecorder::GCCycle>
        last_young_gc_event_;
    base::Optional<cppgc::internal::MetricRecorder::MainThreadIncrementalMark>
        last_incremental_mark_event_;
  };

  class PauseConcurrentMarkingScope final {
   public:
    explicit PauseConcurrentMarkingScope(CppHeap*);

   private:
    base::Optional<cppgc::internal::MarkerBase::PauseConcurrentMarkingScope>
        pause_scope_;
  };

  static void InitializeOncePerProcess();

  static CppHeap* From(v8::CppHeap* heap) {
    return static_cast<CppHeap*>(heap);
  }
  static const CppHeap* From(const v8::CppHeap* heap) {
    return static_cast<const CppHeap*>(heap);
  }

  CppHeap(v8::Platform*,
          const std::vector<std::unique_ptr<cppgc::CustomSpaceBase>>&,
          const v8::WrapperDescriptor&, cppgc::Heap::MarkingType,
          cppgc::Heap::SweepingType);
  ~CppHeap() final;

  CppHeap(const CppHeap&) = delete;
  CppHeap& operator=(const CppHeap&) = delete;

  HeapBase& AsBase() { return *this; }
  const HeapBase& AsBase() const { return *this; }

  void AttachIsolate(Isolate* isolate);
  void DetachIsolate();

  void Terminate();

  void CollectCustomSpaceStatisticsAtLastGC(
      std::vector<cppgc::CustomSpaceIndex>,
      std::unique_ptr<CustomSpaceStatisticsReceiver>);

  void FinishSweepingIfRunning();
  void FinishSweepingIfOutOfWork();

  void InitializeTracing(CollectionType, GarbageCollectionFlags);
  void StartTracing();
  bool AdvanceTracing(double max_duration);
  bool IsTracingDone() const;
  void TraceEpilogue();
  void EnterFinalPause(cppgc::EmbedderStackState stack_state);
  bool FinishConcurrentMarkingIfNeeded();
  void WriteBarrier(JSObject);

  // StatsCollector::AllocationObserver interface.
  void AllocatedObjectSizeIncreased(size_t) final;
  void AllocatedObjectSizeDecreased(size_t) final;
  void ResetAllocatedObjectSize(size_t) final {}

  MetricRecorderAdapter* GetMetricRecorder() const;

  v8::WrapperDescriptor wrapper_descriptor() const {
    return wrapper_descriptor_;
  }

  Isolate* isolate() const { return isolate_; }

  size_t used_size() const {
    return used_size_.load(std::memory_order_relaxed);
  }
  size_t allocated_size() const { return allocated_size_; }

  ::heap::base::Stack* stack() final;

  std::unique_ptr<CppMarkingState> CreateCppMarkingState();
  std::unique_ptr<CppMarkingState> CreateCppMarkingStateForMutatorThread();

  // cppgc::internal::GarbageCollector interface.
  void CollectGarbage(cppgc::internal::GCConfig) override;
  const cppgc::EmbedderStackState* override_stack_state() const override;
  void StartIncrementalGarbageCollection(cppgc::internal::GCConfig) override;
  size_t epoch() const override;

  V8_INLINE void RememberCrossHeapReferenceIfNeeded(
      v8::internal::JSObject host_obj, void* value);
  template <typename F>
  inline void VisitCrossHeapRememberedSetIfNeeded(F f);
  void ResetCrossHeapRememberedSet();

  // Testing-only APIs.
  void EnableDetachedGarbageCollectionsForTesting();
  void CollectGarbageForTesting(CollectionType, StackState);
  void ReduceGCCapabilitiesFromFlagsForTesting();

 private:
  void ReduceGCCapabilitiesFromFlags();

  void FinalizeIncrementalGarbageCollectionIfNeeded(
      cppgc::Heap::StackState) final {
    // For unified heap, CppHeap shouldn't finalize independently (i.e.
    // finalization is not needed) thus this method is left empty.
  }

  void ReportBufferedAllocationSizeIfPossible();

  void StartIncrementalGarbageCollectionForTesting() final;
  void FinalizeIncrementalGarbageCollectionForTesting(
      cppgc::EmbedderStackState) final;

  MarkingType SelectMarkingType() const;
  SweepingType SelectSweepingType() const;

  bool TracingInitialized() const { return collection_type_.has_value(); }

  Isolate* isolate_ = nullptr;
  bool marking_done_ = true;
  // |collection_type_| is initialized when marking is in progress.
  base::Optional<CollectionType> collection_type_;
  GarbageCollectionFlags current_gc_flags_;

  std::unique_ptr<MinorGCHeapGrowing> minor_gc_heap_growing_;
  CrossHeapRememberedSet cross_heap_remembered_set_;

  std::unique_ptr<cppgc::internal::Sweeper::SweepingOnMutatorThreadObserver>
      sweeping_on_mutator_thread_observer_;

  // Buffered allocated bytes. Reporting allocated bytes to V8 can trigger a GC
  // atomic pause. Allocated bytes are buffer in case this is temporarily
  // prohibited.
  int64_t buffered_allocated_bytes_ = 0;

  v8::WrapperDescriptor wrapper_descriptor_;

  bool in_detached_testing_mode_ = false;
  bool force_incremental_marking_for_testing_ = false;
  bool is_in_v8_marking_step_ = false;

  // Used size of objects. Reported to V8's regular heap growing strategy.
  std::atomic<size_t> used_size_{0};
  // Total bytes allocated since the last GC. Monotonically increasing value.
  // Used to approximate allocation rate.
  size_t allocated_size_ = 0;
  // Limit for |allocated_size| in bytes to avoid checking for starting a GC
  // on each increment.
  size_t allocated_size_limit_for_check_ = 0;

  friend class MetricRecorderAdapter;
};

void CppHeap::RememberCrossHeapReferenceIfNeeded(
    v8::internal::JSObject host_obj, void* value) {
  if (!generational_gc_supported()) return;
  DCHECK(isolate_);
  cross_heap_remembered_set_.RememberReferenceIfNeeded(*isolate_, host_obj,
                                                       value);
}

template <typename F>
void CppHeap::VisitCrossHeapRememberedSetIfNeeded(F f) {
  if (!generational_gc_supported()) return;
  DCHECK(isolate_);
  cross_heap_remembered_set_.Visit(*isolate_, std::move(f));
}

DEFINE_OPERATORS_FOR_FLAGS(CppHeap::GarbageCollectionFlags)

}  // namespace internal
}  // namespace v8

#endif  // V8_HEAP_CPPGC_JS_CPP_HEAP_H_
