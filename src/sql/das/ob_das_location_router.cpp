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

#define USING_LOG_PREFIX SQL_DAS
#include "sql/das/ob_das_location_router.h"
#include "sql/das/ob_das_define.h"
#include "share/ob_ls_id.h"
#include "observer/ob_server_struct.h"
#include "share/location_cache/ob_location_service.h"
#include "share/schema/ob_part_mgr_util.h"
#include "share/schema/ob_multi_version_schema_service.h"
#include "share/schema/ob_schema_utils.h"
#include "sql/das/ob_das_utils.h"

namespace oceanbase
{
using namespace common;
using namespace share;
using namespace share::schema;
namespace sql
{
OB_SERIALIZE_MEMBER(DASRelatedTabletMap::MapEntry,
                    src_tablet_id_,
                    related_table_id_,
                    related_tablet_id_,
                    related_part_id_);

int VirtualSvrPair::init(ObIAllocator &allocator,
                         ObTableID vt_id,
                         ObIArray<ObAddr> &part_locations)
{
  int ret = OB_SUCCESS;
  all_server_.set_allocator(&allocator);
  all_server_.set_capacity(part_locations.count());
  table_id_ = vt_id;
  if (OB_FAIL(all_server_.assign(part_locations))) {
    LOG_WARN("store addr to all server list failed", K(ret));
  }
  return ret;
}

int VirtualSvrPair::get_server_by_tablet_id(const ObTabletID &tablet_id, ObAddr &addr) const
{
  int ret = OB_SUCCESS;
  //tablet id must start with 1, so the server addr index is (tablet_id - 1) in virtual table
  int64_t idx = tablet_id.id() - 1;
  if (VirtualSvrPair::EMPTY_VIRTUAL_TABLE_TABLET_ID == tablet_id.id()) {
    addr = GCTX.self_addr();
  } else if (OB_UNLIKELY(!tablet_id.is_valid())
      || OB_UNLIKELY(idx < 0)
      || OB_UNLIKELY(idx >= all_server_.count())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("tablet_id is invalid", K(ret), K(tablet_id), K(all_server_));
  } else {
    addr = all_server_.at(idx);
  }
  return ret;
}

int VirtualSvrPair::get_all_part_and_tablet_id(ObIArray<ObObjectID> &part_ids,
                                               ObIArray<ObTabletID> &tablet_ids) const
{
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < all_server_.count(); ++i) {
    //tablet id must start with 1,
    //so use the all server index + 1 as the tablet id and partition_id in virtual table
    if (OB_FAIL(part_ids.push_back(i + 1))) {
      LOG_WARN("mock part id failed", K(ret), K(i));
    } else if (OB_FAIL(tablet_ids.push_back(ObTabletID(i + 1)))) {
      LOG_WARN("mock tablet id failed", K(ret), K(i));
    }
  }
  return ret;
}

int VirtualSvrPair::get_part_and_tablet_id_by_server(const ObAddr &addr,
                                                     ObObjectID &part_id,
                                                     ObTabletID &tablet_id) const
{
  int ret = OB_SUCCESS;
  for (int64_t i = 0; i < all_server_.count(); ++i) {
    if (addr == all_server_.at(i)) {
      //tablet id must start with 1,
      //so use the all server index + 1 as the tablet id and partition_id in virtual table
      part_id = i + 1;
      tablet_id = i + 1;
      break;
    }
  }
  if (!tablet_id.is_valid()) {
    LOG_DEBUG("virtual table partition not exists", K(ret), K(addr));
  }
  return ret;
}

void VirtualSvrPair::get_default_tablet_and_part_id(ObTabletID &tablet_id, ObObjectID &part_id) const
{
  //tablet id must start with 1, （0 is invalid tablet id）
  part_id = VirtualSvrPair::EMPTY_VIRTUAL_TABLE_TABLET_ID;
  tablet_id = VirtualSvrPair::EMPTY_VIRTUAL_TABLE_TABLET_ID;
}

int DASRelatedTabletMap::add_related_tablet_id(ObTabletID src_tablet_id,
                                               ObTableID related_table_id,
                                               ObTabletID related_tablet_id,
                                               ObObjectID related_part_id)
{
  MapEntry map_entry;
  map_entry.src_tablet_id_ = src_tablet_id;
  map_entry.related_table_id_ = related_table_id;
  map_entry.related_tablet_id_ = related_tablet_id;
  map_entry.related_part_id_ = related_part_id;
  return list_.push_back(map_entry);
}

int DASRelatedTabletMap::get_related_tablet_id(ObTabletID src_tablet_id,
                                               ObTableID related_table_id,
                                               Value &val)
{
  int ret = OB_SUCCESS;
  MapEntry *final_entry = nullptr;
  FOREACH_X(node, list_, final_entry == nullptr) {
    MapEntry &entry = *node;
    if (entry.src_tablet_id_ == src_tablet_id && entry.related_table_id_ == related_table_id) {
      final_entry = &entry;
    }
  }
  if (OB_LIKELY(final_entry != nullptr)) {
    val.first = final_entry->related_tablet_id_;
    val.second = final_entry->related_part_id_;
  } else {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("get related tablet id failed", K(ret), K(src_tablet_id), K(related_table_id), K(list_));
  }
  return ret;
}

int ObDASTabletMapper::get_tablet_and_object_id(
    const ObPartitionLevel part_level,
    const ObPartID part_id,
    const ObNewRange &range,
    ObIArray<ObTabletID> &tablet_ids,
    ObIArray<ObObjectID> &object_ids)
{
  int ret = OB_SUCCESS;
  ObSEArray<ObTabletID, 4> tmp_tablet_ids;
  ObSEArray<ObObjectID, 4> tmp_part_ids;
  if (OB_NOT_NULL(table_schema_)) {
    share::schema::RelatedTableInfo *related_info_ptr = nullptr;
    if (related_info_.related_tids_ != nullptr && !related_info_.related_tids_->empty()) {
      related_info_ptr = &related_info_;
    }
    if (OB_FAIL(ret)) {
    } else if (PARTITION_LEVEL_ZERO == part_level) {
      ObTabletID tablet_id;
      ObObjectID object_id;
      if (OB_FAIL(ObPartitionUtils::get_tablet_and_object_id(
          *table_schema_, tablet_id, object_id, related_info_ptr))) {
        LOG_WARN("fail to get tablet_id and object_id", KR(ret), KPC_(table_schema));
      } else if (OB_FAIL(tmp_tablet_ids.push_back(tablet_id))) {
        LOG_WARN("fail to push back tablet_id", KR(ret), K(tablet_id));
      } else if (OB_FAIL(tmp_part_ids.push_back(object_id))) {
        LOG_WARN("fail to push back object_id", KR(ret), K(object_id));
      }
    } else if (PARTITION_LEVEL_ONE == part_level) {
      if (OB_FAIL(ObPartitionUtils::get_tablet_and_part_id(
          *table_schema_, range, tmp_tablet_ids, tmp_part_ids, related_info_ptr))) {
        LOG_WARN("fail to get tablet_id and part_id", KR(ret), K(range), KPC_(table_schema));
      }
    } else if (PARTITION_LEVEL_TWO == part_level) {
      if (OB_FAIL(ObPartitionUtils::get_tablet_and_subpart_id(
          *table_schema_, part_id, range, tmp_tablet_ids, tmp_part_ids, related_info_ptr))) {
        LOG_WARN("fail to get tablet_id and part_id", KR(ret), K(part_id), K(range), KPC_(table_schema));
      }
    } else {
      ret = OB_INVALID_ARGUMENT;
      LOG_WARN("invalid part level", KR(ret), K(part_level));
    }
    OZ(append_array_no_dup(tablet_ids, tmp_tablet_ids));
    OZ(append_array_no_dup(object_ids, tmp_part_ids));
  } else {
    if (part_level == PARTITION_LEVEL_TWO) {
      ret = OB_NOT_SUPPORTED;
      LOG_WARN("virtual table with subpartition table not supported", KR(ret), KPC(vt_svr_pair_));
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "virtual table with subpartition table");
    } else if (!range.is_whole_range()) {
      ret = OB_NOT_SUPPORTED;
      LOG_WARN("virtual table get tablet_id only with whole range is supported", KR(ret), KPC(vt_svr_pair_));
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "virtual table get tablet_id with precise range info");
    } else if (OB_FAIL(vt_svr_pair_->get_all_part_and_tablet_id(object_ids, tablet_ids))) {
      LOG_WARN("get all part and tablet id failed", K(ret));
    } else if (OB_FAIL(mock_vtable_related_tablet_id_map(tablet_ids, object_ids))) {
      LOG_WARN("fail to mock vtable related tablet id map", KR(ret), K(tablet_ids), K(object_ids));
    }
  }
  return ret;
}

