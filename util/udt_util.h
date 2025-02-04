//  Copyright (c) Meta Platforms, Inc. and affiliates.
//
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#pragma once
#include <memory>
#include <optional>
#include <sstream>
#include <unordered_map>
#include <vector>

#include "db/write_batch_internal.h"
#include "rocksdb/slice.h"
#include "rocksdb/status.h"
#include "rocksdb/write_batch.h"
#include "util/coding.h"

namespace ROCKSDB_NAMESPACE {

// Dummy record in WAL logs signaling user-defined timestamp sizes for
// subsequent records.
class UserDefinedTimestampSizeRecord {
 public:
  UserDefinedTimestampSizeRecord() {}
  explicit UserDefinedTimestampSizeRecord(
      std::vector<std::pair<uint32_t, size_t>>&& cf_to_ts_sz)
      : cf_to_ts_sz_(std::move(cf_to_ts_sz)) {}

  const std::vector<std::pair<uint32_t, size_t>>& GetUserDefinedTimestampSize()
      const {
    return cf_to_ts_sz_;
  }

  inline void EncodeTo(std::string* dst) const {
    assert(dst != nullptr);
    for (const auto& [cf_id, ts_sz] : cf_to_ts_sz_) {
      assert(ts_sz != 0);
      PutFixed32(dst, cf_id);
      PutFixed16(dst, static_cast<uint16_t>(ts_sz));
    }
  }

  inline Status DecodeFrom(Slice* src) {
    const size_t total_size = src->size();
    if ((total_size % kSizePerColumnFamily) != 0) {
      std::ostringstream oss;
      oss << "User-defined timestamp size record length: " << total_size
          << " is not a multiple of " << kSizePerColumnFamily << std::endl;
      return Status::Corruption(oss.str());
    }
    int num_of_entries = static_cast<int>(total_size / kSizePerColumnFamily);
    for (int i = 0; i < num_of_entries; i++) {
      uint32_t cf_id = 0;
      uint16_t ts_sz = 0;
      if (!GetFixed32(src, &cf_id) || !GetFixed16(src, &ts_sz)) {
        return Status::Corruption(
            "Error decoding user-defined timestamp size record entry");
      }
      cf_to_ts_sz_.emplace_back(cf_id, static_cast<size_t>(ts_sz));
    }
    return Status::OK();
  }

  inline std::string DebugString() const {
    std::ostringstream oss;

    for (const auto& [cf_id, ts_sz] : cf_to_ts_sz_) {
      oss << "Column family: " << cf_id
          << ", user-defined timestamp size: " << ts_sz << std::endl;
    }
    return oss.str();
  }

 private:
  // 4 bytes for column family id, 2 bytes for user-defined timestamp size.
  static constexpr size_t kSizePerColumnFamily = 4 + 2;

  std::vector<std::pair<uint32_t, size_t>> cf_to_ts_sz_;
};

// This handler is used to recover a WriteBatch read from WAL logs during
// recovery. It does a best-effort recovery if the column families contained in
// the WriteBatch have inconsistency between the recorded timestamp size and the
// running timestamp size. And creates a new WriteBatch that are consistent with
// the running timestamp size with entries from the original WriteBatch.
//
// Note that for a WriteBatch with no inconsistency, a new WriteBatch is created
// nonetheless, and it should be exactly the same as the original WriteBatch.
//
// To access the new WriteBatch, invoke `TransferNewBatch` after calling
// `Iterate`. The handler becomes invalid afterwards.
//
// For the user key in each entry, the best effort recovery means:
// 1) If recorded timestamp size is 0, running timestamp size is > 0, a min
// timestamp of length running timestamp size is padded to the user key.
// 2) If recorded timestamp size is > 0, running timestamp size is 0, the last
// bytes of length recorded timestamp size is stripped from user key.
// 3) If recorded timestamp size is the same as running timestamp size, no-op.
// 4) If recorded timestamp size and running timestamp size are both non-zero
// but not equal, return Status::InvalidArgument.
class TimestampRecoveryHandler : public WriteBatch::Handler {
 public:
  TimestampRecoveryHandler(
      const std::unordered_map<uint32_t, size_t>& running_ts_sz,
      const std::unordered_map<uint32_t, size_t>& record_ts_sz);

  ~TimestampRecoveryHandler() override {}

  // No copy or move.
  TimestampRecoveryHandler(const TimestampRecoveryHandler&) = delete;
  TimestampRecoveryHandler(TimestampRecoveryHandler&&) = delete;
  TimestampRecoveryHandler& operator=(const TimestampRecoveryHandler&) = delete;
  TimestampRecoveryHandler& operator=(TimestampRecoveryHandler&&) = delete;

