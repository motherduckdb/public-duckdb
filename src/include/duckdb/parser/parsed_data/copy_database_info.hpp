//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/parser/parsed_data/copy_database_info.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/parser/parsed_data/parse_info.hpp"
#include "duckdb/common/unique_ptr.hpp"
#include "duckdb/common/vector.hpp"

namespace duckdb {

struct CopyDatabaseInfo : public ParseInfo {
public:
	static constexpr const ParseInfoType TYPE = ParseInfoType::COPY_DATABASE_INFO;

public:
	explicit CopyDatabaseInfo() : ParseInfo(TYPE), to_database(INVALID_CATALOG) {
	}

	explicit CopyDatabaseInfo(const std::string &to_database)
	: ParseInfo(TYPE), to_database(to_database) {
	}

	// The destination database to which catalog entries are being copied
	std::string to_database;

	// The catalog entries that are going to be created in the destination DB
	vector<unique_ptr<CreateInfo>> entries;

public:
	void Serialize(Serializer &serializer) const override;
	static unique_ptr<ParseInfo> Deserialize(Deserializer &deserializer);
};

} // namespace duckdb
