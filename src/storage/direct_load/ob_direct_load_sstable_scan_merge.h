// Copyright (c) 2022-present Oceanbase Inc. All Rights Reserved.
// Author:
//   suzhi.yt <>

#pragma once

#include "lib/container/ob_loser_tree.h"
#include "lib/container/ob_se_array.h"
#include "storage/access/ob_simple_rows_merger.h"
#include "storage/direct_load/ob_direct_load_sstable_scan_merge_loser_tree.h"
#include "storage/direct_load/ob_direct_load_table_data_desc.h"
#include "storage/access/ob_store_row_iterator.h"
#include "storage/direct_load/ob_direct_load_sstable.h"
#include "share/table/ob_table_load_define.h"

namespace oceanbase
{
namespace blocksstable
{
class ObDatumRange;
class ObStorageDatumUtils;
} // namespace blocksstable
namespace observer
{
class ObTableLoadErrorRowHandler;
} // namespace observer
namespace storage
{
class ObDirectLoadExternalRow;
struct ObDirectLoadSSTableScanMergeParam
{
public:
  ObDirectLoadSSTableScanMergeParam();
  ~ObDirectLoadSSTableScanMergeParam();
  bool is_valid() const;
  TO_STRING_KV(K_(tablet_id), K_(table_data_desc), KP_(datum_utils), KP_(error_row_handler), KP_(result_info));
public:
  common::ObTabletID tablet_id_;
  ObDirectLoadTableDataDesc table_data_desc_;
  const blocksstable::ObStorageDatumUtils *datum_utils_;
  observer::ObTableLoadErrorRowHandler *error_row_handler_;
  table::ObTableLoadResultInfo *result_info_;
};

class ObDirectLoadSSTableScanMerge : public ObIStoreRowIterator
{
public:
  static const int64_t MAX_SSTABLE_COUNT = 1024;
  typedef ObDirectLoadSSTableScanMergeLoserTreeItem LoserTreeItem;
  typedef ObDirectLoadSSTableScanMergeLoserTreeCompare LoserTreeCompare;
  typedef ObSimpleRowsMerger<LoserTreeItem, LoserTreeCompare> ScanSimpleMerger;
  typedef common::ObLoserTree<LoserTreeItem, LoserTreeCompare, MAX_SSTABLE_COUNT>
    ScanMergeLoserTree;
public:
  ObDirectLoadSSTableScanMerge();
  ~ObDirectLoadSSTableScanMerge();
  void reset();
  int init(const ObDirectLoadSSTableScanMergeParam &param,
           const common::ObIArray<ObDirectLoadSSTable *> &sstable_array,
           const blocksstable::ObDatumRange &range);
  int get_next_row(const ObDirectLoadExternalRow *&external_row);
  int get_next_row(const blocksstable::ObDatumRow *&datum_row) override;
private:
  int init_rows_merger(int64_t sstable_count);
  int supply_consume();
  int inner_get_next_row(const ObDirectLoadExternalRow *&external_row);
private:
  common::ObArenaAllocator allocator_;
  common::ObTabletID tablet_id_;
  ObDirectLoadTableDataDesc table_data_desc_;
  const blocksstable::ObStorageDatumUtils *datum_utils_;
  observer::ObTableLoadErrorRowHandler *error_row_handler_;
  table::ObTableLoadResultInfo *result_info_;
  const blocksstable::ObDatumRange *range_;
  common::ObSEArray<ObDirectLoadSSTableScanner *, 64> scanners_;
  int64_t *consumers_;
  int64_t consumer_cnt_;
  LoserTreeCompare compare_;
  ScanSimpleMerger *simple_merge_;
  ScanMergeLoserTree *loser_tree_;
  common::ObRowsMerger<LoserTreeItem, LoserTreeCompare> *rows_merger_;
  blocksstable::ObDatumRow datum_row_;
  bool is_inited_;
};

} // namespace storage
} // namespace oceanbase
