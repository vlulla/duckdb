//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/storage/data_table.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/enums/index_constraint_type.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/common/unique_ptr.hpp"
#include "duckdb/storage/index.hpp"
#include "duckdb/storage/statistics/column_statistics.hpp"
#include "duckdb/storage/table/column_segment.hpp"
#include "duckdb/storage/table/data_table_info.hpp"
#include "duckdb/storage/table/persistent_table_data.hpp"
#include "duckdb/storage/table/row_group.hpp"
#include "duckdb/storage/table/row_group_collection.hpp"
#include "duckdb/storage/table/table_statistics.hpp"
#include "duckdb/transaction/local_storage.hpp"

namespace duckdb {

class BoundForeignKeyConstraint;
class ClientContext;
class ColumnDataCollection;
class ColumnDefinition;
class DataTable;
class DuckTransaction;
class RowGroup;
class StorageManager;
class TableCatalogEntry;
class TableIOManager;
class Transaction;
class WriteAheadLog;
class TableDataWriter;
class ConflictManager;
class TableScanState;
struct TableDeleteState;
struct ConstraintState;
struct TableUpdateState;
enum class VerifyExistenceType : uint8_t;

enum class DataTableVersion {
	MAIN_TABLE, // this is the newest version of the table - it has not been altered or dropped
	ALTERED,    // this table has been altered
	DROPPED     // this table has been dropped
};

//! DataTable represents a physical table on disk
class DataTable : public enable_shared_from_this<DataTable> {
public:
	//! Constructs a new data table from an (optional) set of persistent segments
	DataTable(AttachedDatabase &db, shared_ptr<TableIOManager> table_io_manager, const string &schema,
	          const string &table, vector<ColumnDefinition> column_definitions_p,
	          unique_ptr<PersistentTableData> data = nullptr);
	//! Constructs a DataTable as a delta on an existing data table with a newly added column
	DataTable(ClientContext &context, DataTable &parent, ColumnDefinition &new_column, Expression &default_value);
	//! Constructs a DataTable as a delta on an existing data table but with one column removed
	DataTable(ClientContext &context, DataTable &parent, idx_t removed_column);
	//! Constructs a DataTable as a delta on an existing data table but with one column changed type
	DataTable(ClientContext &context, DataTable &parent, idx_t changed_idx, const LogicalType &target_type,
	          const vector<StorageIndex> &bound_columns, Expression &cast_expr);
	//! Constructs a DataTable as a delta on an existing data table but with one column added new constraint
	DataTable(ClientContext &context, DataTable &parent, BoundConstraint &constraint);

	//! A reference to the database instance
	AttachedDatabase &db;

public:
	AttachedDatabase &GetAttached();
	TableIOManager &GetTableIOManager();

	bool IsTemporary() const;

	//! Returns a list of types of the table
	vector<LogicalType> GetTypes();
	const vector<ColumnDefinition> &Columns() const;

	void InitializeScan(ClientContext &context, DuckTransaction &transaction, TableScanState &state,
	                    const vector<StorageIndex> &column_ids, optional_ptr<TableFilterSet> table_filters = nullptr);

	//! Returns the maximum amount of threads that should be assigned to scan this data table
	idx_t MaxThreads(ClientContext &context) const;
	void InitializeParallelScan(ClientContext &context, ParallelTableScanState &state);
	bool NextParallelScan(ClientContext &context, ParallelTableScanState &state, TableScanState &scan_state);

	//! Scans up to STANDARD_VECTOR_SIZE elements from the table starting
	//! from offset and store them in result. Offset is incremented with how many
	//! elements were returned.
	//! Returns true if all pushed down filters were executed during data fetching
	void Scan(DuckTransaction &transaction, DataChunk &result, TableScanState &state);

	//! Fetch data from the specific row identifiers from the base table
	void Fetch(DuckTransaction &transaction, DataChunk &result, const vector<StorageIndex> &column_ids,
	           const Vector &row_ids, idx_t fetch_count, ColumnFetchState &state);
	//! Returns true, if the transaction can fetch the row ID.
	bool CanFetch(DuckTransaction &transaction, const row_t row_id);