int ObDASTabletMapper::get_tablet_and_object_id(const ObPartitionLevel part_level,
                                                const ObPartID part_id,
                                                const ObNewRow &row,
                                                ObTabletID &tablet_id,
                                                ObObjectID &object_id)
{
  int ret = OB_SUCCESS;
  tablet_id = ObTabletID::INVALID_TABLET_ID;
  if (OB_NOT_NULL(table_schema_)) {
    share::schema::RelatedTableInfo *related_info_ptr = nullptr;
    if (related_info_.related_tids_ != nullptr && !related_info_.related_tids_->empty()) {
      related_info_ptr = &related_info_;
    }
    if (OB_FAIL(ret)) {
    } else if (PARTITION_LEVEL_ZERO == part_level) {
      if (OB_FAIL(ObPartitionUtils::get_tablet_and_object_id(
          *table_schema_, tablet_id, object_id, related_info_ptr))) {
        LOG_WARN("fail to get tablet_id and object_id", KR(ret), KPC_(table_schema));
      }
    } else if (PARTITION_LEVEL_ONE == part_level) {
      if (OB_FAIL(ObPartitionUtils::get_tablet_and_part_id(
          *table_schema_, row, tablet_id, object_id, related_info_ptr))) {
        LOG_WARN("fail to get tablet_id and part_id", KR(ret), K(row), KPC_(table_schema));
      }
    } else if (PARTITION_LEVEL_TWO == part_level) {
      if (OB_FAIL(ObPartitionUtils::get_tablet_and_subpart_id(
          *table_schema_, part_id, row, tablet_id, object_id, related_info_ptr))) {
        LOG_WARN("fail to get tablet_id and part_id", KR(ret), K(part_id), K(row), KPC_(table_schema));
      }
    } else {
      ret = OB_INVALID_ARGUMENT;
      LOG_WARN("invalid part level", KR(ret), K(part_level));
    }
  } else {
    //virtual table, only supported partition by list(svr_ip, svr_port) ...
    ObAddr svr_addr;
    if (part_level == PARTITION_LEVEL_TWO) {
      ret = OB_NOT_SUPPORTED;
      LOG_WARN("virtual table with subpartition table not supported", KR(ret), KPC(vt_svr_pair_));
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "virtual table with subpartition table");
    } else if (row.get_count() != 2) {
      ret = OB_NOT_SUPPORTED;
      LOG_WARN("virtual table, only supported partition by list(svr_ip, svr_port)", KR(ret), K(row));
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "virtual table partition by other than list(svr_ip, svr_port)");
    } else {
      const ObObj &svr_ip = row.get_cell(0);
      const ObObj &port_obj = row.get_cell(1);
      int64_t port_int = 0;
      if (is_oracle_mode()) {
        OZ(port_obj.get_number().extract_valid_int64_with_trunc(port_int));
      } else {
        port_int = port_obj.get_int();
      }
      svr_addr.set_ip_addr(svr_ip.get_string(), port_int);
    }
    if (OB_SUCC(ret) && OB_FAIL(vt_svr_pair_->get_part_and_tablet_id_by_server(svr_addr, object_id, tablet_id))) {
      LOG_WARN("get part and tablet id by server failed", K(ret));
    } else if (OB_FAIL(mock_vtable_related_tablet_id_map(tablet_id, object_id))) {
      LOG_WARN("fail to mock vtable related tablet id map", KR(ret), K(tablet_id), K(object_id));
    }
  }
  return ret;
}

