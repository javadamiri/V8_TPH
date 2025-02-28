// Copyright 2020 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/heap/safepoint.h"

#include "src/handles/local-handles.h"
#include "src/handles/persistent-handles.h"
#include "src/heap/gc-tracer.h"
#include "src/heap/heap-inl.h"
#include "src/heap/local-heap.h"

namespace v8 {
namespace internal {

GlobalSafepoint::GlobalSafepoint(Heap* heap)
    : heap_(heap), local_heaps_head_(nullptr), active_safepoint_scopes_(0) {}

void GlobalSafepoint::EnterSafepointScope() {
  if (!FLAG_local_heaps) return;

  if (++active_safepoint_scopes_ > 1) return;

  TimedHistogramScope timer(heap_->isolate()->counters()->time_to_safepoint());
  TRACE_GC(heap_->tracer(), GCTracer::Scope::STOP_THE_WORLD);

  local_heaps_mutex_.Lock();
  local_heap_of_this_thread_ = LocalHeap::Current();

  barrier_.Arm();

  for (LocalHeap* current = local_heaps_head_; current;
       current = current->next_) {
    if (current == local_heap_of_this_thread_) {
      continue;
    }
    current->RequestSafepoint();
  }

  for (LocalHeap* current = local_heaps_head_; current;
       current = current->next_) {
    if (current == local_heap_of_this_thread_) {
      continue;
    }
    current->state_mutex_.Lock();

    while (current->state_ == LocalHeap::ThreadState::Running) {
      current->state_change_.Wait(&current->state_mutex_);
    }
  }
}

void GlobalSafepoint::LeaveSafepointScope() {
  if (V8_ENABLE_THIRD_PARTY_HEAP_BOOL) return;
  if (!FLAG_local_heaps) return;

  DCHECK_GT(active_safepoint_scopes_, 0);
  if (--active_safepoint_scopes_ > 0) return;

  DCHECK_EQ(local_heap_of_this_thread_, LocalHeap::Current());

  for (LocalHeap* current = local_heaps_head_; current;
       current = current->next_) {
    if (current == local_heap_of_this_thread_) {
      continue;
    }
    current->state_mutex_.Unlock();
  }

  barrier_.Disarm();

  local_heap_of_this_thread_ = nullptr;
  local_heaps_mutex_.Unlock();
}

void GlobalSafepoint::EnterFromThread(LocalHeap* local_heap) {
  {
    base::MutexGuard guard(&local_heap->state_mutex_);
    DCHECK_EQ(local_heap->state_, LocalHeap::ThreadState::Running);
    local_heap->state_ = LocalHeap::ThreadState::Safepoint;
    local_heap->state_change_.NotifyAll();
  }

  barrier_.Wait();

  {
    base::MutexGuard guard(&local_heap->state_mutex_);
    local_heap->state_ = LocalHeap::ThreadState::Running;
  }
}

void GlobalSafepoint::Barrier::Arm() {
  base::MutexGuard guard(&mutex_);
  CHECK(!armed_);
  armed_ = true;
}

void GlobalSafepoint::Barrier::Disarm() {
  base::MutexGuard guard(&mutex_);
  CHECK(armed_);
  armed_ = false;
  cond_.NotifyAll();
}

void GlobalSafepoint::Barrier::Wait() {
  base::MutexGuard guard(&mutex_);
  while (armed_) {
    cond_.Wait(&mutex_);
  }
}

SafepointScope::SafepointScope(Heap* heap) : safepoint_(heap->safepoint()) {
  if (V8_ENABLE_THIRD_PARTY_HEAP_BOOL) return;
  safepoint_->EnterSafepointScope();
}

SafepointScope::~SafepointScope() { safepoint_->LeaveSafepointScope(); }

void GlobalSafepoint::AddLocalHeap(LocalHeap* local_heap) {
  base::MutexGuard guard(&local_heaps_mutex_);
  if (local_heaps_head_) local_heaps_head_->prev_ = local_heap;
  local_heap->prev_ = nullptr;
  local_heap->next_ = local_heaps_head_;
  local_heaps_head_ = local_heap;
}

void GlobalSafepoint::RemoveLocalHeap(LocalHeap* local_heap) {
  base::MutexGuard guard(&local_heaps_mutex_);
  if (local_heap->next_) local_heap->next_->prev_ = local_heap->prev_;
  if (local_heap->prev_)
    local_heap->prev_->next_ = local_heap->next_;
  else
    local_heaps_head_ = local_heap->next_;
}

bool GlobalSafepoint::ContainsLocalHeap(LocalHeap* local_heap) {
  base::MutexGuard guard(&local_heaps_mutex_);
  LocalHeap* current = local_heaps_head_;

  while (current) {
    if (current == local_heap) return true;
    current = current->next_;
  }

  return false;
}

bool GlobalSafepoint::ContainsAnyLocalHeap() {
  base::MutexGuard guard(&local_heaps_mutex_);
  return local_heaps_head_ != nullptr;
}

void GlobalSafepoint::Iterate(RootVisitor* visitor) {
  DCHECK(IsActive());
  for (LocalHeap* current = local_heaps_head_; current;
       current = current->next_) {
    current->handles()->Iterate(visitor);
  }
}

}  // namespace internal
}  // namespace v8
