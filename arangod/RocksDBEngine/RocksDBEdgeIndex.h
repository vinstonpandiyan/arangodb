////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2016 ArangoDB GmbH, Cologne, Germany
/// Copyright 2004-2014 triAGENS GmbH, Cologne, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is ArangoDB GmbH, Cologne, Germany
///
/// @author Simon Grätzer
////////////////////////////////////////////////////////////////////////////////

#ifndef ARANGOD_ROCKSDB_ENGINE_ROCKSDB_EDGE_INDEX_H
#define ARANGOD_ROCKSDB_ENGINE_ROCKSDB_EDGE_INDEX_H 1

#include "Basics/Common.h"
#include "Indexes/Index.h"
#include "Indexes/IndexIterator.h"
#include "RocksDBEngine/RocksDBIndex.h"
#include "RocksDBEngine/RocksDBKey.h"
#include "RocksDBEngine/RocksDBKeyBounds.h"
#include "VocBase/voc-types.h"
#include "VocBase/vocbase.h"

#include <velocypack/Iterator.h>
#include <velocypack/Slice.h>

namespace rocksdb {
class TransactionDB;
class Iterator;
}  // namespace rocksdb

namespace arangodb {
class RocksDBEdgeIndex;

class RocksDBEdgeIndexIterator final : public IndexIterator {
 public:
  RocksDBEdgeIndexIterator(LogicalCollection* collection,
                           transaction::Methods* trx,
                           ManagedDocumentResult* mmdr,
                           arangodb::RocksDBEdgeIndex const* index,
                           std::unique_ptr<VPackBuilder>& keys);

  ~RocksDBEdgeIndexIterator();

  char const* typeName() const override { return "edge-index-iterator"; }

  bool next(TokenCallback const& cb, size_t limit) override;

  void reset() override;

 private:
  bool updateBounds();

  std::unique_ptr<arangodb::velocypack::Builder> _keys;
  arangodb::velocypack::ArrayIterator _keysIterator;
  RocksDBEdgeIndex const* _index;
  std::unique_ptr<rocksdb::Iterator> _iterator;
  RocksDBKeyBounds _bounds;
};

class RocksDBEdgeIndex final : public RocksDBIndex {
  friend class RocksDBEdgeIndexIterator;

 public:
  RocksDBEdgeIndex() = delete;

  RocksDBEdgeIndex(TRI_idx_iid_t, arangodb::LogicalCollection*,
                   velocypack::Slice const& info, std::string const&);

  ~RocksDBEdgeIndex();

  IndexType type() const override { return Index::TRI_IDX_TYPE_EDGE_INDEX; }

  char const* typeName() const override { return "edge"; }

  bool allowExpansion() const override { return false; }

  bool canBeDropped() const override { return false; }

  bool isSorted() const override { return false; }

  bool hasSelectivityEstimate() const override { return true; }

  double selectivityEstimate(
      arangodb::StringRef const* = nullptr) const override;

  size_t memory() const override;

  void toVelocyPack(VPackBuilder&, bool, bool) const override;

  void toVelocyPackFigures(VPackBuilder&) const override;

  int insert(transaction::Methods*, TRI_voc_rid_t,
             arangodb::velocypack::Slice const&, bool isRollback) override;

  int insertRaw(rocksdb::WriteBatchWithIndex*,
                TRI_voc_rid_t, VPackSlice const&) override;

  int remove(transaction::Methods*, TRI_voc_rid_t,
             arangodb::velocypack::Slice const&, bool isRollback) override;
  
  /// optimization for truncateNoTrx, never called in fillIndex
  int removeRaw(rocksdb::WriteBatch*, TRI_voc_rid_t,
                arangodb::velocypack::Slice const&) override;

  void batchInsert(
      transaction::Methods*,
      std::vector<std::pair<TRI_voc_rid_t, arangodb::velocypack::Slice>> const&,
      std::shared_ptr<arangodb::basics::LocalTaskQueue> queue) override;

  int drop() override;

  bool hasBatchInsert() const override { return false; }

  bool supportsFilterCondition(arangodb::aql::AstNode const*,
                               arangodb::aql::Variable const*, size_t, size_t&,
                               double&) const override;

  IndexIterator* iteratorForCondition(transaction::Methods*,
                                      ManagedDocumentResult*,
                                      arangodb::aql::AstNode const*,
                                      arangodb::aql::Variable const*,
                                      bool) override;

  arangodb::aql::AstNode* specializeCondition(
      arangodb::aql::AstNode*, arangodb::aql::Variable const*) const override;

  /// @brief Transform the list of search slices to search values.
  ///        This will multiply all IN entries and simply return all other
  ///        entries.
  void expandInSearchValues(arangodb::velocypack::Slice const,
                            arangodb::velocypack::Builder&) const override;
  
  int cleanup() override;
  
 private:
  /// @brief create the iterator
  IndexIterator* createEqIterator(transaction::Methods*, ManagedDocumentResult*,
                                  arangodb::aql::AstNode const*,
                                  arangodb::aql::AstNode const*) const;

  IndexIterator* createInIterator(transaction::Methods*, ManagedDocumentResult*,
                                  arangodb::aql::AstNode const*,
                                  arangodb::aql::AstNode const*) const;

  /// @brief add a single value node to the iterator's keys
  void handleValNode(VPackBuilder* keys,
                     arangodb::aql::AstNode const* valNode) const;

  std::string _directionAttr;
};
}  // namespace arangodb

#endif