int ObDASTabletMapper::mock_vtable_related_tablet_id_map(
    const ObIArray<ObTabletID> &tablet_ids,
    const ObIArray<ObObjectID> &part_ids)
{
  int ret = OB_SUCCESS;
  if (!tablet_ids.empty() && related_info_.related_tids_ != nullptr && !related_info_.related_tids_->empty()) {
    for (int64_t i = 0; OB_SUCC(ret) && i < tablet_ids.count(); ++i) {
      const ObTabletID &src_tablet_id = tablet_ids.at(i);
      const ObObjectID &src_part_id = part_ids.at(i);
      if (OB_FAIL(mock_vtable_related_tablet_id_map(src_tablet_id, src_part_id))) {
        LOG_WARN("mock related tablet id map failed", KR(ret), K(src_tablet_id), K(src_part_id));
      }
    }
  }
  return ret;
}

int ObDASTabletMapper::mock_vtable_related_tablet_id_map(
    const ObTabletID &tablet_id,
    const ObObjectID &part_id)
{
  int ret = OB_SUCCESS;
  if (tablet_id.is_valid() && related_info_.related_tids_ != nullptr && !related_info_.related_tids_->empty()) {
    for (int64_t i = 0; OB_SUCC(ret) && i < related_info_.related_tids_->count(); ++i) {
      ObTableID related_table_id = related_info_.related_tids_->at(i);
      ObTabletID related_tablet_id = tablet_id;
      ObObjectID related_object_id = part_id;
      if (OB_FAIL(related_info_.related_map_->add_related_tablet_id(tablet_id,
                                                                    related_table_id,
                                                                    related_tablet_id,
                                                                    related_object_id))) {
        LOG_WARN("add related tablet id to map failed", KR(ret), K(tablet_id),
                 K(related_table_id), K(related_tablet_id), K(related_object_id));
      } else {
        LOG_DEBUG("mock related tablet id map",
                  K(tablet_id), K(part_id),
                  K(related_table_id), K(related_tablet_id), K(related_object_id));
      }
    }
  }
  return ret;
}

int ObDASTabletMapper::get_non_partition_tablet_id(ObIArray<ObTabletID> &tablet_ids,
                                                   ObIArray<ObObjectID> &out_part_ids)
{
  int ret = OB_SUCCESS;
  ObNewRange range;
  // here need whole range, for virtual table calc tablet and object id
  range.set_whole_range();
  OZ(get_tablet_and_object_id(PARTITION_LEVEL_ZERO, OB_INVALID_ID,
                              range, tablet_ids, out_part_ids));

  return ret;
}

int ObDASTabletMapper::get_tablet_and_object_id(const ObPartitionLevel part_level,
                                                const ObPartID part_id,
                                                const ObIArray<ObNewRange*> &ranges,
                                                ObIArray<ObTabletID> &tablet_ids,
                                                ObIArray<ObObjectID> &out_part_ids)
{
  int ret = OB_SUCCESS;
  ObSEArray<ObTabletID, 4> tmp_tablet_ids;
  ObSEArray<ObObjectID, 4> tmp_part_ids;
  for (int64_t i = 0; OB_SUCC(ret) && i < ranges.count(); i++) {
    tmp_tablet_ids.reset();
    tmp_part_ids.reset();
    OZ(get_tablet_and_object_id(part_level, part_id, *ranges.at(i), tmp_tablet_ids, tmp_part_ids));
    OZ(append_array_no_dup(tablet_ids, tmp_tablet_ids));
    OZ(append_array_no_dup(out_part_ids, tmp_part_ids));
  }

  return ret;
}

