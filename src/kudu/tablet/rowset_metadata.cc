// Copyright (c) 2014, Cloudera, inc.

#include "kudu/tablet/rowset_metadata.h"

#include <string>
#include <utility>
#include <vector>

#include "kudu/common/wire_protocol.h"
#include "kudu/gutil/strings/substitute.h"
#include "kudu/gutil/map-util.h"

using strings::Substitute;

namespace kudu {
namespace tablet {

// ============================================================================
//  RowSet Metadata
// ============================================================================
Status RowSetMetadata::Load(TabletMetadata* tablet_metadata,
                            const RowSetDataPB& pb,
                            gscoped_ptr<RowSetMetadata>* metadata) {
  gscoped_ptr<RowSetMetadata> ret(new RowSetMetadata(tablet_metadata));
  RETURN_NOT_OK(ret->InitFromPB(pb));
  metadata->reset(ret.release());
  return Status::OK();
}

Status RowSetMetadata::CreateNew(TabletMetadata* tablet_metadata,
                                 int64_t id,
                                 const Schema& schema,
                                 gscoped_ptr<RowSetMetadata>* metadata) {
  metadata->reset(new RowSetMetadata(tablet_metadata, id, schema));
  return Status::OK();
}

Status RowSetMetadata::Flush() {
  return tablet_metadata_->Flush();
}

Status RowSetMetadata::InitFromPB(const RowSetDataPB& pb) {
  CHECK(!initted_);

  id_ = pb.id();

  // Load Bloom File
  if (pb.has_bloom_block()) {
    bloom_block_ = BlockId::FromPB(pb.bloom_block());
  }

  // Load AdHoc Index File
  if (pb.has_adhoc_index_block()) {
    adhoc_index_block_ = BlockId::FromPB(pb.adhoc_index_block());
  }

  // Load Column Files
  int key_columns = 0;
  std::vector<size_t> cols_ids;
  std::vector<ColumnSchema> cols;
  BOOST_FOREACH(const ColumnDataPB& col_pb, pb.columns()) {
    column_blocks_.push_back(BlockId::FromPB(col_pb.block()));
    cols.push_back(ColumnSchemaFromPB(col_pb.schema()));
    cols_ids.push_back(col_pb.schema().id());
    key_columns += !!col_pb.schema().is_key();
  }
  RETURN_NOT_OK(schema_.Reset(cols, cols_ids, key_columns));

  // Load redo delta files
  BOOST_FOREACH(const DeltaDataPB& redo_delta_pb, pb.redo_deltas()) {
    redo_delta_blocks_.push_back(BlockId::FromPB(redo_delta_pb.block()));
  }

  last_durable_redo_dms_id_ = pb.last_durable_dms_id();

  // Load undo delta files
  BOOST_FOREACH(const DeltaDataPB& undo_delta_pb, pb.undo_deltas()) {
    undo_delta_blocks_.push_back(BlockId::FromPB(undo_delta_pb.block()));
  }

  initted_ = true;
  return Status::OK();
}

void RowSetMetadata::ToProtobuf(RowSetDataPB *pb) {
  pb->set_id(id_);

  // Write Column Files
  size_t idx = 0;
  BOOST_FOREACH(const BlockId& block_id, column_blocks_) {
    ColumnDataPB *col_data = pb->add_columns();
    ColumnSchemaPB *col_schema = col_data->mutable_schema();
    block_id.CopyToPB(col_data->mutable_block());
    ColumnSchemaToPB(schema_.column(idx), col_schema);
    col_schema->set_id(schema_.column_id(idx));
    col_schema->set_is_key(idx < schema_.num_key_columns());
    idx++;
  }

  // Write Delta Files
  {
    boost::lock_guard<LockType> l(deltas_lock_);
    pb->set_last_durable_dms_id(last_durable_redo_dms_id_);

    BOOST_FOREACH(const BlockId& redo_delta_block, redo_delta_blocks_) {
      DeltaDataPB *redo_delta_pb = pb->add_redo_deltas();
      redo_delta_block.CopyToPB(redo_delta_pb->mutable_block());
    }

    BOOST_FOREACH(const BlockId& undo_delta_block, undo_delta_blocks_) {
      DeltaDataPB *undo_delta_pb = pb->add_undo_deltas();
      undo_delta_block.CopyToPB(undo_delta_pb->mutable_block());
    }
  }

  // Write Bloom File
  if (!bloom_block_.IsNull()) {
    bloom_block_.CopyToPB(pb->mutable_bloom_block());
  }

  // Write AdHoc Index
  if (!adhoc_index_block_.IsNull()) {
    adhoc_index_block_.CopyToPB(pb->mutable_adhoc_index_block());
  }
}

const string RowSetMetadata::ToString() const {
  return Substitute("RowSet($0)", id_);
}

void RowSetMetadata::SetColumnDataBlocks(const std::vector<BlockId>& blocks) {
  CHECK_EQ(blocks.size(), schema_.num_columns());
  boost::lock_guard<LockType> l(deltas_lock_);
  column_blocks_ = blocks;
}

Status RowSetMetadata::CommitRedoDeltaDataBlock(int64_t dms_id,
                                                const BlockId& block_id) {
  boost::lock_guard<LockType> l(deltas_lock_);
  last_durable_redo_dms_id_ = dms_id;
  redo_delta_blocks_.push_back(block_id);
  return Status::OK();
}

Status RowSetMetadata::CommitUndoDeltaDataBlock(const BlockId& block_id) {
  boost::lock_guard<LockType> l(deltas_lock_);
  undo_delta_blocks_.push_back(block_id);
  return Status::OK();
}

Status RowSetMetadata::CommitUpdate(const RowSetMetadataUpdate& update) {
  boost::lock_guard<LockType> l(deltas_lock_);

  BOOST_FOREACH(const RowSetMetadataUpdate::ReplaceDeltaBlocks rep, update.replace_redo_blocks_) {
    CHECK(!rep.to_remove.empty());

    vector<BlockId>::iterator start_it =
      std::find(redo_delta_blocks_.begin(), redo_delta_blocks_.end(), rep.to_remove[0]);

    vector<BlockId>::iterator end_it = start_it;
    BOOST_FOREACH(const BlockId& b, rep.to_remove) {
      if (end_it == redo_delta_blocks_.end() || *end_it != b) {
        return Status::InvalidArgument(
          Substitute("Cannot find subsequence <$0> in <$1>",
                     BlockId::JoinStrings(rep.to_remove),
                     BlockId::JoinStrings(redo_delta_blocks_)));
      }
      ++end_it;
    }

    redo_delta_blocks_.erase(start_it, end_it);
    redo_delta_blocks_.insert(start_it, rep.to_add.begin(), rep.to_add.end());
  }

  // Add new redo blocks
  BOOST_FOREACH(const BlockId& b, update.new_redo_blocks_) {
    redo_delta_blocks_.push_back(b);
  }

  typedef std::pair<int, BlockId> IntBlockPair;
  BOOST_FOREACH(const IntBlockPair& e, update.cols_to_replace_) {
    CHECK_LT(e.first, column_blocks_.size());
    column_blocks_[e.first] = e.second;
  }
  return Status::OK();
}

RowSetMetadataUpdate::RowSetMetadataUpdate() {
}
RowSetMetadataUpdate::~RowSetMetadataUpdate() {
}
RowSetMetadataUpdate& RowSetMetadataUpdate::ReplaceColumnBlock(
    int col_idx, const BlockId& block_id) {
  InsertOrDie(&cols_to_replace_, col_idx, block_id);
  return *this;
}
RowSetMetadataUpdate& RowSetMetadataUpdate::ReplaceRedoDeltaBlocks(
    const std::vector<BlockId>& to_remove,
    const std::vector<BlockId>& to_add) {

  ReplaceDeltaBlocks rdb = { to_remove, to_add };
  replace_redo_blocks_.push_back(rdb);
  return *this;
}


} // namespace tablet
} // namespace kudu