	//! Initializes appending to transaction-local storage
	void InitializeLocalAppend(LocalAppendState &state, TableCatalogEntry &table, ClientContext &context,
	                           const vector<unique_ptr<BoundConstraint>> &bound_constraints);
	//! Initializes only the delete-indexes of the transaction-local storage
	void InitializeLocalStorage(LocalAppendState &state, TableCatalogEntry &table, ClientContext &context,
	                            const vector<unique_ptr<BoundConstraint>> &bound_constraints);
	//! Append a DataChunk to the transaction-local storage of the table.
	void LocalAppend(LocalAppendState &state, ClientContext &context, DataChunk &chunk, bool unsafe);
	//! Finalizes a transaction-local append
	void FinalizeLocalAppend(LocalAppendState &state);
	//! Append a chunk to the transaction-local storage of this table and update the delete indexes.
	void LocalAppend(TableCatalogEntry &table, ClientContext &context, DataChunk &chunk,
	                 const vector<unique_ptr<BoundConstraint>> &bound_constraints, Vector &row_ids,
	                 DataChunk &delete_chunk);
	//! Appends to the transaction-local storage of this table
	void LocalAppend(TableCatalogEntry &table, ClientContext &context, DataChunk &chunk,
	                 const vector<unique_ptr<BoundConstraint>> &bound_constraints);
	//! Append a chunk to the transaction-local storage of this table.
	void LocalWALAppend(TableCatalogEntry &table, ClientContext &context, DataChunk &chunk,
	                    const vector<unique_ptr<BoundConstraint>> &bound_constraints);
	//! Append a column data collection with default values to the transaction-local storage of this table.
	void LocalAppend(TableCatalogEntry &table, ClientContext &context, ColumnDataCollection &collection,
	                 const vector<unique_ptr<BoundConstraint>> &bound_constraints,
	                 optional_ptr<const vector<LogicalIndex>> column_ids);
	//! Merge a row group collection into the transaction-local storage
	void LocalMerge(ClientContext &context, RowGroupCollection &collection);
	//! Create an optimistic row group collection for this table. Used for optimistically writing parallel appends.
	//! Returns the index into the optimistic_collections vector for newly created collection.
	PhysicalIndex CreateOptimisticCollection(ClientContext &context, unique_ptr<RowGroupCollection> collection);
	//! Returns the optimistic row group collection corresponding to the index.
	RowGroupCollection &GetOptimisticCollection(ClientContext &context, const PhysicalIndex collection_index);
	//! Resets the optimistic row group collection corresponding to the index.
	void ResetOptimisticCollection(ClientContext &context, const PhysicalIndex collection_index);
	//! Returns the optimistic writer of the corresponding local table.
	OptimisticDataWriter &GetOptimisticWriter(ClientContext &context);

	unique_ptr<TableDeleteState> InitializeDelete(TableCatalogEntry &table, ClientContext &context,
	                                              const vector<unique_ptr<BoundConstraint>> &bound_constraints);
	//! Delete the entries with the specified row identifier from the table
	idx_t Delete(TableDeleteState &state, ClientContext &context, Vector &row_ids, idx_t count);

	unique_ptr<TableUpdateState> InitializeUpdate(TableCatalogEntry &table, ClientContext &context,
	                                              const vector<unique_ptr<BoundConstraint>> &bound_constraints);
	//! Update the entries with the specified row identifier from the table
	void Update(TableUpdateState &state, ClientContext &context, Vector &row_ids,
	            const vector<PhysicalIndex> &column_ids, DataChunk &data);
	//! Update a single (sub-)column along a column path
	//! The column_path vector is a *path* towards a column within the table
	//! i.e. if we have a table with a single column S STRUCT(A INT, B INT)
	//! and we update the validity mask of "S.B"
	//! the column path is:
	//! 0 (first column of table)
	//! -> 1 (second subcolumn of struct)
	//! -> 0 (first subcolumn of INT)
	//! This method should only be used from the WAL replay. It does not verify update constraints.
	void UpdateColumn(TableCatalogEntry &table, ClientContext &context, Vector &row_ids,
	                  const vector<column_t> &column_path, DataChunk &updates);