int ObDASTabletMapper::get_tablet_and_object_id(const ObPartitionLevel part_level,
                                                const ObPartID part_id,
                                                const ObObj &value,
                                                ObIArray<ObTabletID> &tablet_ids,
                                                ObIArray<ObObjectID> &out_part_ids)
{
  int ret = OB_SUCCESS;
  uint64_t table_id = NULL == table_schema_ ? vt_svr_pair_->get_table_id()
                                            : table_schema_->get_table_id();
  ObRowkey rowkey(const_cast<ObObj*>(&value), 1);
  ObNewRange range;
  ObSEArray<ObTabletID, 4> tmp_tablet_ids;
  ObSEArray<ObObjectID, 4> tmp_part_ids;
  if (OB_FAIL(range.build_range(table_id, rowkey))) {
    LOG_WARN("failed to build range", K(ret));
  } else if (OB_FAIL(get_tablet_and_object_id(part_level, part_id, range, tmp_tablet_ids, tmp_part_ids))) {
    LOG_WARN("fail to get tablet id", K(part_level), K(part_id), K(range), K(ret));
  } else {
    OZ(append_array_no_dup(tablet_ids, tmp_tablet_ids));
    OZ(append_array_no_dup(out_part_ids, tmp_part_ids));
  }

  return ret;
}

int ObDASTabletMapper::get_all_tablet_and_object_id(const ObPartitionLevel part_level,
                                                    const ObPartID part_id,
                                                    ObIArray<ObTabletID> &tablet_ids,
                                                    ObIArray<ObObjectID> &out_part_ids)
{
  int ret = OB_SUCCESS;
  uint64_t table_id = NULL == table_schema_ ? vt_svr_pair_->get_table_id()
                                            : table_schema_->get_table_id();
  ObNewRange whole_range;
  whole_range.set_whole_range();
  whole_range.table_id_ = table_id;
  ObSEArray<ObTabletID, 4> tmp_tablet_ids;
  ObSEArray<ObObjectID, 4> tmp_part_ids;
  OZ (get_tablet_and_object_id(part_level, part_id, whole_range, tmp_tablet_ids, tmp_part_ids));
  OZ(append_array_no_dup(tablet_ids, tmp_tablet_ids));
  OZ(append_array_no_dup(out_part_ids, tmp_part_ids));

  return ret;
}

int ObDASTabletMapper::get_all_tablet_and_object_id(ObIArray<ObTabletID> &tablet_ids,
                                                    ObIArray<ObObjectID> &out_part_ids)
{
  int ret = OB_SUCCESS;
  if (OB_NOT_NULL(table_schema_)) {
    if (!table_schema_->is_partitioned_table()) {
      if (OB_FAIL(get_non_partition_tablet_id(tablet_ids, out_part_ids))) {
        LOG_WARN("get non partitino tablet id failed", K(ret));
      }
    } else if (PARTITION_LEVEL_ONE == table_schema_->get_part_level()) {
      if (OB_FAIL(get_all_tablet_and_object_id(PARTITION_LEVEL_ONE, OB_INVALID_ID,
                                             tablet_ids, out_part_ids))) {
        LOG_WARN("fail to get tablet ids", K(ret));
      }
    } else {
      ObArray<ObTabletID> tmp_tablet_ids;
      ObArray<ObObjectID> tmp_part_ids;
      if (OB_FAIL(get_all_tablet_and_object_id(PARTITION_LEVEL_ONE, OB_INVALID_ID,
                                               tmp_tablet_ids, tmp_part_ids))) {
        LOG_WARN("Failed to get all part ids", K(ret));
      }
      for (int64_t idx = 0; OB_SUCC(ret) && idx < tmp_part_ids.count(); ++idx) {
        ObObjectID part_id = tmp_part_ids.at(idx);
        if (OB_FAIL(get_all_tablet_and_object_id(PARTITION_LEVEL_TWO, part_id,
                                                 tablet_ids, out_part_ids))) {
          LOG_WARN("fail to get tablet ids", K(ret));
        }
      }
    }
  }
  return ret;
}

