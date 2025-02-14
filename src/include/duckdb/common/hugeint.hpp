#pragma once

#include "duckdb/common/winapi.hpp"
#include "duckdb/common/string.hpp"
#include <stdint.h>
#include "duckdb/common/typedefs.hpp"

namespace duckdb {

// Forward declaration to allow conversion between hugeint and uhugeint
struct uhugeint_t; // NOLINT: use numeric casing

struct hugeint_t { // NOLINT: use numeric casing
public:
	uint64_t lower;
	int64_t upper;

public:
	hugeint_t() = default;
	DUCKDB_API hugeint_t(int64_t value); // NOLINT: Allow implicit conversion from `int64_t`
	constexpr hugeint_t(int64_t upper, uint64_t lower) : lower(lower), upper(upper) {
	}
	constexpr hugeint_t(const hugeint_t &rhs) = default;
	constexpr hugeint_t(hugeint_t &&rhs) = default;
	hugeint_t &operator=(const hugeint_t &rhs) = default;
	hugeint_t &operator=(hugeint_t &&rhs) = default;

	DUCKDB_API string ToString() const;

	// comparison operators
	DUCKDB_API bool operator==(const hugeint_t &rhs) const;
	DUCKDB_API bool operator!=(const hugeint_t &rhs) const;
	DUCKDB_API bool operator<=(const hugeint_t &rhs) const;
	DUCKDB_API bool operator<(const hugeint_t &rhs) const;
	DUCKDB_API bool operator>(const hugeint_t &rhs) const;
	DUCKDB_API bool operator>=(const hugeint_t &rhs) const;

	// arithmetic operators
	DUCKDB_API hugeint_t operator+(const hugeint_t &rhs) const;
	DUCKDB_API hugeint_t operator-(const hugeint_t &rhs) const;
	DUCKDB_API hugeint_t operator*(const hugeint_t &rhs) const;
	DUCKDB_API hugeint_t operator/(const hugeint_t &rhs) const;
	DUCKDB_API hugeint_t operator%(const hugeint_t &rhs) const;
	DUCKDB_API hugeint_t operator-() const;

	// bitwise operators
	DUCKDB_API hugeint_t operator>>(const hugeint_t &rhs) const;
	DUCKDB_API hugeint_t operator<<(const hugeint_t &rhs) const;
	DUCKDB_API hugeint_t operator&(const hugeint_t &rhs) const;
	DUCKDB_API hugeint_t operator|(const hugeint_t &rhs) const;
	DUCKDB_API hugeint_t operator^(const hugeint_t &rhs) const;
	DUCKDB_API hugeint_t operator~() const;

	// in-place operators
	DUCKDB_API hugeint_t &operator+=(const hugeint_t &rhs);
	DUCKDB_API hugeint_t &operator-=(const hugeint_t &rhs);
	DUCKDB_API hugeint_t &operator*=(const hugeint_t &rhs);
	DUCKDB_API hugeint_t &operator/=(const hugeint_t &rhs);
	DUCKDB_API hugeint_t &operator%=(const hugeint_t &rhs);
	DUCKDB_API hugeint_t &operator>>=(const hugeint_t &rhs);
	DUCKDB_API hugeint_t &operator<<=(const hugeint_t &rhs);
	DUCKDB_API hugeint_t &operator&=(const hugeint_t &rhs);
	DUCKDB_API hugeint_t &operator|=(const hugeint_t &rhs);
	DUCKDB_API hugeint_t &operator^=(const hugeint_t &rhs);

	// boolean operators
	DUCKDB_API explicit operator bool() const;
	DUCKDB_API bool operator!() const;

	// cast operators -- doesn't check bounds/overflow/underflow
	DUCKDB_API explicit operator uint8_t() const;
	DUCKDB_API explicit operator uint16_t() const;
	DUCKDB_API explicit operator uint32_t() const;
	DUCKDB_API explicit operator uint64_t() const;
	DUCKDB_API explicit operator int8_t() const;
	DUCKDB_API explicit operator int16_t() const;
	DUCKDB_API explicit operator int32_t() const;
	DUCKDB_API explicit operator int64_t() const;
	DUCKDB_API operator uhugeint_t() const; // NOLINT: Allow implicit conversion from `hugeint_t`
};

} // namespace duckdb

namespace std {
template <>
struct hash<duckdb::hugeint_t> {
	size_t operator()(const duckdb::hugeint_t &val) const {
		using std::hash;
		return hash<int64_t> {}(val.upper) ^ hash<uint64_t> {}(val.lower);
	}
};
} // namespace std
