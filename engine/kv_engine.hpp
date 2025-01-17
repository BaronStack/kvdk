/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2021 Intel Corporation
 */

#pragma once

#include <atomic>
#include <cassert>
#include <cstdint>
#include <deque>
#include <iostream>
#include <list>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "data_record.hpp"
#include "dram_allocator.hpp"
#include "hash_list.hpp"
#include "hash_table.hpp"
#include "kvdk/engine.hpp"
#include "logger.hpp"
#include "pmem_allocator/pmem_allocator.hpp"
#include "skiplist.hpp"
#include "structures.hpp"
#include "thread_manager.hpp"
#include "time.h"
#include "unordered_collection.hpp"
#include "utils.hpp"

namespace KVDK_NAMESPACE {
class KVEngine : public Engine {
  friend class SortedCollectionRebuilder;

public:
  KVEngine();
  ~KVEngine();

  static Status Open(const std::string &name, Engine **engine_ptr,
                     const Configs &configs);

  // Global Anonymous Collection
  Status Get(const pmem::obj::string_view key, std::string *value) override;
  Status Set(const pmem::obj::string_view key,
             const pmem::obj::string_view value) override;
  Status Delete(const pmem::obj::string_view key) override;
  Status BatchWrite(const WriteBatch &write_batch) override;

  // Sorted Collection
  Status SGet(const pmem::obj::string_view collection,
              const pmem::obj::string_view user_key,
              std::string *value) override;
  Status SSet(const pmem::obj::string_view collection,
              const pmem::obj::string_view user_key,
              const pmem::obj::string_view value) override;
  // TODO: Release delete record and deleted nodes
  Status SDelete(const pmem::obj::string_view collection,
                 const pmem::obj::string_view user_key) override;
  std::shared_ptr<Iterator>
  NewSortedIterator(const pmem::obj::string_view collection) override;

  // Unordered Collection
  virtual Status HGet(pmem::obj::string_view const collection_name,
                      pmem::obj::string_view const key,
                      std::string *value) override;
  virtual Status HSet(pmem::obj::string_view const collection_name,
                      pmem::obj::string_view const key,
                      pmem::obj::string_view const value) override;
  virtual Status HDelete(pmem::obj::string_view const collection_name,
                         pmem::obj::string_view const key) override;
  std::shared_ptr<Iterator>
  NewUnorderedIterator(pmem::obj::string_view const collection_name) override;

  void ReleaseWriteThread() override { write_thread.Release(); }

  const std::vector<std::shared_ptr<Skiplist>> &GetSkiplists() {
    return skiplists_;
  };

private:
  struct BatchWriteHint {
    uint64_t timestamp{0};
    SizedSpaceEntry allocated_space{};
    SizedSpaceEntry free_after_finish{};
    bool delay_free{false};
  };

  struct ThreadLocalRes {
    ThreadLocalRes() = default;

    alignas(64) uint64_t newest_restored_ts = 0;
    PendingBatch *persisted_pending_batch = nullptr;
    std::unordered_map<uint64_t, int> visited_skiplist_ids;
  };

  bool CheckKeySize(const pmem::obj::string_view &key) {
    return key.size() <= UINT16_MAX;
  }

  bool CheckValueSize(const pmem::obj::string_view &value) {
    return value.size() <= UINT32_MAX;
  }

  Status Init(const std::string &name, const Configs &configs);

  Status HashGetImpl(const pmem::obj::string_view &key, std::string *value,
                     uint16_t type_mask);

  inline Status MaybeInitWriteThread();

  Status SearchOrInitPersistentList(const pmem::obj::string_view &collection,
                                    PersistentList **list, bool init,
                                    uint16_t header_type);

  Status SearchOrInitSkiplist(const pmem::obj::string_view &collection,
                              Skiplist **skiplist, bool init) {
    if (!CheckKeySize(collection)) {
      return Status::InvalidDataSize;
    }
    return SearchOrInitPersistentList(collection, (PersistentList **)skiplist,
                                      init, SortedHeaderRecord);
  }

private:
  std::shared_ptr<UnorderedCollection>
  createUnorderedCollection(pmem::obj::string_view const collection_name);
  UnorderedCollection *
  findUnorderedCollection(pmem::obj::string_view collection_name);

  Status MaybeInitPendingBatchFile();

  Status StringSetImpl(const pmem::obj::string_view &key,
                       const pmem::obj::string_view &value);

  Status StringDeleteImpl(const pmem::obj::string_view &key);

  Status StringBatchWriteImpl(const WriteBatch::KV &kv,
                              BatchWriteHint &batch_hint);

