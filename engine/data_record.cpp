/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2021 Intel Corporation
 */

#include "data_record.hpp"
#include "libpmem.h"

namespace KVDK_NAMESPACE {
// use buffer to acc nt-write
thread_local std::string thread_data_buffer;
static constexpr int kDataBufferSize = 1024 * 1024;

StringRecord *StringRecord::PersistStringRecord(
    void *addr, uint32_t record_size, uint64_t timestamp, RecordType type,
    const pmem::obj::string_view &key, const pmem::obj::string_view &value) {
  void *data_cpy_target;
  auto write_size = key.size() + value.size() + sizeof(StringRecord);
  bool with_buffer = write_size <= kDataBufferSize;
  if (with_buffer) {
    if (thread_data_buffer.empty()) {
      thread_data_buffer.resize(kDataBufferSize);
    }
    data_cpy_target = &thread_data_buffer[0];
  } else {
    data_cpy_target = addr;
  }
  StringRecord *record = StringRecord::ConstructStringRecord(
      data_cpy_target, record_size, timestamp, type, key, value);
  if (with_buffer) {
    pmem_memcpy(addr, data_cpy_target, write_size, PMEM_F_MEM_NONTEMPORAL);
    pmem_drain();
  } else {
    pmem_persist(addr, write_size);
  }

  return static_cast<StringRecord *>(addr);
}

DLRecord *DLRecord::PersistDLRecord(void *addr, uint32_t record_size,
                                    uint64_t timestamp, RecordType type,
                                    uint64_t prev, uint64_t next,
                                    const pmem::obj::string_view &key,
                                    const pmem::obj::string_view &value) {
  void *data_cpy_target;
  auto write_size = key.size() + value.size() + sizeof(DLRecord);
  bool with_buffer = write_size <= kDataBufferSize;
  if (with_buffer) {
    if (thread_data_buffer.empty()) {
      thread_data_buffer.resize(kDataBufferSize);
    }
    data_cpy_target = &thread_data_buffer[0];
  } else {
    data_cpy_target = addr;
  }
  DLRecord *record = DLRecord::ConstructDLRecord(
      data_cpy_target, record_size, timestamp, type, prev, next, key, value);
  if (with_buffer) {
    pmem_memcpy(addr, data_cpy_target, write_size, PMEM_F_MEM_NONTEMPORAL);
    pmem_drain();
  } else {
    pmem_persist(addr, write_size);
  }

  return static_cast<DLRecord *>(addr);
}

} // namespace KVDK_NAMESPACE