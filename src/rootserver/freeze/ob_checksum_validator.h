/**
 * Copyright (c) 2021 OceanBase
 * OceanBase CE is licensed under Mulan PubL v2.
 * You can use this software according to the terms and conditions of the Mulan PubL v2.
 * You may obtain a copy of Mulan PubL v2 at:
 *          http://license.coscl.org.cn/MulanPubL-2.0
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PubL v2 for more details.
 */

#ifndef OCEANBASE_ROOTSERVER_FREEZE_OB_CHECKSUM_VALIDATOR_H_
#define OCEANBASE_ROOTSERVER_FREEZE_OB_CHECKSUM_VALIDATOR_H_

#include "share/ob_tablet_checksum_iterator.h"
#include "share/ob_tablet_replica_checksum_iterator.h"
#include "share/ob_freeze_info_proxy.h"
#include "share/ob_zone_merge_info.h"

namespace oceanbase
{
namespace share
{
class ObTabletChecksumItem;
}
namespace rootserver
{
class ObZoneMergeManager;
class ObFreezeInfoManager;
class ObServerManager;
struct ObMergeTimeStatistics;

class ObMergeErrorCallback
{
public:
  ObMergeErrorCallback()
    : is_inited_(false), tenant_id_(OB_INVALID_TENANT_ID),
      zone_merge_mgr_(nullptr)
  {}
  virtual ~ObMergeErrorCallback() {}

  int init(const uint64_t tenant_id, ObZoneMergeManager &zone_merge_mgr);

  int handle_merge_error(const int64_t error_type, const int64_t expected_epoch);

private:
  bool is_inited_;
  uint64_t tenant_id_;
  ObZoneMergeManager *zone_merge_mgr_;
  DISALLOW_COPY_AND_ASSIGN(ObMergeErrorCallback);
};

class ObChecksumValidatorBase
{
public:
  ObChecksumValidatorBase()
    : is_inited_(false), tenant_id_(OB_INVALID_TENANT_ID), is_primary_service_(true),
      need_validate_(false), sql_proxy_(NULL), zone_merge_mgr_(NULL), merge_err_cb_()
  {}
  virtual ~ObChecksumValidatorBase() {}
  virtual int init(const uint64_t tenant_id,
                   const bool is_primary_service,
                   common::ObMySQLProxy &sql_proxy,
                   ObZoneMergeManager &zone_merge_mgr);
  void set_need_validate(const bool need_validate) { need_validate_ = need_validate; }
  bool need_validate() const { return need_validate_; }
  int validate_checksum(const volatile bool &stop,
                        const share::SCN &frozen_scn,
                        const hash::ObHashMap<share::ObTabletLSPair, share::ObTabletCompactionStatus> &tablet_compaction_map,
                        int64_t &table_count,
                        hash::ObHashMap<uint64_t, share::ObTableCompactionInfo> &table_compaction_map,
                        ObMergeTimeStatistics &merge_time_statistics,
                        const int64_t expected_epoch);

