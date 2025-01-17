/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2021 Intel Corporation
 */

#pragma once

#include "allocator.hpp"
#include "assert.h"
#include "atomic"
#include "kvdk/engine.hpp"
#include "logger.hpp"
#include "structures.hpp"
#include <string>
#include <sys/mman.h>

namespace KVDK_NAMESPACE {

// Chunk based simple implementation
// TODO: optimize, implement free
class ChunkBasedAllocator : Allocator {
public:
  SizedSpaceEntry Allocate(uint64_t size) override;
  void Free(const SizedSpaceEntry &entry) override;
  inline void *offset2addr(uint64_t offset) { return (void *)offset; }
  template <typename T> inline T *offset2addr(uint64_t offset) {
    return static_cast<T *>(offset);
  }
  inline uint64_t addr2offset(void *addr) { return (uint64_t)addr; }
  ChunkBasedAllocator(uint32_t write_threads) : thread_cache_(write_threads) {}
  ~ChunkBasedAllocator() {
    for (auto &tc : thread_cache_) {
      for (void *chunk : tc.allocated_chunks) {
        free(chunk);
      }
    }
  }

private:
  struct ThreadCache {
    alignas(64) char *chunk_addr = nullptr;
    uint64_t usable_bytes = 0;
    std::vector<void *> allocated_chunks;
  };

  const uint32_t chunk_size_ = (1 << 20);
  std::vector<ThreadCache> thread_cache_;
};
} // namespace KVDK_NAMESPACE