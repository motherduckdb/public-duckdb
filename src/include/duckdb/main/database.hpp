//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/main/database.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/winapi.hpp"
#include "duckdb/main/capi/extension_api.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/extension.hpp"
#include "duckdb/main/valid_checker.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/main/extension_manager.hpp"

namespace duckdb {
class BufferManager;
class DatabaseManager;
class StorageManager;
class Catalog;
class TransactionManager;
class ConnectionManager;
class ExtensionManager;
class FileSystem;
class TaskScheduler;
class ObjectCache;
struct AttachInfo;
struct AttachOptions;
class DatabaseFileSystem;
struct DatabaseCacheEntry;
class LogManager;
class ExternalFileCache;

class DatabaseInstance : public enable_shared_from_this<DatabaseInstance> {
	friend class DuckDB;

public:
	DUCKDB_API DatabaseInstance();
	DUCKDB_API ~DatabaseInstance();

	DBConfig config;

public:
	BufferPool &GetBufferPool() const;
	DUCKDB_API SecretManager &GetSecretManager();
	DUCKDB_API BufferManager &GetBufferManager();
	DUCKDB_API const BufferManager &GetBufferManager() const;
	DUCKDB_API DatabaseManager &GetDatabaseManager();
	DUCKDB_API FileSystem &GetFileSystem();
	DUCKDB_API ExternalFileCache &GetExternalFileCache();
	DUCKDB_API TaskScheduler &GetScheduler();
	DUCKDB_API ObjectCache &GetObjectCache();
	DUCKDB_API ConnectionManager &GetConnectionManager();
	DUCKDB_API ExtensionManager &GetExtensionManager();
	DUCKDB_API ValidChecker &GetValidChecker();
	DUCKDB_API LogManager &GetLogManager() const;

	DUCKDB_API const duckdb_ext_api_v1 GetExtensionAPIV1();

	idx_t NumberOfThreads();

	DUCKDB_API static DatabaseInstance &GetDatabase(ClientContext &context);
	DUCKDB_API static const DatabaseInstance &GetDatabase(const ClientContext &context);

	DUCKDB_API bool ExtensionIsLoaded(const string &name);

	DUCKDB_API SettingLookupResult TryGetCurrentSetting(const string &key, Value &result) const;

	DUCKDB_API shared_ptr<EncryptionUtil> GetEncryptionUtil() const;

	unique_ptr<AttachedDatabase> CreateAttachedDatabase(ClientContext &context, AttachInfo &info,
	                                                    AttachOptions &options);

private:
	void Initialize(const char *path, DBConfig *config);
	void LoadExtensionSettings();
	void CreateMainDatabase();

	void Configure(DBConfig &config, const char *path);

private:
	shared_ptr<BufferManager> buffer_manager;
	unique_ptr<DatabaseManager> db_manager;
	unique_ptr<TaskScheduler> scheduler;
	unique_ptr<ObjectCache> object_cache;
	unique_ptr<ConnectionManager> connection_manager;
	unique_ptr<ExtensionManager> extension_manager;
	ValidChecker db_validity;
	unique_ptr<DatabaseFileSystem> db_file_system;
	shared_ptr<LogManager> log_manager;
	unique_ptr<ExternalFileCache> external_file_cache;

	duckdb_ext_api_v1 (*create_api_v1)();
};

//! The database object. This object holds the catalog and all the
//! database-specific meta information.
class DuckDB {
public:
	DUCKDB_API explicit DuckDB(const char *path = nullptr, DBConfig *config = nullptr);
	DUCKDB_API explicit DuckDB(const string &path, DBConfig *config = nullptr);
	DUCKDB_API explicit DuckDB(DatabaseInstance &instance);

	DUCKDB_API ~DuckDB();

	//! Reference to the actual database instance
	shared_ptr<DatabaseInstance> instance;

public:
	// Load a statically loaded extension by its class
	template <class T>
	void LoadStaticExtension() {
		T extension;
		auto &manager = ExtensionManager::Get(*instance);
		auto info = manager.BeginLoad(extension.Name());
		if (!info) {
			// already loaded - return
			return;
		}

		// Instantiate a new loader
		ExtensionLoader loader(*instance, extension.Name());

		// Call the Load method of the extension
		extension.Load(loader);

		// Finalize the loading process
		loader.FinalizeLoad();

		ExtensionInstallInfo install_info;
		install_info.mode = ExtensionInstallMode::STATICALLY_LINKED;
		install_info.version = extension.Version();
		info->FinishLoad(install_info);
	}

	DUCKDB_API FileSystem &GetFileSystem();

	DUCKDB_API idx_t NumberOfThreads();
	DUCKDB_API static const char *SourceID();
	DUCKDB_API static const char *LibraryVersion();
	DUCKDB_API static const char *ReleaseCodename();
	DUCKDB_API static idx_t StandardVectorSize();
	DUCKDB_API static string Platform();
	DUCKDB_API bool ExtensionIsLoaded(const string &name);
};

} // namespace duckdb
