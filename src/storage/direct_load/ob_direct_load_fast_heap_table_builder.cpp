// Copyright (c) 2022-present Oceanbase Inc. All Rights Reserved.
// Author:
//   suzhi.yt <>

#define USING_LOG_PREFIX STORAGE

#include "storage/direct_load/ob_direct_load_fast_heap_table_builder.h"
#include "share/stat/ob_opt_column_stat.h"
#include "share/stat/ob_stat_define.h"
#include "share/table/ob_table_load_define.h"
#include "storage/ddl/ob_direct_insert_sstable_ctx.h"
#include "storage/direct_load/ob_direct_load_fast_heap_table.h"
#include "storage/direct_load/ob_direct_load_insert_table_ctx.h"

namespace oceanbase
{
namespace storage
{
using namespace common;
using namespace blocksstable;
using namespace share;

/**
 * ObDirectLoadFastHeapTableBuildParam
 */

ObDirectLoadFastHeapTableBuildParam::ObDirectLoadFastHeapTableBuildParam()
  : snapshot_version_(0),
    col_descs_(nullptr),
    insert_table_ctx_(nullptr),
    fast_heap_table_ctx_(nullptr),
    result_info_(nullptr),
    online_opt_stat_gather_(false)
{
}

ObDirectLoadFastHeapTableBuildParam::~ObDirectLoadFastHeapTableBuildParam()
{
}

bool ObDirectLoadFastHeapTableBuildParam::is_valid() const
{
  return tablet_id_.is_valid() && snapshot_version_ > 0 && table_data_desc_.is_valid() &&
         nullptr != col_descs_ && nullptr != insert_table_ctx_ && nullptr != fast_heap_table_ctx_ &&
         nullptr != result_info_;
}

/**
 * ObDirectLoadFastHeapTableBuilder
 */

ObDirectLoadFastHeapTableBuilder::ObDirectLoadFastHeapTableBuilder()
  : allocator_("TLD_FastHTable"),
    fast_heap_table_tablet_ctx_(nullptr),
    slice_writer_(nullptr),
    row_count_(0),
    is_closed_(false),
    is_inited_(false)
{
}

ObDirectLoadFastHeapTableBuilder::~ObDirectLoadFastHeapTableBuilder()
{
  if (nullptr != slice_writer_) {
    slice_writer_->~ObSSTableInsertSliceWriter();
    allocator_.free(slice_writer_);
    slice_writer_ = nullptr;
  }
  for (int64_t i = 0; i < column_stat_array_.count(); ++i) {
    ObOptColumnStat *col_stat = column_stat_array_.at(i);
    col_stat->~ObOptColumnStat();
    col_stat = nullptr;
  }
}

int ObDirectLoadFastHeapTableBuilder::init_sql_statistics()
{
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < param_.table_data_desc_.column_count_; ++i) {
    ObOptColumnStat *col_stat = OB_NEWx(ObOptColumnStat, (&allocator_), allocator_);
    if (OB_ISNULL(col_stat)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      LOG_WARN("fail to allocate buffer", KR(ret));
    } else if (OB_FAIL(column_stat_array_.push_back(col_stat))) {
      LOG_WARN("fail to push back", KR(ret));
    }
    if (OB_FAIL(ret)) {
      if (col_stat != nullptr) {
        col_stat->~ObOptColumnStat();
        allocator_.free(col_stat);
        col_stat = nullptr;
      }
    }
  }
  return ret;
}

int ObDirectLoadFastHeapTableBuilder::collect_obj(const ObDatumRow &datum_row)
{
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < param_.table_data_desc_.column_count_; i++) {
    const ObStorageDatum &datum =
      datum_row.storage_datums_[i + ObMultiVersionRowkeyHelpper::get_extra_rowkey_col_cnt() + 1];
    const ObColDesc &col_desc = param_.col_descs_->at(i + 1);
    ObOptColumnStat *col_stat = column_stat_array_.at(i);
    bool is_valid = ObColumnStatParam::is_valid_histogram_type(col_desc.col_type_.get_type());
    if (col_stat != nullptr && is_valid) {
      ObObj obj;
      if (OB_FAIL(datum.to_obj_enhance(obj, col_desc.col_type_))) {
        LOG_WARN("Failed to transform datum to obj", K(ret), K(i), K(datum_row.storage_datums_[i]));
      } else if (OB_FAIL(col_stat->merge_obj(obj))) {
        LOG_WARN("Failed to merge obj", K(ret), K(obj), KP(col_stat));
      }
    }
  }
  return ret;
}