	//! Fetches an append lock
	void AppendLock(TableAppendState &state);
	//! Begin appending structs to this table, obtaining necessary locks, etc
	void InitializeAppend(DuckTransaction &transaction, TableAppendState &state);
	//! Append a chunk to the table using the AppendState obtained from InitializeAppend
	void Append(DataChunk &chunk, TableAppendState &state);
	//! Finalize an append
	void FinalizeAppend(DuckTransaction &transaction, TableAppendState &state);
	//! Commit the append
	void CommitAppend(transaction_t commit_id, idx_t row_start, idx_t count);
	//! Write a segment of the table to the WAL
	void WriteToLog(DuckTransaction &transaction, WriteAheadLog &log, idx_t row_start, idx_t count,
	                optional_ptr<StorageCommitState> commit_state);
	//! Revert a set of appends made by the given AppendState, used to revert appends in the event of an error during
	//! commit (e.g. because of an I/O exception)
	void RevertAppend(DuckTransaction &transaction, idx_t start_row, idx_t count);
	void RevertAppendInternal(idx_t start_row);

	void ScanTableSegment(DuckTransaction &transaction, idx_t start_row, idx_t count,
	                      const std::function<void(DataChunk &chunk)> &function);

	//! Merge a row group collection directly into this table - appending it to the end of the table without copying
	void MergeStorage(RowGroupCollection &data, TableIndexList &indexes, optional_ptr<StorageCommitState> commit_state);

	//! Append a chunk with the row ids [row_start, ..., row_start + chunk.size()] to all indexes of the table.
	//! Returns empty ErrorData, if the append was successful.
	ErrorData AppendToIndexes(optional_ptr<TableIndexList> delete_indexes, DataChunk &chunk, row_t row_start,
	                          const IndexAppendMode index_append_mode);
	static ErrorData AppendToIndexes(TableIndexList &indexes, optional_ptr<TableIndexList> delete_indexes,
	                                 DataChunk &chunk, row_t row_start, const IndexAppendMode index_append_mode);
	//! Remove a chunk with the row ids [row_start, ..., row_start + chunk.size()] from all indexes of the table
	void RemoveFromIndexes(TableAppendState &state, DataChunk &chunk, row_t row_start);
	//! Remove the chunk with the specified set of row identifiers from all indexes of the table
	void RemoveFromIndexes(TableAppendState &state, DataChunk &chunk, Vector &row_identifiers);
	//! Remove the row identifiers from all the indexes of the table
	void RemoveFromIndexes(Vector &row_identifiers, idx_t count);

	void SetAsMainTable() {
		this->version = DataTableVersion::MAIN_TABLE;
	}

	void SetAsDropped() {
		this->version = DataTableVersion::DROPPED;
	}

	bool IsMainTable() const {
		return this->version == DataTableVersion::MAIN_TABLE;
	}
	bool IsRoot() const {
		return IsMainTable();
	}
	string TableModification() const;

	//! Get statistics of a physical column within the table
	unique_ptr<BaseStatistics> GetStatistics(ClientContext &context, column_t column_id);

	//! Get table sample
	unique_ptr<BlockingSample> GetSample();
	//! Sets statistics of a physical column within the table
	void SetDistinct(column_t column_id, unique_ptr<DistinctStatistics> distinct_stats);

	//! Obtains a shared lock to prevent checkpointing while operations are running
	unique_ptr<StorageLockKey> GetSharedCheckpointLock();
	//! Obtains a lock during a checkpoint operation that prevents other threads from reading this table
	unique_ptr<StorageLockKey> GetCheckpointLock();
	//! Checkpoint the table to the specified table data writer
	void Checkpoint(TableDataWriter &writer, Serializer &serializer);
	void CommitDropTable();
	void CommitDropColumn(const idx_t column_index);

	idx_t ColumnCount() const;
	idx_t GetTotalRows() const;

	vector<ColumnSegmentInfo> GetColumnSegmentInfo();