  Status PutCF(uint32_t cf, const Slice& key, const Slice& value) override;

  Status DeleteCF(uint32_t cf, const Slice& key) override;

  Status SingleDeleteCF(uint32_t cf, const Slice& key) override;

  Status DeleteRangeCF(uint32_t cf, const Slice& begin_key,
                       const Slice& end_key) override;

  Status MergeCF(uint32_t cf, const Slice& key, const Slice& value) override;

  Status PutBlobIndexCF(uint32_t cf, const Slice& key,
                        const Slice& value) override;

  Status MarkBeginPrepare(bool) override { return Status::OK(); }

  Status MarkEndPrepare(const Slice&) override { return Status::OK(); }

  Status MarkCommit(const Slice&) override { return Status::OK(); }

  Status MarkCommitWithTimestamp(const Slice&, const Slice&) override {
    return Status::OK();
  }

  Status MarkRollback(const Slice&) override { return Status::OK(); }

  Status MarkNoop(bool /*empty_batch*/) override { return Status::OK(); }

  std::unique_ptr<WriteBatch>&& TransferNewBatch() {
    assert(new_batch_diff_from_orig_batch_);
    handler_valid_ = false;
    return std::move(new_batch_);
  }

 private:
  Status ReconcileTimestampDiscrepancy(uint32_t cf, const Slice& key,
                                       std::string* new_key_buf,
                                       Slice* new_key);

  // Mapping from column family id to user-defined timestamp size for all
  // running column families including the ones with zero timestamp size.
  const std::unordered_map<uint32_t, size_t>& running_ts_sz_;

  // Mapping from column family id to user-defined timestamp size as recorded
  // in the WAL. This only contains non-zero user-defined timestamp size.
  const std::unordered_map<uint32_t, size_t>& record_ts_sz_;

  std::unique_ptr<WriteBatch> new_batch_;
  // Handler is valid upon creation and becomes invalid after its `new_batch_`
  // is transferred.
  bool handler_valid_;

  // False upon creation, and become true if at least one user key from the
  // original batch is updated when creating the new batch.
  bool new_batch_diff_from_orig_batch_;
};

// Mode for checking and handling timestamp size inconsistency encountered in a
// WriteBatch read from WAL log.
enum class TimestampSizeConsistencyMode {
  // Verified that the recorded user-defined timestamp size is consistent with
  // the running one for all the column families involved in a WriteBatch.
  // Column families referred to in the WriteBatch but are dropped are ignored.
  kVerifyConsistency,
  // Verified that if any inconsistency exists in a WriteBatch, it's all
  // tolerable by a best-effort reconciliation. And optionally creates a new
  // WriteBatch from the original WriteBatch that is consistent with the running
  // timestamp size. Column families referred to in the WriteBatch but are
  // dropped are ignored. If a new WriteBatch is created, such entries are
  // copied over as is.
  kReconcileInconsistency,
};

// Handles the inconsistency between recorded timestamp sizes and running
// timestamp sizes for a WriteBatch. A non-OK `status` indicates there are
// intolerable inconsistency with the specified `check_mode`.
//
// If `check_mode` is `kVerifyConsistency`, intolerable inconsistency means any
// running column family has an inconsistent user-defined timestamp size.
//
// If `check_mode` is `kReconcileInconsistency`, intolerable inconsistency means
// any running column family has an inconsistent user-defined timestamp size
// that cannot be reconciled with a best-effort recovery. Check
// `TimestampRecoveryHandler` for what a best-effort recovery is capable of. In
// this mode, output argument `new_batch` should be set, a new WriteBatch is
// created on the heap and transferred to `new_batch` if there is tolerable
// inconsistency.
//
// An invariant that WAL logging ensures is that all timestamp size info
// is logged prior to a WriteBatch that needed this info. And zero timestamp
// size is skipped. So `record_ts_sz` only contains column family with non-zero
// timestamp size and a column family id absent from `record_ts_sz` will be
// interpreted as that column family has zero timestamp size. On the other hand,
// `running_ts_sz` should contain the timestamp size for all running column
// families including the ones with zero timestamp size.
Status HandleWriteBatchTimestampSizeDifference(
    const WriteBatch* batch,
    const std::unordered_map<uint32_t, size_t>& running_ts_sz,
    const std::unordered_map<uint32_t, size_t>& record_ts_sz,
    TimestampSizeConsistencyMode check_mode,
    std::unique_ptr<WriteBatch>* new_batch = nullptr);
}  // namespace ROCKSDB_NAMESPACE