int ObDirectLoadFastHeapTableBuilder::init(const ObDirectLoadFastHeapTableBuildParam &param)
{
  int ret = OB_SUCCESS;
  if (IS_INIT) {
    ret = OB_INIT_TWICE;
    LOG_WARN("ObDirectLoadFastHeapTableBuilder init twice", KR(ret), KP(this));
  } else if (OB_UNLIKELY(!param.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid args", KR(ret), K(param));
  } else {
    param_ = param;
    allocator_.set_tenant_id(MTL_ID());
    if (param_.online_opt_stat_gather_ && OB_FAIL(init_sql_statistics())) {
      LOG_WARN("fail to inner init sql statistics", KR(ret));
    } else if (OB_FAIL(param_.fast_heap_table_ctx_->get_tablet_context(
                 param_.tablet_id_, fast_heap_table_tablet_ctx_))) {
      LOG_WARN("fail to get tablet context", KR(ret));
    } else if (OB_FAIL(init_sstable_slice_ctx())) {
      LOG_WARN("fail to init sstable slice ctx", KR(ret));
    } else if (OB_FAIL(datum_row_.init(param.table_data_desc_.column_count_ +
                                       HIDDEN_ROWKEY_COLUMN_NUM +
                                       ObMultiVersionRowkeyHelpper::get_extra_rowkey_col_cnt()))) {
      LOG_WARN("fail to init datum row", KR(ret));
    } else {
      datum_row_.row_flag_.set_flag(ObDmlFlag::DF_INSERT);
      datum_row_.mvcc_row_flag_.set_last_multi_version_row(true);
      datum_row_.storage_datums_[HIDDEN_ROWKEY_COLUMN_NUM].set_int(-param_.snapshot_version_); // fill trans_version
      datum_row_.storage_datums_[HIDDEN_ROWKEY_COLUMN_NUM + 1].set_int(0); // fill sql_no
      is_inited_ = true;
    }
  }
  return ret;
}

int ObDirectLoadFastHeapTableBuilder::init_sstable_slice_ctx()
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(fast_heap_table_tablet_ctx_->get_write_ctx(write_ctx_))) {
    LOG_WARN("fail to get write ctx", KR(ret));
  } else if (OB_FAIL(param_.insert_table_ctx_->construct_sstable_slice_writer(
               fast_heap_table_tablet_ctx_->get_target_tablet_id(),
               write_ctx_.start_seq_,
               slice_writer_,
               allocator_))) {
    LOG_WARN("fail to construct sstable slice writer", KR(ret));
  }
  return ret;
}

int ObDirectLoadFastHeapTableBuilder::switch_sstable_slice()
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(slice_writer_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected null slice builder", KR(ret));
  } else if (OB_FAIL(slice_writer_->close())) {
    LOG_WARN("fail to close sstable slice builder", KR(ret));
  } else {
    slice_writer_->~ObSSTableInsertSliceWriter();
    allocator_.reuse();
    if (OB_FAIL(init_sstable_slice_ctx())) {
      LOG_WARN("fail to init sstable slice ctx", KR(ret));
    }
  }
  return ret;
}

