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

#define USING_LOG_PREFIX SQL_OPT
#include "ob_stat_define.h"

namespace oceanbase
{
namespace common
{
const int64_t ObColumnStatParam::DEFAULT_HISTOGRAM_BUCKET_NUM = 254;

void ObAnalyzeSampleInfo::set_percent(double percent)
{
  is_sample_ = true;
  sample_type_ = PercentSample;
  sample_value_ = percent;
}

void ObAnalyzeSampleInfo::set_rows(double row_num)
{
  is_sample_ = true;
  sample_type_ = RowSample;
  sample_value_ = row_num;
}

bool ObColumnStatParam::is_valid_histogram_type(const ObObjType type)
{
  bool ret = false;
  // currently, we only support the following type to collect histogram
  ColumnTypeClass type_class = ob_obj_type_class(type);
  if (type_class == ColumnTypeClass::ObIntTC ||
      type_class == ColumnTypeClass::ObUIntTC ||
      type_class == ColumnTypeClass::ObFloatTC ||
      type_class == ColumnTypeClass::ObDoubleTC ||
      type_class == ColumnTypeClass::ObNumberTC ||
      type_class == ColumnTypeClass::ObDateTimeTC ||
      type_class == ColumnTypeClass::ObDateTC ||
      type_class == ColumnTypeClass::ObTimeTC ||
      type_class == ColumnTypeClass::ObYearTC ||
      type_class == ColumnTypeClass::ObStringTC ||
      type_class == ColumnTypeClass::ObRawTC ||
      type_class == ColumnTypeClass::ObOTimestampTC ||
      type_class == ColumnTypeClass::ObBitTC ||
      type_class == ColumnTypeClass::ObEnumSetTC ||
      (lib::is_mysql_mode() && type == ObTinyTextType)) {
    ret = true;
  }
  return ret;
}

bool StatTable::operator<(const StatTable &other) const
{
  return stale_percent_ < other.stale_percent_;
}

int StatTable::assign(const StatTable &other)
{
  int ret = OB_SUCCESS;
  database_id_ = other.database_id_;
  table_id_ = other.table_id_;
  incremental_stat_ = other.incremental_stat_;
  stale_percent_ = other.stale_percent_;
  need_gather_subpart_ = other.need_gather_subpart_;
  return no_regather_partition_ids_.assign(other.no_regather_partition_ids_);
}

int ObTableStatParam::assign(const ObTableStatParam &other)
{
  int ret = OB_SUCCESS;
  tenant_id_ = other.tenant_id_;
  db_name_ = other.db_name_;
  db_id_ = other.db_id_;
  tab_name_ = other.tab_name_;
  table_id_ = other.table_id_;
  part_level_ = other.part_level_;
  total_part_cnt_ = other.total_part_cnt_;
  part_name_ = other.part_name_;
  sample_info_.is_sample_ = other.sample_info_.is_sample_;
  sample_info_.is_block_sample_ = other.sample_info_.is_block_sample_;
  sample_info_.sample_type_ = other.sample_info_.sample_type_;
  sample_info_.sample_value_ = other.sample_info_.sample_value_;
  method_opt_ = other.method_opt_;
  degree_ = other.degree_;
  need_global_ = other.need_global_;
  need_approx_global_ = other.need_approx_global_;
  need_part_ = other.need_part_;
  need_subpart_ = other.need_subpart_;
  granularity_ = other.granularity_;
  cascade_ = other.cascade_;
  stat_tab_ = other.stat_tab_;
  stat_id_ = other.stat_id_;
  stat_own_ = other.stat_own_;
  no_invalidate_ = other.no_invalidate_;
  force_ = other.force_;
  is_subpart_name_ = other.is_subpart_name_;
  stat_category_ = other.stat_category_;
  tab_group_ = other.tab_group_;
  stattype_ = other.stattype_;
  need_approx_ndv_ = other.need_approx_ndv_;
  is_index_stat_ = other.is_index_stat_;
  data_table_name_ = other.data_table_name_;
  is_global_index_ = other.is_global_index_;
  global_part_id_ = other.global_part_id_;
  duration_time_ = other.duration_time_;
  global_tablet_id_ = other.global_tablet_id_;
  global_data_part_id_ = other.global_data_part_id_;
  data_table_id_ = other.data_table_id_;
  need_estimate_block_ = other.need_estimate_block_;
  if (OB_FAIL(part_infos_.assign(other.part_infos_))) {
    LOG_WARN("failed to assign", K(ret));
  } else if (OB_FAIL(subpart_infos_.assign(other.subpart_infos_))) {
    LOG_WARN("failed to assign", K(ret));
  } else if (OB_FAIL(approx_part_infos_.assign(other.approx_part_infos_))) {
    LOG_WARN("failed to assign", K(ret));
  } else if (OB_FAIL(column_params_.assign(other.column_params_))) {
    LOG_WARN("failed to assign", K(ret));
  } else if (OB_FAIL(part_ids_.assign(other.part_ids_))) {
    LOG_WARN("failed to assign", K(ret));
  } else if (OB_FAIL(subpart_ids_.assign(other.subpart_ids_))) {
    LOG_WARN("failed to assign", K(ret));
  } else if (OB_FAIL(no_regather_partition_ids_.assign(other.no_regather_partition_ids_))) {
    LOG_WARN("failed to assign", K(ret));
  } else if (OB_FAIL(all_part_infos_.assign(other.all_part_infos_))) {
    LOG_WARN("failed to assign", K(ret));
  } else if (OB_FAIL(all_subpart_infos_.assign(other.all_subpart_infos_))) {
    LOG_WARN("failed to assign", K(ret));
  } else {/*do nothing*/}
  return ret;
}

int ObTableStatParam::assign_common_property(const ObTableStatParam &other)
{
  int ret = OB_SUCCESS;
  sample_info_.is_sample_ = other.sample_info_.is_sample_;
  sample_info_.is_block_sample_ = other.sample_info_.is_block_sample_;
  sample_info_.sample_type_ = other.sample_info_.sample_type_;
  sample_info_.sample_value_ = other.sample_info_.sample_value_;
  method_opt_ = other.method_opt_;
  degree_ = other.degree_;
  need_global_ = other.need_global_;
  need_approx_global_ = other.need_approx_global_;
  need_part_ = other.need_part_;
  need_subpart_ = other.need_subpart_;
  granularity_ = other.granularity_;
  cascade_ = other.cascade_;
  stat_tab_ = other.stat_tab_;
  stat_id_ = other.stat_id_;
  stat_own_ = other.stat_own_;
  no_invalidate_ = other.no_invalidate_;
  force_ = other.force_;
  stat_category_ = other.stat_category_;
  stattype_ = other.stattype_;
  need_approx_ndv_ = other.need_approx_ndv_;
  duration_time_ = other.duration_time_;
  return ret;
}

}
}