//If the part_id calculated by the partition filter in the where clause is empty,
//we will use the default part id in this query as the final part_id,
//because optimizer needs at least one part_id to generate a plan
int ObDASTabletMapper::get_default_tablet_and_object_id(const ObIArray<ObObjectID> &part_hint_ids,
                                                        ObTabletID &tablet_id,
                                                        ObObjectID &object_id)
{
  int ret = OB_SUCCESS;
  if (OB_LIKELY(nullptr == vt_svr_pair_)) {
    ObCheckPartitionMode check_partition_mode = CHECK_PARTITION_MODE_NORMAL;
    ObPartitionSchemaIter iter(*table_schema_, check_partition_mode);
    ObPartitionSchemaIter::Info info;
    while (OB_SUCC(ret) && !tablet_id.is_valid()) {
      if (OB_FAIL(iter.next_partition_info(info))) {
        if (OB_ITER_END != ret) {
          LOG_WARN("switch the src partition info failed", K(ret));
        }
      } else if (part_hint_ids.empty()) {
        //if partition hint is empty,
        //we use the first partition in table schema as the default partition
        object_id = info.object_id_;
        tablet_id = info.tablet_id_;
      } else if (info.object_id_ == part_hint_ids.at(0)) {
        //if partition hint is specified, we must use the first part id in part_hint_ids_
        //as the default partition,
        //and can't use the first part id in table schema,
        //otherwise, the result of some cases will be incorrect,
        //such as:
        //create table t1(a int primary key, b int) partition by hash(a) partitions 2;
        //select * from t1 partition(p1) where a=0;
        //if where a=0 prune result is partition_id=0 and first_part_id in table schema is 0
        //but query specify that use partition_id=1 to access table, so the result is empty
        //if we use the first part id in table schema as the default partition to access table
        //the result of this query will not be empty
        object_id = info.object_id_;
        tablet_id = info.tablet_id_;
      }
      //calculate related partition id and tablet id
      if (OB_SUCC(ret) && tablet_id.is_valid() &&
          related_info_.related_tids_ != nullptr &&
          !related_info_.related_tids_->empty()) {
        ObSchemaGetterGuard guard;
        const uint64_t tenant_id=  table_schema_->get_tenant_id();
        if (OB_ISNULL(GCTX.schema_service_)) {
          ret = OB_INVALID_ARGUMENT;
          LOG_ERROR("invalid schema service", KR(ret));
        } else if (OB_FAIL(GCTX.schema_service_->get_tenant_schema_guard(table_schema_->get_tenant_id(), guard))) {
          LOG_WARN("get tenant schema guard fail", KR(ret), K(tenant_id));
        }
        for (int64_t i = 0; OB_SUCC(ret) && i < related_info_.related_tids_->count(); ++i) {
          ObTableID related_table_id = related_info_.related_tids_->at(i);
          const ObSimpleTableSchemaV2 *table_schema = nullptr;
          ObObjectID related_part_id = OB_INVALID_ID;
          ObTabletID related_tablet_id;
          if (OB_FAIL(guard.get_simple_table_schema(tenant_id, related_table_id, table_schema))) {
            LOG_WARN("get_table_schema fail", K(ret), K(tenant_id), K(related_table_id));
          } else if (OB_ISNULL(table_schema)) {
            ret = OB_SCHEMA_EAGAIN;
            LOG_WARN("fail to get table schema", KR(ret), K(related_table_id));
          } else if (OB_FAIL(table_schema->get_part_id_and_tablet_id_by_idx(info.part_idx_,
                                                                            info.subpart_idx_,
                                                                            related_part_id,
                                                                            related_tablet_id))) {
            LOG_WARN("get part by idx failed", K(ret), K(info), K(related_table_id));
          } else if (OB_FAIL(related_info_.related_map_->add_related_tablet_id(tablet_id,
                                                                               related_table_id,
                                                                               related_tablet_id,
                                                                               related_part_id))) {
            LOG_WARN("add related tablet id failed", K(ret),
                     K(tablet_id), K(related_table_id), K(related_part_id), K(related_tablet_id));
          } else {
            LOG_DEBUG("add related tablet id to map",
                      K(tablet_id), K(related_table_id), K(related_part_id), K(related_tablet_id));
          }
        }
      }
    }
    if (OB_ITER_END == ret) {
      ret = OB_SUCCESS;
    }
  } else if (!part_hint_ids.empty()) {
    ret = OB_NOT_SUPPORTED;
    LOG_WARN("specify partition name in virtual table not supported", K(ret));
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "specify partition name in virtual table");
  } else {
    //virtual table start partition id and tablet id with id=1
    vt_svr_pair_->get_default_tablet_and_part_id(tablet_id, object_id);
    if (related_info_.related_tids_ != nullptr && !related_info_.related_tids_->empty()) {
      for (int64_t i = 0; OB_SUCC(ret) && i < related_info_.related_tids_->count(); ++i) {
        ObTableID related_table_id = related_info_.related_tids_->at(i);
        //all related tables have the same part_id and tablet_id
        if (OB_FAIL(related_info_.related_map_->add_related_tablet_id(tablet_id, related_table_id, tablet_id, object_id))) {
          LOG_WARN("add related tablet id failed", K(ret), K(related_table_id), K(object_id));
        }
      }
    }
  }
  if (OB_SUCC(ret) && !tablet_id.is_valid()) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("invalid first tablet id", K(ret), KPC(table_schema_), KPC(vt_svr_pair_));
  }
  return ret;
}