int ObDirectLoadFastHeapTableBuilder::append_row(const ObTabletID &tablet_id,
                                                 const ObDatumRow &datum_row)
{
  UNUSED(tablet_id);
  int ret = OB_SUCCESS;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    LOG_WARN("ObDirectLoadFastHeapTableBuilder not init", KR(ret), KP(this));
  } else if (OB_UNLIKELY(is_closed_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("fast heap table builder is closed", KR(ret));
  } else if (OB_FAIL(!datum_row.is_valid() ||
                     datum_row.get_column_count() != param_.table_data_desc_.column_count_)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid args", KR(ret), K(datum_row), K(param_.table_data_desc_.column_count_));
  } else {
    uint64_t pk_seq = OB_INVALID_ID;
    if (OB_FAIL(write_ctx_.pk_interval_.next_value(pk_seq))) {
      if (OB_UNLIKELY(OB_EAGAIN != ret)) {
        LOG_WARN("fail to get next pk seq", KR(ret));
      } else if (OB_FAIL(switch_sstable_slice())) {
        LOG_WARN("fail to switch sstable slice", KR(ret));
      } else if (OB_FAIL(write_ctx_.pk_interval_.next_value(pk_seq))) {
        LOG_WARN("fail to get next pk seq", KR(ret));
      }
    }
    if (OB_SUCC(ret)) {
      datum_row_.storage_datums_[0].set_int(pk_seq);
      for (int64_t i = 0, j = HIDDEN_ROWKEY_COLUMN_NUM +
                              ObMultiVersionRowkeyHelpper::get_extra_rowkey_col_cnt();
           i < datum_row.count_; ++i, ++j) {
        datum_row_.storage_datums_[j] = datum_row.storage_datums_[i];
      }
      if (OB_FAIL(slice_writer_->append_row(datum_row_))) {
        LOG_WARN("fail to append row", KR(ret));
      } else if (param_.online_opt_stat_gather_ && OB_FAIL(collect_obj(datum_row_))) {
        LOG_WARN("fail to collect", KR(ret));
      } else {
        ++row_count_;
        ATOMIC_INC(&param_.result_info_->rows_affected_);
      }
    }
  }
  return ret;
}

int ObDirectLoadFastHeapTableBuilder::close()
{
  int ret = OB_SUCCESS;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    LOG_WARN("ObDirectLoadFastHeapTableBuilder not init", KR(ret), KP(this));
  } else if (OB_UNLIKELY(is_closed_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("fast heap table builder is closed", KR(ret));
  } else {
    if (OB_FAIL(slice_writer_->close())) {
      LOG_WARN("fail to close sstable slice writer", KR(ret));
    } else {
      is_closed_ = true;
    }
  }
  return ret;
}

int ObDirectLoadFastHeapTableBuilder::get_tables(
  ObIArray<ObIDirectLoadPartitionTable *> &table_array, ObIAllocator &allocator)
{
  int ret = OB_SUCCESS;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    LOG_WARN("ObDirectLoadFastHeapTableBuilder not init", KR(ret), KP(this));
  } else if (OB_UNLIKELY(!is_closed_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("fast heap table builder not closed", KR(ret));
  } else {
    ObDirectLoadFastHeapTableCreateParam create_param;
    create_param.tablet_id_ = param_.tablet_id_;
    create_param.row_count_ = row_count_;
    create_param.column_stat_array_ = &column_stat_array_;
    ObDirectLoadFastHeapTable *fast_heap_table = nullptr;
    if (OB_ISNULL(fast_heap_table = OB_NEWx(ObDirectLoadFastHeapTable, (&allocator)))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      LOG_WARN("fail to new ObDirectLoadFastHeapTable", KR(ret));
    } else if (OB_FAIL(fast_heap_table->init(create_param))) {
      LOG_WARN("fail to init sstable", KR(ret), K(create_param));
    } else if (OB_FAIL(table_array.push_back(fast_heap_table))) {
      LOG_WARN("fail to push back sstable", KR(ret));
    }
    if (OB_FAIL(ret)) {
      if (nullptr != fast_heap_table) {
        fast_heap_table->~ObDirectLoadFastHeapTable();
        allocator.free(fast_heap_table);
        fast_heap_table = nullptr;
      }
    }
  }
  return ret;
}

} // namespace storage
} // namespace oceanbase
