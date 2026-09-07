#pragma once

// Shims over the DuckDB vector API so one source tree builds against both the v1.5
// line and v2. v2 split the per-class vector helpers out of vector.hpp, made the
// FlatVector / ListVector read accessors const, added *Mutable variants for writes,
// changed STRUCT child names from string to Identifier, changed
// StructVector::GetEntries to hold Vector by value instead of unique_ptr, and replaced
// the three-argument scalar bind callback with one taking a BindScalarFunctionInput.

#include "duckdb.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"

#if __has_include("duckdb/common/vector/list_vector.hpp")
#define DUCK_GEOARROW_DUCKDB_V2 1
#include "duckdb/common/identifier.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/list_vector.hpp"
#include "duckdb/common/vector/struct_vector.hpp"
#else
#define DUCK_GEOARROW_DUCKDB_V2 0
#endif

namespace duckdb {

// Raw string of a STRUCT field name (child_list_t::first).
inline const string &FieldName(const string &name) {
	return name;
}
#if DUCK_GEOARROW_DUCKDB_V2
inline const string &FieldName(const Identifier &name) {
	return name.GetIdentifierName();
}
#endif

// Writable data pointer of a flat vector.
template <class T>
inline T *MutableData(Vector &vector) {
#if DUCK_GEOARROW_DUCKDB_V2
	return FlatVector::GetDataMutable<T>(vector);
#else
	return FlatVector::GetData<T>(vector);
#endif
}

// Writable validity mask of a flat vector.
inline ValidityMask &MutableValidity(Vector &vector) {
#if DUCK_GEOARROW_DUCKDB_V2
	return FlatVector::ValidityMutable(vector);
#else
	return FlatVector::Validity(vector);
#endif
}

// Child vector of a LIST vector, for reading.
inline const Vector &ListChild(const Vector &vector) {
#if DUCK_GEOARROW_DUCKDB_V2
	return ListVector::GetChild(vector);
#else
	return ListVector::GetEntry(vector);
#endif
}

// Child vector of a LIST vector, for writing.
inline Vector &ListChildMutable(Vector &vector) {
#if DUCK_GEOARROW_DUCKDB_V2
	return ListVector::GetChildMutable(vector);
#else
	return ListVector::GetEntry(vector);
#endif
}

// One entry of StructVector::GetEntries, regardless of whether it is stored by
// value (v2) or behind a unique_ptr (v1.5).
inline Vector &StructEntry(Vector &entry) {
	return entry;
}
inline Vector &StructEntry(unique_ptr<Vector> &entry) {
	return *entry;
}

// --- Scalar function binding ---
//
// Bind callbacks are written once against BindInput; DUCK_GEOARROW_BIND(fn) produces the
// function pointer DuckDB expects for the version being compiled.

#if DUCK_GEOARROW_DUCKDB_V2

using BindInput = BindScalarFunctionInput;
using BoundFunction = BoundScalarFunction;
#define DUCK_GEOARROW_BIND(FN) FN

inline ClientContext &BindContext(BindInput &input) {
	return input.GetClientContext();
}
inline BoundFunction &BindFunction(BindInput &input) {
	return input.GetBoundFunction();
}
inline vector<unique_ptr<Expression>> &BindArguments(BindInput &input) {
	return input.GetArguments();
}
inline const string &BoundName(const BoundFunction &function) {
	return function.GetName().GetIdentifierName();
}
inline const vector<LogicalType> &BoundArgumentTypes(const BoundFunction &function) {
	return function.GetArguments();
}
inline const LogicalType &BoundReturnType(const BoundFunction &function) {
	return function.GetReturnType();
}
inline void SetBoundReturnType(BoundFunction &function, LogicalType type) {
	function.SetReturnType(std::move(type));
}
inline FunctionData &BindData(const BoundFunctionExpression &expr) {
	return *expr.BindInfo();
}
// Function-set names are Identifiers in v2, and Identifier's string constructor is explicit.
using FunctionSetName = Identifier;

#else

struct BindInput {
	ClientContext &context;
	ScalarFunction &bound_function;
	vector<unique_ptr<Expression>> &arguments;
};
using BoundFunction = ScalarFunction;

template <unique_ptr<FunctionData> (*FN)(BindInput &)>
unique_ptr<FunctionData> BindAdapter(ClientContext &context, ScalarFunction &bound_function,
                                     vector<unique_ptr<Expression>> &arguments) {
	BindInput input {context, bound_function, arguments};
	return FN(input);
}
#define DUCK_GEOARROW_BIND(FN) BindAdapter<FN>

inline ClientContext &BindContext(BindInput &input) {
	return input.context;
}
inline BoundFunction &BindFunction(BindInput &input) {
	return input.bound_function;
}
inline vector<unique_ptr<Expression>> &BindArguments(BindInput &input) {
	return input.arguments;
}
inline const string &BoundName(const BoundFunction &function) {
	return function.name;
}
inline const vector<LogicalType> &BoundArgumentTypes(const BoundFunction &function) {
	return function.arguments;
}
inline const LogicalType &BoundReturnType(const BoundFunction &function) {
	return function.return_type;
}
inline void SetBoundReturnType(BoundFunction &function, LogicalType type) {
	function.return_type = std::move(type);
}
inline FunctionData &BindData(const BoundFunctionExpression &expr) {
	return *expr.bind_info;
}
using FunctionSetName = string;

#endif

// --- Deprecated-in-v2 vector / chunk calls ---

#if DUCK_GEOARROW_DUCKDB_V2
inline void ToUnified(const Vector &vector, idx_t, UnifiedVectorFormat &data) {
	vector.ToUnifiedFormat(data);
}
inline void FlattenVector(Vector &vector, idx_t) {
	vector.Flatten();
}
// v2 vectors carry their own size, so the chunk only needs to verify it.
inline void SetChunkCardinality(DataChunk &chunk, idx_t count) {
	chunk.CheckCardinality(count);
}
#else
inline void ToUnified(Vector &vector, idx_t count, UnifiedVectorFormat &data) {
	vector.ToUnifiedFormat(count, data);
}
inline void FlattenVector(Vector &vector, idx_t count) {
	vector.Flatten(count);
}
inline void SetChunkCardinality(DataChunk &chunk, idx_t count) {
	chunk.SetCardinality(count);
}
#endif

} // namespace duckdb
