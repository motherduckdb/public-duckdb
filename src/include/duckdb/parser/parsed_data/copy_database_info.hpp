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
	CopyDatabaseInfo(const std::string &to_database)
	: ParseInfo(TYPE), to_database(to_database) {
	}

	std::string to_database;
	vector<unique_ptr<CreateInfo>> entries;
};

} // namespace duckdb