//get the local index partition id by data table partition id
//or get the local index partition id by other local index partition id
//or get the data table partition id by its local index partition id
int ObDASTabletMapper::get_related_partition_id(const ObTableID &src_table_id,
                                                const ObObjectID &src_part_id,
                                                const ObTableID &dst_table_id,
                                                ObObjectID &dst_object_id)
{
  int ret = OB_SUCCESS;
  if (src_table_id == dst_table_id || nullptr != vt_svr_pair_) {
    dst_object_id = src_part_id;
  } else {
    bool is_found = false;
    ObCheckPartitionMode check_partition_mode = CHECK_PARTITION_MODE_NORMAL;
    ObPartitionSchemaIter iter(*table_schema_, check_partition_mode);
    ObPartitionSchemaIter::Info info;
    while (OB_SUCC(ret) && !is_found) {
      if (OB_FAIL(iter.next_partition_info(info))) {
        if (OB_ITER_END != ret) {
          LOG_WARN("switch the src partition info failed", K(ret));
        }
      } else if (info.object_id_ == src_part_id) {
        //find the partition array offset by search partition id
        is_found = true;
      }
    }
    if (OB_ITER_END == ret) {
      ret = OB_SUCCESS;
    }
    if (OB_SUCC(ret) && is_found) {
      ObSchemaGetterGuard guard;
      const ObSimpleTableSchemaV2 *dst_table_schema = nullptr;
      ObObjectID related_part_id = OB_INVALID_ID;
      ObTabletID related_tablet_id;
      if (OB_ISNULL(GCTX.schema_service_)) {
        ret = OB_INVALID_ARGUMENT;
        LOG_ERROR("invalid schema service", KR(ret));
      } else if (OB_FAIL(GCTX.schema_service_->get_tenant_schema_guard(table_schema_->get_tenant_id(), guard))) {
        LOG_WARN("get tenant schema guard fail", KR(ret), K(table_schema_->get_tenant_id()));
      } else if (OB_FAIL(guard.get_simple_table_schema(table_schema_->get_tenant_id(), dst_table_id, dst_table_schema))) {
        LOG_WARN("get_table_schema fail", K(ret), K(dst_table_id));
      } else if (OB_ISNULL(dst_table_schema)) {
        ret = OB_SCHEMA_EAGAIN;
        LOG_WARN("fail to get table schema", KR(ret), K(dst_table_id));
      } else if (OB_FAIL(dst_table_schema->get_part_id_and_tablet_id_by_idx(info.part_idx_,
                                                                            info.subpart_idx_,
                                                                            related_part_id,
                                                                            related_tablet_id))) {
        LOG_WARN("get part by idx failed", K(ret), K(info), K(dst_table_id));
      } else {
        dst_object_id = related_part_id;
      }
    }
  }
  return ret;
}

ObDASLocationRouter::ObDASLocationRouter(ObIAllocator &allocator)
  : virtual_server_list_(allocator),
    allocator_(allocator)
{
}

int ObDASLocationRouter::nonblock_get_readable_replica(const uint64_t tenant_id,
                                                       const ObTabletID &tablet_id,
                                                       ObDASTabletLoc &tablet_loc,
                                                       int64_t expire_renew_time)
{
  int ret = OB_SUCCESS;
  bool is_cache_hit = false;
  bool is_found = false;
  ObLSLocation ls_loc;
  tablet_loc.tablet_id_ = tablet_id;
  if (OB_FAIL(GCTX.location_service_->get(tenant_id,
                                          tablet_id,
                                          expire_renew_time,
                                          is_cache_hit,
                                          tablet_loc.ls_id_))) {
    LOG_WARN("nonblock get ls id failed", K(ret));
  } else if (OB_FAIL(GCTX.location_service_->get(GCONF.cluster_id,
                                                 tenant_id,
                                                 tablet_loc.ls_id_,
                                                 expire_renew_time,
                                                 is_cache_hit,
                                                 ls_loc))) {
    LOG_WARN("get ls replica location failed", K(ret));
  }

  if (OB_UNLIKELY(tablet_loc.need_refresh_)){
    for (int64_t i = 0; OB_SUCC(ret) && !is_found && i < ls_loc.get_replica_locations().count(); ++i) {
      const ObLSReplicaLocation &tmp_replica_loc = ls_loc.get_replica_locations().at(i);
      if (tmp_replica_loc.is_strong_leader()) {
        //in version 4.0, if das task in retry, we force to choose the leader replica
        tablet_loc.server_ = tmp_replica_loc.get_server();
        is_found = true;
      }
    }
  }

  for (int64_t i = 0; OB_SUCC(ret) && !is_found && i < ls_loc.get_replica_locations().count(); ++i) {
    const ObLSReplicaLocation &tmp_replica_loc = ls_loc.get_replica_locations().at(i);
    if (tmp_replica_loc.get_server() == GCTX.self_addr()) {
      //prefer choose the local replica
      tablet_loc.server_ = tmp_replica_loc.get_server();
      is_found = true;
    }
  }
  if (OB_SUCC(ret) && OB_UNLIKELY(!is_found)) {
    //no local copy, randomly select a readable replica
    int64_t select_idx = rand() % ls_loc.get_replica_locations().count();
    const ObLSReplicaLocation &tmp_replica_loc = ls_loc.get_replica_locations().at(select_idx);
    tablet_loc.server_ = tmp_replica_loc.get_server();
  }
  return ret;
}