	//! Scans the next chunk for the CREATE INDEX operator
	bool CreateIndexScan(TableScanState &state, DataChunk &result, TableScanType type);
	//! Returns true, if the index name is unique (i.e., no PK, UNIQUE, FK constraint has the same name)
	//! FIXME: This is only necessary until we treat all indexes as catalog entries, allowing to alter constraints
	bool IndexNameIsUnique(const string &name);

	//! Initialize constraint verification state
	unique_ptr<ConstraintState> InitializeConstraintState(TableCatalogEntry &table,
	                                                      const vector<unique_ptr<BoundConstraint>> &bound_constraints);
	//! Verify constraints with a chunk from the Append containing all columns of the table
	void VerifyAppendConstraints(ConstraintState &constraint_state, ClientContext &context, DataChunk &chunk,
	                             optional_ptr<LocalTableStorage> local_storage, optional_ptr<ConflictManager> manager);

	shared_ptr<DataTableInfo> &GetDataTableInfo();

	void BindIndexes(ClientContext &context);
	bool HasIndexes() const;
	bool HasUniqueIndexes() const;
	bool HasForeignKeyIndex(const vector<PhysicalIndex> &keys, ForeignKeyType type);
	void SetIndexStorageInfo(vector<IndexStorageInfo> index_storage_info);
	void VacuumIndexes();
	void VerifyIndexBuffers();
	void CleanupAppend(transaction_t lowest_transaction, idx_t start, idx_t count);

	string GetTableName() const;
	void SetTableName(string new_name);

	TableStorageInfo GetStorageInfo();

	idx_t GetRowGroupSize() const;

	//! Verify any unique indexes using optional delete indexes in the local storage.
	void VerifyUniqueIndexes(TableIndexList &indexes, optional_ptr<LocalTableStorage> storage, DataChunk &chunk,
	                         optional_ptr<ConflictManager> manager);
	//! AddIndex initializes an index and adds it to the table's index list.
	//! It is either empty, or initialized via its index storage information.
	void AddIndex(const ColumnList &columns, const vector<LogicalIndex> &column_indexes, const IndexConstraintType type,
	              const IndexStorageInfo &info);
	//! AddIndex moves an index to this table's index list.
	void AddIndex(unique_ptr<Index> index);

	//! Returns a list of the partition stats
	vector<PartitionStatistics> GetPartitionStats(ClientContext &context);

private:
	//! Verify the new added constraints against current persistent&local data
	void VerifyNewConstraint(LocalStorage &local_storage, DataTable &parent, const BoundConstraint &constraint);

	//! Verify constraints with a chunk from the Update containing only the specified column_ids
	void VerifyUpdateConstraints(ConstraintState &state, ClientContext &context, DataChunk &chunk,
	                             const vector<PhysicalIndex> &column_ids);
	//! Verify constraints with a chunk from the Delete containing all columns of the table
	void VerifyDeleteConstraints(optional_ptr<LocalTableStorage> storage, TableDeleteState &state,
	                             ClientContext &context, DataChunk &chunk);

	void InitializeScanWithOffset(DuckTransaction &transaction, TableScanState &state,
	                              const vector<StorageIndex> &column_ids, idx_t start_row, idx_t end_row);

	void VerifyForeignKeyConstraint(optional_ptr<LocalTableStorage> storage,
	                                const BoundForeignKeyConstraint &bound_foreign_key, ClientContext &context,
	                                DataChunk &chunk, VerifyExistenceType type);
	void VerifyAppendForeignKeyConstraint(optional_ptr<LocalTableStorage> storage,
	                                      const BoundForeignKeyConstraint &bound_foreign_key, ClientContext &context,
	                                      DataChunk &chunk);
	void VerifyDeleteForeignKeyConstraint(optional_ptr<LocalTableStorage> storage,
	                                      const BoundForeignKeyConstraint &bound_foreign_key, ClientContext &context,
	                                      DataChunk &chunk);

private:
	//! The table info
	shared_ptr<DataTableInfo> info;
	//! The set of physical columns stored by this DataTable
	vector<ColumnDefinition> column_definitions;
	//! Lock for appending entries to the table
	mutex append_lock;
	//! The row groups of the table
	shared_ptr<RowGroupCollection> row_groups;
	//! The version of the data table
	atomic<DataTableVersion> version;
};
} // namespace duckdb