  Status SSetImpl(Skiplist *skiplist, const pmem::obj::string_view &user_key,
                  const pmem::obj::string_view &value);

  Status SDeleteImpl(Skiplist *skiplist,
                     const pmem::obj::string_view &user_key);

  Status Recovery();

  Status RestoreData(uint64_t thread_id);

  Status RestoreSkiplistHead(DLRecord *pmem_record,
                             const DataEntry &cached_entry);

  Status RestoreStringRecord(StringRecord *pmem_record,
                             const DataEntry &cached_entry);

  Status RestoreSkiplistRecord(DLRecord *pmem_record,
                               const DataEntry &cached_data_entry);

  // Check if a doubly linked record has been successfully inserted, and try
  // repair un-finished prev pointer
  bool CheckAndRepairDLRecord(DLRecord *record);

  bool ValidateRecord(void *data_record);

  bool ValidateRecordAndGetValue(void *data_record, uint32_t expected_checksum,
                                 std::string *value);

  Status RestorePendingBatch();

  Status PersistOrRecoverImmutableConfigs();

  Status RestoreDlistRecords(DLRecord *pmp_record);

  // Regularly works excecuted by background thread
  void BackgroundWork();

  Status CheckConfigs(const Configs &configs);

  void FreeSkiplistDramNodes();

  inline uint64_t get_cpu_tsc() {
    uint32_t lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)lo) | (((uint64_t)hi) << 32);
  }

  inline uint64_t get_timestamp() {
    auto res = get_cpu_tsc() - ts_on_startup_ + newest_version_on_startup_;
    return res;
  }

  inline std::string db_file_name() { return dir_ + "data"; }

  inline std::string persisted_pending_block_file(int thread_id) {
    return pending_batch_dir_ + std::to_string(thread_id);
  }

  inline std::string config_file_name() { return dir_ + "configs"; }

  inline bool checkDLRecordLinkageLeft(DLRecord *pmp_record) {
    uint64_t offset = pmem_allocator_->addr2offset_checked(pmp_record);
    DLRecord *pmem_record_prev =
        pmem_allocator_->offset2addr_checked<DLRecord>(pmp_record->prev);
    return pmem_record_prev->next == offset;
  }

  inline bool checkDLRecordLinkageRight(DLRecord *pmp_record) {
    uint64_t offset = pmem_allocator_->addr2offset_checked(pmp_record);
    DLRecord *pmp_next =
        pmem_allocator_->offset2addr_checked<DLRecord>(pmp_record->next);
    return pmp_next->prev == offset;
  }

  bool isLinkedDLDataEntry(DLRecord *pmp_record) {
    uint64_t offset = pmem_allocator_->addr2offset_checked(pmp_record);
    DLRecord *pmp_prev =
        pmem_allocator_->offset2addr_checked<DLRecord>(pmp_record->prev);
    DLRecord *pmp_next =
        pmem_allocator_->offset2addr_checked<DLRecord>(pmp_record->next);
    bool is_linked_right = (pmp_prev->next == offset);
    bool is_linked_left = (pmp_next->prev == offset);

    if (is_linked_left && is_linked_right) {
      return true;
    } else if (!is_linked_left && !is_linked_right) {
      return false;
    } else if (is_linked_left && !is_linked_right) {
      GlobalLogger.Error(
          "Broken DLDataEntry linkage: prev<=>curr->right, repaired.\n");
      pmp_next->prev = offset;
      pmem_persist(&pmp_next->prev, sizeof(decltype(offset)));
      return true;
    } else {
      GlobalLogger.Error("Broken DLDataEntry linkage: prev<-curr<=>right, "
                         "which is logically impossible! Abort...\n");
      std::abort();
    }
  }

  std::vector<ThreadLocalRes> thread_res_;

  // restored kvs in reopen
  std::atomic<uint64_t> restored_{0};
  std::atomic<uint64_t> list_id_{0};

  uint64_t ts_on_startup_ = 0;
  uint64_t newest_version_on_startup_ = 0;
  std::shared_ptr<HashTable> hash_table_;

  std::vector<std::shared_ptr<Skiplist>> skiplists_;
  std::vector<std::shared_ptr<UnorderedCollection>>
      vec_sp_unordered_collections_;
  std::mutex list_mu_;

  std::string dir_;
  std::string pending_batch_dir_;
  std::string db_file_;
  std::shared_ptr<ThreadManager> thread_manager_;
  std::shared_ptr<PMEMAllocator> pmem_allocator_;
  Configs configs_;
  bool closing_{false};
  std::vector<std::thread> bg_threads_;
  SortedCollectionRebuilder sorted_rebuilder_;
};

} // namespace KVDK_NAMESPACE
