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

#ifndef _OB_OPT_STAT_MANAGER_H_
#define _OB_OPT_STAT_MANAGER_H_

#include "lib/queue/ob_dedup_queue.h"
#include "share/stat/ob_opt_column_stat.h"
#include "share/stat/ob_opt_table_stat.h"
#include "share/stat/ob_opt_stat_service.h"
#include "share/stat/ob_opt_stat_sql_service.h"
#include "share/ob_rpc_struct.h"
#include "lib/queue/ob_dedup_queue.h"
#include "share/stat/ob_stat_define.h"

namespace oceanbase {
namespace common {
class ObOptColumnStatHandle;

class ObOptStatManager
{
public:
  ObOptStatManager();
  virtual ~ObOptStatManager() {}
  virtual int init(ObMySQLProxy *proxy,
                   ObServerConfig *config);

  static int64_t get_default_data_size();

  static int64_t get_default_avg_row_size();

  int check_has_opt_stat(const uint64_t tenant_id,
                         const uint64_t table_ref_id,
                         const ObIArray<int64_t> &part_ids,
                         const int64_t part_cnt,
                         bool &has_opt_stat);

  int check_stat_version(const uint64_t tenant_id,
                         const uint64_t tab_ref_id,
                         const int64_t part_id,
                         int64_t &last_analyzed);

  int update_table_stat(const uint64_t tenant_id,
                        const ObOptTableStat *table_stats,
                        const bool is_index_stat);

  int update_table_stat(const uint64_t tenant_id,
                        const ObIArray<ObOptTableStat*> &table_stats,
                        const bool is_index_stat);

  int get_column_stat(const uint64_t tenant_id,
                      const uint64_t tab_ref_id,
                      const int64_t part_id,
                      const uint64_t column_id,
                      int64_t *num_distinct = NULL,
                      int64_t *num_null = NULL,
                      int64_t *avg_length = NULL,
                      ObObj *min_obj = NULL,
                      ObObj *max_obj = NULL,
                      ObIAllocator *alloc = NULL);

  int get_column_stat(const uint64_t tenant_id,
                      const uint64_t tab_ref_id,
                      const ObIArray<int64_t> &part_ids,
                      const uint64_t column_id,
                      int64_t *num_distinct = NULL,
                      int64_t *num_null = NULL,
                      int64_t *avg_length = NULL,
                      ObObj *min_obj = NULL,
                      ObObj *max_obj = NULL,
                      ObIAllocator *alloc = NULL);

  int get_column_stat(const uint64_t tenant_id,
                      const uint64_t table_id,
                      const ObIArray<int64_t> &part_ids,
                      const ObIArray<uint64_t> &column_ids,
                      ObIArray<ObOptColumnStatHandle> &handles);

  int get_column_stat(const uint64_t tenant_id,
                      const uint64_t ref_id,
                      const int64_t part_id,
                      const uint64_t col_id,
                      ObOptColumnStatHandle &handle);

  int get_table_stat(const uint64_t tenant_id,
                     const uint64_t table_ref_id,
                     const int64_t part_id,
                     const int64_t part_cnt,
                     int64_t *row_count = NULL,
                     int64_t *avg_len = NULL,
                     int64_t *avg_part_size = NULL,
                     int64_t *macro_block_count = NULL,
                     int64_t *micro_block_count = NULL,
                     int64_t *last_analyzed = NULL);

  int get_table_stat(const uint64_t tenant_id,
                     const uint64_t tab_ref_id,
                     const ObIArray<int64_t> &part_ids,
                     const int64_t part_cnt,
                     int64_t *row_count = NULL,
                     int64_t *avg_len = NULL,
                     int64_t *avg_part_size = NULL,
                     int64_t *micro_block_count = NULL,
                     int64_t *last_analyzed = NULL);

  int get_table_stat(const uint64_t tenant_id,
                     const uint64_t table_id,
                     const ObIArray<int64_t> &part_ids,
                     ObIArray<ObOptTableStat> &tstats);

  int get_table_stat(const uint64_t tenant_id,
                     const uint64_t table_id,
                     const ObIArray<int64_t> &part_ids,
                     ObIArray<ObOptTableStatHandle> &handles);

  /**
   *  @brief  外部获取列统计信息的接口，以引用的形式返回一个包含统计信息对象指针的handle，通过这个指针
   *          可以获取统计信息。这样的方式是由ObKVCache的底层实现决定的。如果返回的handle的指针非空，
   *          那么handle对象保证在自身析构前其统计信息指针总是有效的。
   */
  virtual int get_column_stat(const uint64_t tenant_id,
                              const ObOptColumnStat::Key &key,
                              ObOptColumnStatHandle &handle);
  virtual int update_column_stat(share::schema::ObSchemaGetterGuard *schema_guard,
                                 const uint64_t tenant_id,
                                 const common::ObIArray<ObOptColumnStat *> &column_stats,
                                 bool only_update_col_stat = false);

  int delete_table_stat(const uint64_t tenant_id,
                        const uint64_t ref_id,
                        int64_t &affected_rows);

  int delete_table_stat(uint64_t tenant_id,
                        const uint64_t ref_id,
                        const ObIArray<int64_t> &part_ids,
                        const bool cascade_column,
                        int64_t &affected_rows);

  int delete_column_stat(const uint64_t tenant_id,
                         const uint64_t ref_id,
                         const ObIArray<uint64_t> &column_ids,
                         const ObIArray<int64_t> &part_ids,
                         const bool only_histogram = false);

  int erase_column_stat(const ObOptColumnStat::Key &key);
  int erase_table_stat(const ObOptTableStat::Key &key);

  int erase_table_stat(const uint64_t tenant_id,
                       const uint64_t table_id,
                       const ObIArray<int64_t> &part_ids);
  int erase_column_stat(const uint64_t tenant_id,
                        const uint64_t table_id,
                        const ObIArray<int64_t> &part_ids,
                        const ObIArray<uint64_t> &column_ids);

  int batch_write(share::schema::ObSchemaGetterGuard *schema_guard,
                  const uint64_t tenant_id,
                  ObIArray<ObOptTableStat *> &table_stats,
                  ObIArray<ObOptColumnStat *> &column_stats,
                  const int64_t current_time,
                  const bool is_index_stat,
                  const bool is_history_stat);

  /**  @brief  外部获取行统计信息的接口 */
  virtual int get_table_stat(const uint64_t tenant_id,
                             const ObOptTableStat::Key &key,
                             ObOptTableStat &tstat);
  virtual int add_refresh_stat_task(const obrpc::ObUpdateStatCacheArg &analyze_arg);

  int invalidate_plan(const uint64_t tenant_id, const uint64_t table_id);

  int handle_refresh_stat_task(const obrpc::ObUpdateStatCacheArg &arg);

  static ObOptStatManager &get_instance()
  {
    static ObOptStatManager instance_;
    return instance_;
  }
  bool is_inited() const { return inited_; }
protected:
  static const int64_t REFRESH_STAT_TASK_NUM = 5;
  bool inited_;
  common::ObDedupQueue refresh_stat_task_queue_;
  ObOptStatService stat_service_;
  int64_t last_schema_version_;
};

template <typename T>
inline void assign_value(const T &val, T *ptr)
{
  if (NULL != ptr) {
    *ptr = val;
  }
}

}
}

#endif /* _OB_OPT_STAT_MANAGER_H_ */