int ObDASLocationRouter::get(const ObDASTableLocMeta &loc_meta,
                             const common::ObTabletID &tablet_id,
                             ObLSLocation &location)
{
  int ret = OB_SUCCESS;
  uint64_t tenant_id = MTL_ID();
  bool is_vt = is_virtual_table(loc_meta.ref_table_id_);
  bool is_mapping_real_vt = is_oracle_mapping_real_virtual_table(loc_meta.ref_table_id_);
  uint64_t ref_table_id = loc_meta.ref_table_id_;
  if (is_mapping_real_vt) {
    is_vt = false;
    ref_table_id = share::schema::ObSchemaUtils::get_real_table_mappings_tid(loc_meta.ref_table_id_);
  }
  if (OB_UNLIKELY(is_vt)) {
    if (OB_FAIL(get_vt_ls_location(ref_table_id, tablet_id, location))) {
      LOG_WARN("get virtual table ls location failed", K(ret), K(ref_table_id), K(tablet_id));
    }
  } else {
    int64_t expire_renew_time = 2 * 1000000; // 2s
    bool is_cache_hit = false;
    ObLSID ls_id;
    if (OB_FAIL(GCTX.location_service_->get(tenant_id,
                                            tablet_id,
                                            expire_renew_time,
                                            is_cache_hit,
                                            ls_id))) {
      LOG_WARN("nonblock get ls id failed", K(ret));
    } else if (OB_FAIL(GCTX.location_service_->get(GCONF.cluster_id,
                                            tenant_id,
                                            ls_id,
                                            expire_renew_time,
                                            is_cache_hit,
                                            location))) {
      LOG_WARN("fail to get tablet locations", K(ret), K(tenant_id), K(ls_id));
    }
  }

  return ret;
}

int ObDASLocationRouter::get_tablet_loc(const ObDASTableLocMeta &loc_meta,
                                        const ObTabletID &tablet_id,
                                        ObDASTabletLoc &tablet_loc)
{
  int ret = OB_SUCCESS;
  uint64_t tenant_id = MTL_ID();
  bool is_vt = is_virtual_table(loc_meta.ref_table_id_);
  const int64_t expire_renew_time = tablet_loc.need_refresh_ ? INT64_MAX : 2 * 1000000;
  if (OB_UNLIKELY(is_vt)) {
    if (OB_FAIL(get_vt_tablet_loc(loc_meta.ref_table_id_, tablet_id, tablet_loc))) {
      LOG_WARN("get virtual tablet loc failed", K(ret), K(loc_meta));
    }
  } else if (OB_LIKELY(loc_meta.select_leader_)) {
    ret = get_leader(tenant_id, tablet_id, tablet_loc, expire_renew_time);
  } else {
    ret = nonblock_get_readable_replica(tenant_id, tablet_id, tablet_loc, expire_renew_time);
  }
  return ret;
}

int ObDASLocationRouter::get_leader(const uint64_t tenant_id,
                                    const ObTabletID &tablet_id,
                                    ObDASTabletLoc &tablet_loc,
                                    int64_t expire_renew_time)
{
  int ret = OB_SUCCESS;
  bool is_cache_hit = false;
  tablet_loc.tablet_id_ = tablet_id;
  if (OB_FAIL(GCTX.location_service_->get(tenant_id,
                                          tablet_id,
                                          expire_renew_time,
                                          is_cache_hit,
                                          tablet_loc.ls_id_))) {
    LOG_WARN("nonblock get ls id failed", K(ret));
  } else if (OB_FAIL(GCTX.location_service_->get_leader(GCONF.cluster_id,
                                                        tenant_id,
                                                        tablet_loc.ls_id_,
                                                        false,
                                                        tablet_loc.server_))) {
    LOG_WARN("nonblock get ls location failed", K(ret));
  }
  return ret;
}

int ObDASLocationRouter::get_leader(const uint64_t tenant_id,
                                    const ObTabletID &tablet_id,
                                    ObAddr &leader_addr,
                                    int64_t expire_renew_time)
{
  int ret = OB_SUCCESS;
  bool is_cache_hit = false;
  ObLSID ls_id;
  if (OB_FAIL(GCTX.location_service_->get(tenant_id,
                                          tablet_id,
                                          expire_renew_time,
                                          is_cache_hit,
                                          ls_id))) {
    LOG_WARN("nonblock get ls id failed", K(ret));
  } else if (OB_FAIL(GCTX.location_service_->get_leader(GCONF.cluster_id,
                                                        tenant_id,
                                                        ls_id,
                                                        false,
                                                        leader_addr))) {
    LOG_WARN("nonblock get ls location failed", K(ret));
  }
  return ret;
}