  static const int64_t MIN_CHECK_INTERVAL = 10 * 1000 * 1000LL;

protected:
  bool exist_in_table_array(const uint64_t table_id,
                            const common::ObIArray<uint64_t> &table_ids) const;
  int get_table_compaction_info(const share::schema::ObTableSchema &table_schema,
                                hash::ObHashMap<uint64_t, share::ObTableCompactionInfo> &table_compaction_map,
                                share::ObTableCompactionInfo &table_compaction_info);
  // compare 'table_compaction_map' with 'table_ids', and then remove those
  // whose table_id does not exist in 'table_ids' from 'table_compaction_map'.
  // because tables may be dropped during major freeze, and we should skip these dropped tables.
  int remove_not_exist_table(const ObArray<uint64_t> &table_ids,
                             hash::ObHashMap<uint64_t, share::ObTableCompactionInfo> &table_compaction_map);

private:
  virtual int check_all_table_verification_finished(const volatile bool &stop,
                                                    const share::SCN &frozen_scn,
                                                    const hash::ObHashMap<share::ObTabletLSPair, share::ObTabletCompactionStatus> &tablet_compaction_map,
                                                    int64_t &table_count,
                                                    hash::ObHashMap<uint64_t, share::ObTableCompactionInfo> &table_compaction_map,
                                                    ObMergeTimeStatistics &merge_time_statistics,
                                                    const int64_t expected_epoch) = 0;

protected:
  bool is_inited_;
  uint64_t tenant_id_;
  bool is_primary_service_;  // identify ObMajorFreezeServiceType::SERVICE_TYPE_PRIMARY
  bool need_validate_;
  common::ObMySQLProxy *sql_proxy_;
  ObZoneMergeManager *zone_merge_mgr_;
  ObMergeErrorCallback merge_err_cb_;
};

// Mainly to verify checksum between each tablet replicas in primary/standby cluster
class ObTabletChecksumValidator : public ObChecksumValidatorBase
{
public:
  ObTabletChecksumValidator() {}
  virtual ~ObTabletChecksumValidator() {}

private:
  // each table has tablets should finish tablet replica checksum verification.
  // those tables has no tablets just skip verification.
  virtual int check_all_table_verification_finished(const volatile bool &stop,
                                                    const share::SCN &frozen_scn,
                                                    const hash::ObHashMap<share::ObTabletLSPair, share::ObTabletCompactionStatus> &tablet_compaction_map,
                                                    int64_t &table_count,
                                                    hash::ObHashMap<uint64_t, share::ObTableCompactionInfo> &table_compaction_map,
                                                    ObMergeTimeStatistics &merge_time_statistics,
                                                    const int64_t expected_epoch) override;
  // check whether all tablets of this table finished compaction or not,
  // and execute tablet replica checksum verification if this table has tablet.
  int check_table_compaction_finished(const share::schema::ObTableSchema &table_schema,
                                      const share::SCN &frozen_scn,
                                      const hash::ObHashMap<share::ObTabletLSPair, share::ObTabletCompactionStatus> &tablet_compaction_map,
                                      hash::ObHashMap<uint64_t, share::ObTableCompactionInfo> &table_compaction_map);
};

// Mainly to verify checksum of cross-cluster's tablet which sync from primary cluster
class ObCrossClusterTabletChecksumValidator : public ObChecksumValidatorBase
{
public:
  ObCrossClusterTabletChecksumValidator();
  virtual ~ObCrossClusterTabletChecksumValidator() {}
  int check_and_set_validate(const bool is_primary_service,
                             const share::SCN &frozen_scn);
  void set_major_merge_start_time(const int64_t major_merge_start_us)
  {
    major_merge_start_us_ = major_merge_start_us;
  }
  uint64_t get_special_table_id() const { return special_table_id_; }
  // sync data from __all_tablet_replica_checksum to __all_tablet_checksum at table granularity
  int write_tablet_checksum_at_table_level(const volatile bool &stop,
                                           const ObArray<share::ObTabletLSPair> &pairs,
                                           const share::SCN &frozen_scn,
                                           const share::ObTableCompactionInfo &table_compaction_info,
                                           const uint64_t table_id,
                                           const int64_t expected_epoch);

private:
  virtual int check_all_table_verification_finished(const volatile bool &stop,
                                                    const share::SCN &frozen_scn,
                                                    const hash::ObHashMap<share::ObTabletLSPair, share::ObTabletCompactionStatus> &tablet_compaction_map,
                                                    int64_t &table_count,
                                                    hash::ObHashMap<uint64_t, share::ObTableCompactionInfo> &table_compaction_map,
                                                    ObMergeTimeStatistics &merge_time_statistics,
                                                    const int64_t expected_epoch) override;
  int check_need_validate(const bool is_primary_service,
                          const share::SCN &frozen_scn,
                          bool &need_validate) const;
  int check_cross_cluster_checksum(const share::schema::ObTableSchema &table_schema, const share::SCN &frozen_scn);
  void sort_tablet_ids(ObArray<ObTabletID> &tablet_ids);
  int check_column_checksum(const ObArray<share::ObTabletReplicaChecksumItem> &tablet_replica_checksum_items,
                            const ObArray<share::ObTabletChecksumItem> &tablet_checksum_items);
  bool is_first_tablet_in_sys_ls(const share::ObTabletReplicaChecksumItem &item) const;
  bool check_waiting_tablet_checksum_timeout() const;
  // handle the table, update its all tablets' status if needed. And update its compaction_info in @table_compaction_map
  int handle_table_verification_finished(const volatile bool &stop,
                                         const share::schema::ObTableSchema *table_schema,
                                         const share::SCN &frozen_scn,
                                         hash::ObHashMap<uint64_t, share::ObTableCompactionInfo> &table_compaction_map,
                                         ObMergeTimeStatistics &merge_time_statistics,
                                         const int64_t expected_epoch);
  int try_update_tablet_checksum_items(const volatile bool &stop,
                                       const ObArray<share::ObTabletLSPair> &pairs,
                                       const share::SCN &frozen_scn,
                                       const int64_t expected_epoch);
  int contains_first_tablet_in_sys_ls(const ObArray<share::ObTabletLSPair> &pairs,
                                      bool &is_containing) const;

private:
  const static int64_t MAX_BATCH_INSERT_COUNT = 100;
  // record the time when starting to major merge, used for check_waiting_tablet_checksum_timeout
  int64_t major_merge_start_us_;
  // record the table_id of the table which contains first tablet in sys ls
  uint64_t special_table_id_;
};

// Mainly to verify checksum between (global and local) index table and main table
class ObIndexChecksumValidator : public ObChecksumValidatorBase
{
public:
  ObIndexChecksumValidator() {}
  virtual ~ObIndexChecksumValidator() {}
  void check_and_set_validate(const bool is_primary_service);

private:
  // valid '<data table, index table>' pair should finish index column checksum verification, other tables just skip verification.
  virtual int check_all_table_verification_finished(const volatile bool &stop,
                                                    const share::SCN &frozen_scn,
                                                    const hash::ObHashMap<share::ObTabletLSPair, share::ObTabletCompactionStatus> &tablet_compaction_map,
                                                    int64_t &table_count,
                                                    hash::ObHashMap<uint64_t, share::ObTableCompactionInfo> &table_compaction_map,
                                                    ObMergeTimeStatistics &merge_time_statistics,
                                                    const int64_t expected_epoch) override;
  void check_need_validate(const bool is_primary_service, bool &need_validate) const;
  // handle data table which has tablet and index table(s). its all index tables may finish virification or not
  // If all finished, update table status.
  int update_data_table_verified(const uint64_t table_id,
                                 const share::ObTableCompactionInfo &data_table_compaction,
                                 const share::SCN &frozen_scn,
                                 hash::ObHashMap<uint64_t, share::ObTableCompactionInfo> &table_compaction_map);
  // handle the table, update its all tablets' status if needed. And update its compaction_info in @table_compaction_map
  int handle_table_verification_finished(const uint64_t table_id,
                                         const share::SCN &frozen_scn,
                                         hash::ObHashMap<uint64_t, share::ObTableCompactionInfo> &table_compaction_map);
  bool is_index_table(const share::schema::ObSimpleTableSchemaV2 &simple_schema);
  // handle data tables with index, mark data tables whose all index tables finished verification as INDEX_CKM_VERIFIED
  int handle_data_table_with_index(const volatile bool &stop,
                                   const share::SCN &frozen_scn,
                                   const common::ObIArray<uint64_t> &table_ids,
                                   const ObIArray<const share::schema::ObSimpleTableSchemaV2 *> &table_schemas,
                                   hash::ObHashMap<uint64_t, share::ObTableCompactionInfo> &table_compaction_map);
  // check data tables with index, return those need to be marked as INDEX_CKM_VERIFIED
  int check_data_table_with_index(const common::ObIArray<const share::schema::ObSimpleTableSchemaV2 *> &table_schemas,
                                  hash::ObHashMap<uint64_t, share::ObTableCompactionInfo> &table_compaction_map,
                                  common::ObIArray<uint64_t> &data_tables_to_update);
};

} // end namespace rootserver
} // end namespace oceanbase

#endif // OCEANBASE_ROOTSERVER_FREEZE_OB_CHECKSUM_VALIDATOR_H_
