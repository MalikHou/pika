//  Copyright (c) 2017-present, Qihoo, Inc.  All rights reserved.
//  This source code is licensed under the BSD-style license found in the
//  LICENSE file in the root directory of this source tree. An additional grant
//  of patent rights can be found in the PATENTS file in the same directory.

#include <sstream>

#include "src/redis.h"
#include "pstd/include/pstd_string.h"
#include <glog/logging.h>

namespace storage {

Redis::Redis(Storage* const s, const DataType& type)
    : storage_(s),
      type_(type),
      lock_mgr_(std::make_shared<LockMgr>(1000, 0, std::make_shared<MutexFactoryImpl>())),
      small_compaction_threshold_(5000),
      small_compaction_duration_threshold_(10000) {
  statistics_store_ = std::make_unique<LRUCache<std::string, KeyStatistics>>();
  scan_cursors_store_ = std::make_unique<LRUCache<std::string, std::string>>();
  scan_cursors_store_->SetCapacity(5000);
  default_compact_range_options_.exclusive_manual_compaction = false;
  default_compact_range_options_.change_level = true;
  handles_.clear();
}

Redis::~Redis() {
  std::vector<rocksdb::ColumnFamilyHandle*> tmp_handles = handles_;
  handles_.clear();
  for (auto handle : tmp_handles) {
    delete handle;
  }
  delete db_;

  if (default_compact_range_options_.canceled) {
    delete default_compact_range_options_.canceled;
  }
}

Status Redis::GetScanStartPoint(const Slice& key, const Slice& pattern, int64_t cursor, std::string* start_point) {
  std::string index_key = key.ToString() + "_" + pattern.ToString() + "_" + std::to_string(cursor);
  return scan_cursors_store_->Lookup(index_key, start_point);
}

Status Redis::StoreScanNextPoint(const Slice& key, const Slice& pattern, int64_t cursor,
                                 const std::string& next_point) {
  std::string index_key = key.ToString() + "_" + pattern.ToString() + "_" + std::to_string(cursor);
  return scan_cursors_store_->Insert(index_key, next_point);
}

Status Redis::SetMaxCacheStatisticKeys(size_t max_cache_statistic_keys) {
  statistics_store_->SetCapacity(max_cache_statistic_keys);
  return Status::OK();
}

Status Redis::SetSmallCompactionThreshold(uint64_t small_compaction_threshold) {
  small_compaction_threshold_ = small_compaction_threshold;
  return Status::OK();
}

Status Redis::SetSmallCompactionDurationThreshold(uint64_t small_compaction_duration_threshold) {
  small_compaction_duration_threshold_ = small_compaction_duration_threshold;
  return Status::OK();
}

Status Redis::UpdateSpecificKeyStatistics(const std::string& key, uint64_t count) {
  if ((statistics_store_->Capacity() != 0U) && (count != 0U) && (small_compaction_threshold_ != 0U)) {
    KeyStatistics data;
    statistics_store_->Lookup(key, &data);
    data.AddModifyCount(count);
    statistics_store_->Insert(key, data);
    AddCompactKeyTaskIfNeeded(key, data.ModifyCount(), data.AvgDuration());
  }
  return Status::OK();
}

Status Redis::UpdateSpecificKeyDuration(const std::string& key, uint64_t duration) {
  if ((statistics_store_->Capacity() != 0U) && (duration != 0U) && (small_compaction_duration_threshold_ != 0U)) {
    KeyStatistics data;
    statistics_store_->Lookup(key, &data);
    data.AddDuration(duration);
    statistics_store_->Insert(key, data);
    AddCompactKeyTaskIfNeeded(key, data.ModifyCount(), data.AvgDuration());
  }
  return Status::OK();
}

Status Redis::AddCompactKeyTaskIfNeeded(const std::string& key, uint64_t count, uint64_t duration) {
  if (count < small_compaction_threshold_ || duration < small_compaction_duration_threshold_) {
    return Status::OK();
  } else {
    storage_->AddBGTask({type_, kCompactRange, {key, key}});
    statistics_store_->Remove(key);
  }
  return Status::OK();
}

Status Redis::SetOptions(const OptionType& option_type, const std::unordered_map<std::string, std::string>& options) {
  if (option_type == OptionType::kDB) {
    return db_->SetDBOptions(options);
  }
  if (handles_.empty()) {
    return db_->SetOptions(db_->DefaultColumnFamily(), options);
  }
  Status s;
  for (auto handle : handles_) {
    s = db_->SetOptions(handle, options);
    if (!s.ok()) {
      break;
    }
  }
  return s;
}

void Redis::GetRocksDBInfo(std::string &info, const char *prefix) {
    std::ostringstream string_stream;
    string_stream << "#" << prefix << "RocksDB" << "\r\n";

    auto write_stream_key_value=[&](const Slice& property, const char *metric) {
        uint64_t value;
        db_->GetAggregatedIntProperty(property, &value);
        string_stream << prefix << metric << ':' << value << "\r\n";
    };

    auto mapToString=[&](const std::map<std::string, std::string>& map_data, const char *prefix) {
      for (const auto& kv : map_data) {
        std::string str_data;
        str_data += kv.first + ": " + kv.second + "\r\n";
        string_stream << prefix << str_data;
      }
    };

    // memtables num
    write_stream_key_value(rocksdb::DB::Properties::kNumImmutableMemTable, "num_immutable_mem_table");
    write_stream_key_value(rocksdb::DB::Properties::kNumImmutableMemTableFlushed, "num_immutable_mem_table_flushed");
    write_stream_key_value(rocksdb::DB::Properties::kMemTableFlushPending, "mem_table_flush_pending");
    write_stream_key_value(rocksdb::DB::Properties::kNumRunningFlushes, "num_running_flushes");

    // compaction
    write_stream_key_value(rocksdb::DB::Properties::kCompactionPending, "compaction_pending");
    write_stream_key_value(rocksdb::DB::Properties::kNumRunningCompactions, "num_running_compactions");

    // background errors
    write_stream_key_value(rocksdb::DB::Properties::kBackgroundErrors, "background_errors");

    // memtables size
    write_stream_key_value(rocksdb::DB::Properties::kCurSizeActiveMemTable, "cur_size_active_mem_table");
    write_stream_key_value(rocksdb::DB::Properties::kCurSizeAllMemTables, "cur_size_all_mem_tables");
    write_stream_key_value(rocksdb::DB::Properties::kSizeAllMemTables, "size_all_mem_tables");

    // keys
    write_stream_key_value(rocksdb::DB::Properties::kEstimateNumKeys, "estimate_num_keys");

    // table readers mem
    write_stream_key_value(rocksdb::DB::Properties::kEstimateTableReadersMem, "estimate_table_readers_mem");

    // snapshot
    write_stream_key_value(rocksdb::DB::Properties::kNumSnapshots, "num_snapshots");

    // version
    write_stream_key_value(rocksdb::DB::Properties::kNumLiveVersions, "num_live_versions");
    write_stream_key_value(rocksdb::DB::Properties::kCurrentSuperVersionNumber, "current_super_version_number");

    // live data size
    write_stream_key_value(rocksdb::DB::Properties::kEstimateLiveDataSize, "estimate_live_data_size");

    // sst files
    write_stream_key_value(rocksdb::DB::Properties::kTotalSstFilesSize, "total_sst_files_size");
    write_stream_key_value(rocksdb::DB::Properties::kLiveSstFilesSize, "live_sst_files_size");

    // pending compaction bytes
    write_stream_key_value(rocksdb::DB::Properties::kEstimatePendingCompactionBytes, "estimate_pending_compaction_bytes");

    // block cache
    write_stream_key_value(rocksdb::DB::Properties::kBlockCacheCapacity, "block_cache_capacity");
    write_stream_key_value(rocksdb::DB::Properties::kBlockCacheUsage, "block_cache_usage");
    write_stream_key_value(rocksdb::DB::Properties::kBlockCachePinnedUsage, "block_cache_pinned_usage");

    // blob files
    write_stream_key_value(rocksdb::DB::Properties::kNumBlobFiles, "num_blob_files");
    write_stream_key_value(rocksdb::DB::Properties::kBlobStats, "blob_stats");
    write_stream_key_value(rocksdb::DB::Properties::kTotalBlobFileSize, "total_blob_file_size");
    write_stream_key_value(rocksdb::DB::Properties::kLiveBlobFileSize, "live_blob_file_size");
    
    // column family stats
    std::map<std::string, std::string> mapvalues;
    db_->rocksdb::DB::GetMapProperty(rocksdb::DB::Properties::kCFStats,&mapvalues);
    mapToString(mapvalues,prefix);
    info.append(string_stream.str());
}

void Redis::SetWriteWalOptions(const bool is_wal_disable) {
  default_write_options_.disableWAL = is_wal_disable;
}

void Redis::SetCompactRangeOptions(const bool is_canceled) {
  if (!default_compact_range_options_.canceled) {
    default_compact_range_options_.canceled = new std::atomic<bool>(is_canceled);
  } else {
    default_compact_range_options_.canceled->store(is_canceled);
  } 
}

Status Redis::FullCompact(const ColumnFamilyType& type = kMetaAndData) {
  return db_->CompactRange(default_compact_range_options_, nullptr, nullptr);
}

Status Redis::LongestNotCompactiontSstCompact(const ColumnFamilyType& type = kMetaAndData) {
  rocksdb::TablePropertiesCollection props;
  Status status = db_->GetPropertiesOfAllTables(&props);
  if (!status.ok()) {
    return Status::Corruption("LongestNotCompactiontSstCompact GetPropertiesOfAllTables:" + status.ToString());
  }

  // The main goal of compaction was reclaimed the disk space and removed
  // the tombstone. It seems that compaction scheduler was unnecessary here when
  // the live files was too few, Hard code to 1 here.
  if (props.size() <= 1) {
      return Status::Corruption("LongestNotCompactiontSstCompact only one file");
  }

  size_t max_files_to_compact = 1;
  if (props.size() / num_sst_docompact_once_ > max_files_to_compact) {
      max_files_to_compact = props.size() / num_sst_docompact_once_;
  }

  int64_t now =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
          .count();

  auto force_compact_min_ratio = static_cast<double>(force_compact_min_delete_ratio_) / 100.0;
  auto best_delete_min_ratio = static_cast<double>(best_delete_min_ratio_) / 100.0;

  std::string best_filename;
  double best_delete_ratio = 0;
  int64_t total_keys = 0, deleted_keys = 0;
  rocksdb::Slice start_key, stop_key, best_start_key, best_stop_key;
  for (const auto& iter : props) {
      uint64_t file_creation_time = iter.second->file_creation_time;
      if (file_creation_time == 0) {
          // Fallback to the file Modification time to prevent repeatedly compacting the same file,
          // file_creation_time is 0 which means the unknown condition in rocksdb
          auto s = rocksdb::Env::Default()->GetFileModificationTime(iter.first, &file_creation_time);
          if (!s.ok()) {
              LOG(INFO) << "Failed to get the file creation time: " << iter.first
                         << ", err: " << s.ToString();
              continue;
          }
      }

      for (const auto& property_iter : iter.second->user_collected_properties) {
          if (property_iter.first == "total_keys") {
              if (!pstd::string2int(property_iter.second.c_str(), property_iter.second.length(), &total_keys)) {
                  LOG(ERROR) << "Parse total_keys error";
                  continue;
              }
          }
          if (property_iter.first == "deleted_keys") {
              if (!pstd::string2int(property_iter.second.c_str(), property_iter.second.length(), &deleted_keys)) {
                  LOG(ERROR) << "Parse deleted_keys error";
                  continue;
              }
          }
          if (property_iter.first == "start_key") {
              start_key = property_iter.second;
          }
          if (property_iter.first == "stop_key") {
              stop_key = property_iter.second;
          }
      }

      if (start_key.empty() || stop_key.empty()) {
          continue;
      }
      double delete_ratio = static_cast<double>(deleted_keys) / static_cast<double>(total_keys);

      // pick the file according to force compact policy
      if (file_creation_time < static_cast<uint64_t>(now/1000 - force_compact_file_age_seconds_) &&
          delete_ratio >= force_compact_min_ratio) {
          auto s = db_->CompactRange(default_compact_range_options_, &start_key, &stop_key);
          max_files_to_compact--;
          continue;
      }

      // don't compact the SST created in x `dont_compact_sst_created_in_seconds_`.
      if (file_creation_time > static_cast<uint64_t>(now - dont_compact_sst_created_in_seconds_)) {
          continue;
      }

      // pick the file which has highest delete ratio
      if (total_keys != 0 && delete_ratio > best_delete_ratio) {
          best_delete_ratio = delete_ratio;
          best_filename     = iter.first;
          best_start_key    = start_key;
          start_key.clear();
          best_stop_key = stop_key;
          stop_key.clear();
      }
  }
  
  if (best_delete_ratio > best_delete_min_ratio && !best_start_key.empty() && !best_stop_key.empty()) {
      auto s = db_->CompactRange(default_compact_range_options_, &best_start_key, &best_stop_key);
      if (!s.ok()) {
          LOG(ERROR) << "Failed to do compaction: " << s.ToString();
      }
  }
  return Status::OK();
}

}  // namespace storage