int ObDASLocationRouter::get_full_ls_replica_loc(const ObObjectID &tenant_id,
                                                 const ObDASTabletLoc &tablet_loc,
                                                 ObLSReplicaLocation &replica_loc)
{
  int ret = OB_SUCCESS;
  bool is_cache_hit = false;
  ObLSLocation ls_loc;
  if (OB_FAIL(GCTX.location_service_->get(GCONF.cluster_id,
                                          tenant_id,
                                          tablet_loc.ls_id_,
                                          0, /*not force to renew*/
                                          is_cache_hit,
                                          ls_loc))) {
    LOG_WARN("get ls replica location failed", K(ret));
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < ls_loc.get_replica_locations().count(); ++i) {
    const ObLSReplicaLocation &tmp_replica_loc = ls_loc.get_replica_locations().at(i);
    if (tmp_replica_loc.get_server() == tablet_loc.server_) {
      replica_loc = tmp_replica_loc;
      break;
    }
  }
  if (OB_SUCC(ret) && !replica_loc.is_valid()) {
    ret = OB_LOCATION_NOT_EXIST;
    LOG_WARN("replica location not found", K(ret), K(tablet_loc));
  }
  return ret;
}

int ObDASLocationRouter::get_vt_svr_pair(uint64_t vt_id, const VirtualSvrPair *&vt_pair)
{
  int ret = OB_SUCCESS;
  FOREACH(tmp_node, virtual_server_list_) {
    if (tmp_node->get_table_id() == vt_id) {
      vt_pair =&(*tmp_node);
    }
  }
  if (!is_virtual_table(vt_id)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("vt_id is not virtual table id", K(ret), K(vt_id));
  } else if (nullptr == vt_pair) {
    VirtualSvrPair empty_pair;
    VirtualSvrPair *tmp_pair = nullptr;
    bool is_cache_hit = false;
    ObSEArray<ObAddr, 8> part_locations;
    if (OB_FAIL(virtual_server_list_.push_back(empty_pair))) {
      LOG_WARN("extend virtual server list failed", K(ret));
    } else if (OB_ISNULL(GCTX.location_service_)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("location_service_ is null", KR(ret));
    } else if (OB_FAIL(GCTX.location_service_->vtable_get(
        MTL_ID(),
        vt_id,
        0,/*expire_renew_time*/
        is_cache_hit,
        part_locations))) {
      LOG_WARN("fail to get virtual table location", KR(ret), K(vt_id));
    } else {
      tmp_pair = &virtual_server_list_.get_last();
      if (OB_FAIL(tmp_pair->init(allocator_, vt_id, part_locations))) {
        LOG_WARN("init tmp virtual table svr pair failed", K(ret), K(vt_id));
      } else {
        vt_pair = tmp_pair;
      }
    }
  }
  return ret;
}

OB_NOINLINE int ObDASLocationRouter::get_vt_tablet_loc(uint64_t table_id,
                                                       const ObTabletID &tablet_id,
                                                       ObDASTabletLoc &tablet_loc)
{
  int ret = OB_SUCCESS;
  VirtualSvrPair *final_pair = nullptr;
  FOREACH(tmp_node, virtual_server_list_) {
    if (tmp_node->get_table_id() == table_id) {
      final_pair = &(*tmp_node);
      break;
    }
  }
  if (OB_ISNULL(final_pair)) {
    ret = OB_LOCATION_NOT_EXIST;
    LOG_WARN("virtual table location not exists", K(table_id), K(virtual_server_list_));
  } else if (OB_FAIL(final_pair->get_server_by_tablet_id(tablet_id, tablet_loc.server_))) {
    LOG_WARN("get server by tablet id failed", K(ret), K(tablet_id));
  } else {
    tablet_loc.tablet_id_ = tablet_id;
    tablet_loc.ls_id_ = ObLSID::VT_LS_ID;
  }
  return ret;
}

OB_NOINLINE int ObDASLocationRouter::get_vt_ls_location(uint64_t table_id,
                                                        const ObTabletID &tablet_id,
                                                        ObLSLocation &location)
{
  int ret = OB_SUCCESS;
  bool is_cache_hit = false;
  VirtualSvrPair *server_pair = nullptr;
  FOREACH(tmp_node, virtual_server_list_) {
    if (tmp_node->get_table_id() == table_id) {
      server_pair = &(*tmp_node);
      break;
    }
  }
  if (OB_ISNULL(server_pair)) {
    ret = OB_LOCATION_NOT_EXIST;
    LOG_WARN("not found virtual location", K(ret), K(tablet_id), K(virtual_server_list_));
  } else {
    // mock ls location
    int64_t now = ObTimeUtility::current_time();
    ObReplicaProperty mock_prop;
    ObLSReplicaLocation ls_replica;
    ObAddr server;
    ObLSRestoreStatus restore_status(ObLSRestoreStatus::RESTORE_NONE);
    if (OB_FAIL(location.init(GCONF.cluster_id, MTL_ID(), ObLSID(ObLSID::VT_LS_ID), now))) {
      LOG_WARN("init location failed", KR(ret));
    } else if (OB_FAIL(server_pair->get_server_by_tablet_id(tablet_id, server))) {
      LOG_WARN("get server by tablet id failed", K(ret));
    } else if (OB_FAIL(ls_replica.init(server, common::LEADER,
                       GCONF.mysql_port, REPLICA_TYPE_FULL, mock_prop,
                       restore_status))) {
      LOG_WARN("init ls replica failed", K(ret));
    } else if (OB_FAIL(location.add_replica_location(ls_replica))) {
      LOG_WARN("add replica location failed", K(ret));
    }
  }
  return ret;
}
}  // namespace sql
}  // namespace oceanbase
