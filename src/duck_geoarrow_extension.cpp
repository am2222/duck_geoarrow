#define DUCKDB_EXTENSION_MAIN

#include "duck_geoarrow_extension.hpp"
#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duck_geoarrow_compat.hpp"
#include "duckdb/function/function_set.hpp"
#include "duckdb/function/scalar_function.hpp"
#include <duckdb/parser/parsed_data/create_scalar_function_info.hpp>
#include "duckdb/common/arrow/arrow_converter.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/execution/expression_executor.hpp"

#include "geoarrow/geoarrow.h"

namespace duckdb {

// --- Dimensions (XY / XYZ / XYM / XYZM) ---
//
// GeoArrow encodes dimensions in the coordinate struct's fields: STRUCT(x, y),
// STRUCT(x, y, z), STRUCT(x, y, m) or STRUCT(x, y, z, m). geoarrow-c has one type
// constant per (geometry type, dimensions) pair, built by GeoArrowMakeType(), so all
// dimension handling here reduces to picking the right constant.

// Number of ordinates carried by a set of dimensions.
static idx_t DimensionCount(enum GeoArrowDimensions dims) {
	switch (dims) {
	case GEOARROW_DIMENSIONS_XY:
		return 2;
	case GEOARROW_DIMENSIONS_XYZ:
	case GEOARROW_DIMENSIONS_XYM:
		return 3;
	case GEOARROW_DIMENSIONS_XYZM:
		return 4;
	default:
		return 0;
	}
}

// The GeoArrow coordinate struct for a set of dimensions.
static LogicalType CoordStructType(enum GeoArrowDimensions dims) {
	child_list_t<LogicalType> fields;
	fields.emplace_back("x", LogicalType::DOUBLE);
	fields.emplace_back("y", LogicalType::DOUBLE);
	if (dims == GEOARROW_DIMENSIONS_XYZ || dims == GEOARROW_DIMENSIONS_XYZM) {
		fields.emplace_back("z", LogicalType::DOUBLE);
	}
	if (dims == GEOARROW_DIMENSIONS_XYM || dims == GEOARROW_DIMENSIONS_XYZM) {
		fields.emplace_back("m", LogicalType::DOUBLE);
	}
	return LogicalType::STRUCT(std::move(fields));
}

// Dimensions implied by a coordinate struct's field names, or UNKNOWN if it is not one.
static enum GeoArrowDimensions DimensionsFromCoordStruct(const LogicalType &type) {
	if (type.id() != LogicalTypeId::STRUCT) {
		return GEOARROW_DIMENSIONS_UNKNOWN;
	}
	string names;
	for (auto &child : StructType::GetChildTypes(type)) {
		if (child.second.id() != LogicalTypeId::DOUBLE) {
			return GEOARROW_DIMENSIONS_UNKNOWN;
		}
		names += StringUtil::Lower(FieldName(child.first));
	}
	if (names == "xy") {
		return GEOARROW_DIMENSIONS_XY;
	}
	if (names == "xyz") {
		return GEOARROW_DIMENSIONS_XYZ;
	}
	if (names == "xym") {
		return GEOARROW_DIMENSIONS_XYM;
	}
	if (names == "xyzm") {
		return GEOARROW_DIMENSIONS_XYZM;
	}
	return GEOARROW_DIMENSIONS_UNKNOWN;
}

// How many levels of LIST wrap the coordinate struct in a native encoding.
static int NativeNestingDepth(enum GeoArrowGeometryType geometry_type) {
	switch (geometry_type) {
	case GEOARROW_GEOMETRY_TYPE_POINT:
		return 0;
	case GEOARROW_GEOMETRY_TYPE_LINESTRING:
	case GEOARROW_GEOMETRY_TYPE_MULTIPOINT:
		return 1;
	case GEOARROW_GEOMETRY_TYPE_POLYGON:
	case GEOARROW_GEOMETRY_TYPE_MULTILINESTRING:
		return 2;
	case GEOARROW_GEOMETRY_TYPE_MULTIPOLYGON:
		return 3;
	default:
		return -1;
	}
}

// The DuckDB type of a GeoArrow native encoding.
static LogicalType NativeType(enum GeoArrowGeometryType geometry_type, enum GeoArrowDimensions dims) {
	auto type = CoordStructType(dims);
	for (int i = 0; i < NativeNestingDepth(geometry_type); i++) {
		type = LogicalType::LIST(type);
	}
	return type;
}

// Peel LIST levels off a native encoding, reporting the depth and the coordinate struct's
// dimensions. Returns false if `type` is not a nesting of a GeoArrow coordinate struct.
static bool InspectNativeType(const LogicalType &type, int &depth, enum GeoArrowDimensions &dims) {
	depth = 0;
	auto current = type;
	while (current.id() == LogicalTypeId::LIST) {
		current = ListType::GetChildType(current);
		depth++;
		if (depth > 3) {
			return false;
		}
	}
	dims = DimensionsFromCoordStruct(current);
	return dims != GEOARROW_DIMENSIONS_UNKNOWN;
}

// Parse a geometry type name, optionally carrying a dimension suffix: "polygon",
// "polygon_z", "POLYGON ZM" and "multipointm" all parse. No GeoArrow geometry type name
// ends in 'z' or 'm', so stripping a suffix is unambiguous. `dims` is left UNKNOWN when
// the name carries no suffix, in which case the caller infers it from the value's type.
static bool ParseGeometryTypeName(const string &name, enum GeoArrowGeometryType &geometry_type,
                                  enum GeoArrowDimensions &dims) {
	string norm;
	for (auto c : StringUtil::Lower(name)) {
		if (c != ' ' && c != '_' && c != '-') {
			norm += c;
		}
	}

	dims = GEOARROW_DIMENSIONS_UNKNOWN;
	if (StringUtil::EndsWith(norm, "zm")) {
		dims = GEOARROW_DIMENSIONS_XYZM;
		norm = norm.substr(0, norm.size() - 2);
	} else if (StringUtil::EndsWith(norm, "z")) {
		dims = GEOARROW_DIMENSIONS_XYZ;
		norm = norm.substr(0, norm.size() - 1);
	} else if (StringUtil::EndsWith(norm, "m")) {
		dims = GEOARROW_DIMENSIONS_XYM;
		norm = norm.substr(0, norm.size() - 1);
	}

	if (norm == "point") {
		geometry_type = GEOARROW_GEOMETRY_TYPE_POINT;
	} else if (norm == "linestring") {
		geometry_type = GEOARROW_GEOMETRY_TYPE_LINESTRING;
	} else if (norm == "polygon") {
		geometry_type = GEOARROW_GEOMETRY_TYPE_POLYGON;
	} else if (norm == "multipoint") {
		geometry_type = GEOARROW_GEOMETRY_TYPE_MULTIPOINT;
	} else if (norm == "multilinestring") {
		geometry_type = GEOARROW_GEOMETRY_TYPE_MULTILINESTRING;
	} else if (norm == "multipolygon") {
		geometry_type = GEOARROW_GEOMETRY_TYPE_MULTIPOLYGON;
	} else {
		return false;
	}
	return true;
}

// Parse a dimension name: "xy", "xyz", "z", "zm", "m".
static bool ParseDimensionName(const string &name, enum GeoArrowDimensions &dims) {
	string norm;
	for (auto c : StringUtil::Lower(name)) {
		if (c != ' ' && c != '_' && c != '-') {
			norm += c;
		}
	}
	if (norm == "xy") {
		dims = GEOARROW_DIMENSIONS_XY;
	} else if (norm == "xyz" || norm == "z") {
		dims = GEOARROW_DIMENSIONS_XYZ;
	} else if (norm == "xym" || norm == "m") {
		dims = GEOARROW_DIMENSIONS_XYM;
	} else if (norm == "xyzm" || norm == "zm") {
		dims = GEOARROW_DIMENSIONS_XYZM;
	} else {
		return false;
	}
	return true;
}

static const char *DimensionSuffix(enum GeoArrowDimensions dims) {
	switch (dims) {
	case GEOARROW_DIMENSIONS_XYZ:
		return " Z";
	case GEOARROW_DIMENSIONS_XYM:
		return " M";
	case GEOARROW_DIMENSIONS_XYZM:
		return " ZM";
	default:
		return "";
	}
}

// The flat GeoArrow struct used as output for st_asgeoarrow and input for
// st_geomfromgeoarrow. `zs` / `ms` are present only for the dimensions that need them, so
// the XY type is byte-identical to the one this extension has always produced.
static LogicalType GeoArrowStructType(enum GeoArrowDimensions dims = GEOARROW_DIMENSIONS_XY) {
	auto double_list = LogicalType::LIST(LogicalType::DOUBLE);
	child_list_t<LogicalType> fields;
	fields.emplace_back("geometry_type", LogicalType::UTINYINT);
	fields.emplace_back("xs", double_list);
	fields.emplace_back("ys", double_list);
	if (dims == GEOARROW_DIMENSIONS_XYZ || dims == GEOARROW_DIMENSIONS_XYZM) {
		fields.emplace_back("zs", double_list);
	}
	if (dims == GEOARROW_DIMENSIONS_XYM || dims == GEOARROW_DIMENSIONS_XYZM) {
		fields.emplace_back("ms", double_list);
	}
	fields.emplace_back("ring_offsets", LogicalType::LIST(LogicalType::INTEGER));
	fields.emplace_back("geom_offsets", LogicalType::LIST(LogicalType::INTEGER));
	return LogicalType::STRUCT(std::move(fields));
}

// Dimensions implied by a flat GeoArrow struct, or UNKNOWN if it is not one.
static enum GeoArrowDimensions DimensionsFromFlatStruct(const LogicalType &type) {
	if (type.id() != LogicalTypeId::STRUCT) {
		return GEOARROW_DIMENSIONS_UNKNOWN;
	}
	bool has_z = false;
	bool has_m = false;
	for (auto &child : StructType::GetChildTypes(type)) {
		auto name = StringUtil::Lower(FieldName(child.first));
		has_z = has_z || name == "zs";
		has_m = has_m || name == "ms";
	}
	if (has_z && has_m) {
		return GEOARROW_DIMENSIONS_XYZM;
	}
	if (has_z) {
		return GEOARROW_DIMENSIONS_XYZ;
	}
	if (has_m) {
		return GEOARROW_DIMENSIONS_XYM;
	}
	return GEOARROW_DIMENSIONS_XY;
}

// --- Coordinate extraction visitor (WKB → struct) ---

struct CoordExtractor {
	vector<double> xs;
	vector<double> ys;
	vector<double> zs;
	vector<double> ms;
	vector<int32_t> ring_offsets;
	vector<int32_t> geom_offsets;
	uint8_t geometry_type = 0;
	// Dimensions reported by the outermost geom_start, i.e. what the WKB actually carries
	enum GeoArrowDimensions dims = GEOARROW_DIMENSIONS_UNKNOWN;
	int depth = 0;
};

static int ExtractFeatStart(struct GeoArrowVisitor *v) {
	auto *ext = static_cast<CoordExtractor *>(v->private_data);
	ext->xs.clear();
	ext->ys.clear();
	ext->zs.clear();
	ext->ms.clear();
	ext->ring_offsets.clear();
	ext->geom_offsets.clear();
	ext->depth = 0;
	ext->geometry_type = 0;
	ext->dims = GEOARROW_DIMENSIONS_UNKNOWN;
	return GEOARROW_OK;
}

static int ExtractGeomStart(struct GeoArrowVisitor *v, enum GeoArrowGeometryType type, enum GeoArrowDimensions dims) {
	auto *ext = static_cast<CoordExtractor *>(v->private_data);
	if (ext->depth == 0) {
		ext->geometry_type = static_cast<uint8_t>(type);
		ext->dims = dims;
	}
	ext->depth++;
	return GEOARROW_OK;
}

static int ExtractRingStart(struct GeoArrowVisitor *v) {
	(void)v;
	return GEOARROW_OK;
}

static int ExtractCoords(struct GeoArrowVisitor *v, const struct GeoArrowCoordView *coords) {
	auto *ext = static_cast<CoordExtractor *>(v->private_data);
	// In an XYM coordinate view the third ordinate is m, not z
	bool third_is_m = ext->dims == GEOARROW_DIMENSIONS_XYM;
	for (int64_t i = 0; i < coords->n_coords; i++) {
		ext->xs.push_back(GEOARROW_COORD_VIEW_VALUE(coords, i, 0));
		ext->ys.push_back(GEOARROW_COORD_VIEW_VALUE(coords, i, 1));
		if (coords->n_values > 2) {
			auto third = GEOARROW_COORD_VIEW_VALUE(coords, i, 2);
			if (third_is_m) {
				ext->ms.push_back(third);
			} else {
				ext->zs.push_back(third);
			}
		}
		if (coords->n_values > 3) {
			ext->ms.push_back(GEOARROW_COORD_VIEW_VALUE(coords, i, 3));
		}
	}
	return GEOARROW_OK;
}

static int ExtractRingEnd(struct GeoArrowVisitor *v) {
	auto *ext = static_cast<CoordExtractor *>(v->private_data);
	ext->ring_offsets.push_back(static_cast<int32_t>(ext->xs.size()));
	return GEOARROW_OK;
}

static int ExtractGeomEnd(struct GeoArrowVisitor *v) {
	auto *ext = static_cast<CoordExtractor *>(v->private_data);
	ext->depth--;
	if (ext->depth == 1) {
		auto root_type = static_cast<enum GeoArrowGeometryType>(ext->geometry_type);
		if (root_type == GEOARROW_GEOMETRY_TYPE_MULTIPOINT || root_type == GEOARROW_GEOMETRY_TYPE_MULTILINESTRING) {
			ext->ring_offsets.push_back(static_cast<int32_t>(ext->xs.size()));
		} else if (root_type == GEOARROW_GEOMETRY_TYPE_MULTIPOLYGON) {
			ext->geom_offsets.push_back(static_cast<int32_t>(ext->ring_offsets.size()));
		}
	}
	return GEOARROW_OK;
}

static int ExtractFeatEnd(struct GeoArrowVisitor *v) {
	(void)v;
	return GEOARROW_OK;
}

static int ExtractNullFeat(struct GeoArrowVisitor *v) {
	(void)v;
	return GEOARROW_OK;
}

// Helper: populate a DuckDB LIST(DOUBLE) vector entry
static void SetDoubleList(Vector &list_vec, idx_t row, const vector<double> &values) {
	auto current_size = ListVector::GetListSize(list_vec);
	auto new_size = current_size + values.size();
	ListVector::Reserve(list_vec, new_size);

	auto &child = ListChildMutable(list_vec);
	auto child_data = MutableData<double>(child);
	for (idx_t j = 0; j < values.size(); j++) {
		child_data[current_size + j] = values[j];
	}

	auto list_data = MutableData<list_entry_t>(list_vec);
	list_data[row].offset = current_size;
	list_data[row].length = values.size();
	ListVector::SetListSize(list_vec, new_size);
}

// Helper: populate a DuckDB LIST(INTEGER) vector entry
static void SetIntList(Vector &list_vec, idx_t row, const vector<int32_t> &values) {
	auto current_size = ListVector::GetListSize(list_vec);
	auto new_size = current_size + values.size();
	ListVector::Reserve(list_vec, new_size);

	auto &child = ListChildMutable(list_vec);
	auto child_data = MutableData<int32_t>(child);
	for (idx_t j = 0; j < values.size(); j++) {
		child_data[current_size + j] = values[j];
	}

	auto list_data = MutableData<list_entry_t>(list_vec);
	list_data[row].offset = current_size;
	list_data[row].length = values.size();
	ListVector::SetListSize(list_vec, new_size);
}

// Helper: set up the extraction visitor callbacks
static void InitExtractVisitor(struct GeoArrowVisitor &visitor, CoordExtractor &extractor) {
	memset(&visitor, 0, sizeof(visitor));
	visitor.feat_start = ExtractFeatStart;
	visitor.null_feat = ExtractNullFeat;
	visitor.geom_start = ExtractGeomStart;
	visitor.ring_start = ExtractRingStart;
	visitor.coords = ExtractCoords;
	visitor.ring_end = ExtractRingEnd;
	visitor.geom_end = ExtractGeomEnd;
	visitor.feat_end = ExtractFeatEnd;
	visitor.private_data = &extractor;
}

// Bind data for the write side: which dimensions the output carries.
struct DimensionsBindData : public FunctionData {
	explicit DimensionsBindData(enum GeoArrowDimensions dims_p) : dims(dims_p) {
	}

	enum GeoArrowDimensions dims;

	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<DimensionsBindData>(dims);
	}
	bool Equals(const FunctionData &other_p) const override {
		return dims == other_p.Cast<DimensionsBindData>().dims;
	}
};

// Read the optional constant dimensions argument, defaulting to XY.
static enum GeoArrowDimensions BindDimensionsArgument(BindInput &input, idx_t arg_index) {
	auto &arguments = BindArguments(input);
	auto &fn_name = BoundName(BindFunction(input));
	if (arguments.size() <= arg_index) {
		return GEOARROW_DIMENSIONS_XY;
	}
	if (!arguments[arg_index]->IsFoldable()) {
		throw BinderException(fn_name + ": the dimensions must be a constant string");
	}
	auto value = ExpressionExecutor::EvaluateScalar(BindContext(input), *arguments[arg_index]);
	if (value.IsNull()) {
		throw BinderException(fn_name + ": the dimensions must not be NULL");
	}
	auto name = value.ToString();
	enum GeoArrowDimensions dims;
	if (!ParseDimensionName(name, dims)) {
		throw BinderException(fn_name + ": unknown dimensions '" + name + "' (expected xy, xyz, xym or xyzm)");
	}
	return dims;
}

// Verify a feature actually carries the ordinates the caller asked for. Projecting XYZ down
// to XY is fine; inventing a z for XY data is not.
static void CheckFeatureDimensions(const CoordExtractor &extractor, enum GeoArrowDimensions wanted,
                                   const char *fn_name) {
	bool has_z = extractor.dims == GEOARROW_DIMENSIONS_XYZ || extractor.dims == GEOARROW_DIMENSIONS_XYZM;
	bool has_m = extractor.dims == GEOARROW_DIMENSIONS_XYM || extractor.dims == GEOARROW_DIMENSIONS_XYZM;
	bool want_z = wanted == GEOARROW_DIMENSIONS_XYZ || wanted == GEOARROW_DIMENSIONS_XYZM;
	bool want_m = wanted == GEOARROW_DIMENSIONS_XYM || wanted == GEOARROW_DIMENSIONS_XYZM;
	if ((want_z && !has_z) || (want_m && !has_m)) {
		throw InvalidInputException(string(fn_name) + ": geometry is " +
		                            string(GeoArrowDimensionsString(extractor.dims)) + " but " +
		                            string(GeoArrowDimensionsString(wanted)) + " was requested");
	}
}

// Locate a named child of a STRUCT vector, or nullptr if it has no such field.
static optional_ptr<Vector> StructChild(Vector &struct_vec, const char *name) {
	auto &child_types = StructType::GetChildTypes(struct_vec.GetType());
	auto &children = StructVector::GetEntries(struct_vec);
	for (idx_t c = 0; c < child_types.size(); c++) {
		if (StringUtil::Lower(FieldName(child_types[c].first)) == name) {
			return &StructEntry(children[c]);
		}
	}
	return nullptr;
}

// Helper: write extractor results into the output STRUCT vectors for row i
static void WriteExtractorOutput(CoordExtractor &extractor, Vector &result, idx_t i) {
	MutableData<uint8_t>(*StructChild(result, "geometry_type"))[i] = extractor.geometry_type;
	SetDoubleList(*StructChild(result, "xs"), i, extractor.xs);
	SetDoubleList(*StructChild(result, "ys"), i, extractor.ys);
	if (auto zs = StructChild(result, "zs")) {
		SetDoubleList(*zs, i, extractor.zs);
	}
	if (auto ms = StructChild(result, "ms")) {
		SetDoubleList(*ms, i, extractor.ms);
	}
	SetIntList(*StructChild(result, "ring_offsets"), i, extractor.ring_offsets);
	SetIntList(*StructChild(result, "geom_offsets"), i, extractor.geom_offsets);
}

// Helper: write NULL into the output STRUCT vectors for row i
static void WriteNullOutput(Vector &result, idx_t i) {
	MutableValidity(result).SetInvalid(i);
	MutableData<uint8_t>(*StructChild(result, "geometry_type"))[i] = 0;
	SetDoubleList(*StructChild(result, "xs"), i, {});
	SetDoubleList(*StructChild(result, "ys"), i, {});
	if (auto zs = StructChild(result, "zs")) {
		SetDoubleList(*zs, i, {});
	}
	if (auto ms = StructChild(result, "ms")) {
		SetDoubleList(*ms, i, {});
	}
	SetIntList(*StructChild(result, "ring_offsets"), i, {});
	SetIntList(*StructChild(result, "geom_offsets"), i, {});
}

// --- st_asgeoarrow: WKB (BLOB/GEOMETRY) → GeoArrow STRUCT ---

// st_asgeoarrow(geom[, dimensions]): the output struct gains zs / ms fields when the
// caller asks for them, so the one-argument form keeps its original XY type.
static unique_ptr<FunctionData> StAsGeoArrowBind(BindInput &input) {
	auto dims = BindDimensionsArgument(input, 1);
	SetBoundReturnType(BindFunction(input), GeoArrowStructType(dims));
	return make_uniq<DimensionsBindData>(dims);
}

static void StAsGeoArrowWKBFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto count = args.size();

	auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
	auto wanted_dims = BindData(func_expr).Cast<DimensionsBindData>().dims;

	UnifiedVectorFormat input_data;
	ToUnified(args.data[0], count, input_data);
	auto input_entries = UnifiedVectorFormat::GetData<string_t>(input_data);

	struct GeoArrowWKBReader wkb_reader;
	GeoArrowWKBReaderInit(&wkb_reader);

	CoordExtractor extractor;
	struct GeoArrowVisitor visitor;
	InitExtractVisitor(visitor, extractor);

	struct GeoArrowError ga_error;

	for (idx_t i = 0; i < count; i++) {
		auto input_idx = input_data.sel->get_index(i);
		if (!input_data.validity.RowIsValid(input_idx)) {
			WriteNullOutput(result, i);
			continue;
		}

		auto wkb_blob = input_entries[input_idx];

		struct GeoArrowBufferView wkb_buf;
		wkb_buf.data = reinterpret_cast<const uint8_t *>(wkb_blob.GetData());
		wkb_buf.size_bytes = static_cast<int64_t>(wkb_blob.GetSize());

		memset(&ga_error, 0, sizeof(ga_error));
		visitor.error = &ga_error;

		visitor.feat_start(&visitor);
		int rc = GeoArrowWKBReaderVisit(&wkb_reader, wkb_buf, &visitor);
		if (rc != GEOARROW_OK) {
			GeoArrowWKBReaderReset(&wkb_reader);
			throw InvalidInputException("st_asgeoarrow: invalid WKB - " + string(ga_error.message));
		}
		visitor.feat_end(&visitor);

		CheckFeatureDimensions(extractor, wanted_dims, "st_asgeoarrow");
		WriteExtractorOutput(extractor, result, i);
	}

	GeoArrowWKBReaderReset(&wkb_reader);
}

// --- Shared plumbing for GeoArrow → GEOMETRY conversions ---

// RAII: a GeoArrowWKBWriter that is always reset, even if we throw mid-way.
struct WKBWriterGuard {
	struct GeoArrowWKBWriter writer;
	WKBWriterGuard() {
		GeoArrowWKBWriterInit(&writer);
	}
	~WKBWriterGuard() {
		GeoArrowWKBWriterReset(&writer);
	}
};

// RAII: an ArrowArray that is always released.
struct ArrowArrayGuard {
	struct ArrowArray array;
	ArrowArrayGuard() {
		memset(&array, 0, sizeof(array));
	}
	~ArrowArrayGuard() {
		if (array.release) {
			array.release(&array);
		}
	}
};

// Copy a finished WKB writer array (one feature per row) into a GEOMETRY vector.
static void WKBArrayToVector(const struct ArrowArray &wkb, Vector &result, idx_t count, const char *fn_name) {
	if (wkb.length != static_cast<int64_t>(count)) {
		throw InternalException(string(fn_name) + ": WKB writer produced " + to_string(wkb.length) + " features for " +
		                        to_string(count) + " rows");
	}
	auto validity = static_cast<const uint8_t *>(wkb.buffers[0]);
	auto offsets = static_cast<const int32_t *>(wkb.buffers[1]);
	auto data = static_cast<const char *>(wkb.buffers[2]);

	auto out = MutableData<string_t>(result);
	auto &result_validity = MutableValidity(result);
	for (idx_t i = 0; i < count; i++) {
		// Arrow validity bitmaps are LSB-first
		if (validity && !((validity[i / 8] >> (i % 8)) & 1)) {
			result_validity.SetInvalid(i);
			continue;
		}
		auto start = offsets[i];
		out[i] = StringVector::AddStringOrBlob(result, data + start, static_cast<idx_t>(offsets[i + 1] - start));
	}
}

// Export one DuckDB vector as an Arrow array whose layout geoarrow-c can read.
static void ExportVectorToArrow(Vector &input, idx_t count, ArrowArrayGuard &out) {
	DataChunk arrow_input;
	arrow_input.InitializeEmpty({input.GetType()});
	arrow_input.data[0].Reference(input);
	SetChunkCardinality(arrow_input, count);

	// Default ClientProperties are exactly what geoarrow-c requires: 32-bit offsets
	// (ArrowOffsetSize::REGULAR) and arrow_use_list_view = false. Deliberately NOT taken
	// from the session - `SET arrow_output_list_view = true` would emit 3-buffer list-view
	// arrays that GeoArrowArrayViewSetArray rejects.
	ClientProperties options;
	ArrowConverter::ToArrowArray(arrow_input, &out.array, options, {});
}

// --- st_geomfromgeoarrow: GeoArrow STRUCT → WKB BLOB ---

// A view of one feature's separated coordinate arrays.
struct CoordArrays {
	const double *xs = nullptr;
	const double *ys = nullptr;
	const double *zs = nullptr;
	const double *ms = nullptr;
	idx_t count = 0;
	enum GeoArrowDimensions dims = GEOARROW_DIMENSIONS_XY;
};

// Build a GeoArrowCoordView over `n` coordinates starting at `offset`. geoarrow orders the
// ordinates x, y, [z], [m] - for XYM the third slot holds m.
static struct GeoArrowCoordView MakeCoordView(const CoordArrays &coords, idx_t offset, idx_t n) {
	struct GeoArrowCoordView cv;
	memset(&cv, 0, sizeof(cv));
	cv.values[0] = coords.xs + offset;
	cv.values[1] = coords.ys + offset;
	int32_t n_values = 2;
	if (coords.dims == GEOARROW_DIMENSIONS_XYZ || coords.dims == GEOARROW_DIMENSIONS_XYZM) {
		cv.values[n_values++] = coords.zs + offset;
	}
	if (coords.dims == GEOARROW_DIMENSIONS_XYM || coords.dims == GEOARROW_DIMENSIONS_XYZM) {
		cv.values[n_values++] = coords.ms + offset;
	}
	cv.n_coords = static_cast<int64_t>(n);
	cv.n_values = n_values;
	cv.coords_stride = 1;
	return cv;
}

// Drive visitor callbacks to produce WKB from extracted coordinate data
static void DriveVisitor(struct GeoArrowVisitor *v, uint8_t geom_type, const CoordArrays &coords,
                         const int32_t *ring_offs, idx_t num_ring_offs, const int32_t *geom_offs, idx_t num_geom_offs) {
	auto gt = static_cast<enum GeoArrowGeometryType>(geom_type);
	auto dims = coords.dims;

	switch (gt) {
	case GEOARROW_GEOMETRY_TYPE_POINT:
	case GEOARROW_GEOMETRY_TYPE_LINESTRING: {
		v->geom_start(v, gt, dims);
		auto cv = MakeCoordView(coords, 0, coords.count);
		v->coords(v, &cv);
		v->geom_end(v);
		break;
	}
	case GEOARROW_GEOMETRY_TYPE_POLYGON: {
		v->geom_start(v, gt, dims);
		int32_t prev = 0;
		for (idx_t r = 0; r < num_ring_offs; r++) {
			v->ring_start(v);
			int32_t ring_end = ring_offs[r];
			auto cv = MakeCoordView(coords, static_cast<idx_t>(prev), static_cast<idx_t>(ring_end - prev));
			v->coords(v, &cv);
			v->ring_end(v);
			prev = ring_end;
		}
		v->geom_end(v);
		break;
	}
	case GEOARROW_GEOMETRY_TYPE_MULTIPOINT:
	case GEOARROW_GEOMETRY_TYPE_MULTILINESTRING: {
		auto part_type =
		    gt == GEOARROW_GEOMETRY_TYPE_MULTIPOINT ? GEOARROW_GEOMETRY_TYPE_POINT : GEOARROW_GEOMETRY_TYPE_LINESTRING;
		v->geom_start(v, gt, dims);
		int32_t prev = 0;
		for (idx_t r = 0; r < num_ring_offs; r++) {
			v->geom_start(v, part_type, dims);
			int32_t seg_end = ring_offs[r];
			auto cv = MakeCoordView(coords, static_cast<idx_t>(prev), static_cast<idx_t>(seg_end - prev));
			v->coords(v, &cv);
			v->geom_end(v);
			prev = seg_end;
		}
		v->geom_end(v);
		break;
	}
	case GEOARROW_GEOMETRY_TYPE_MULTIPOLYGON: {
		v->geom_start(v, gt, dims);
		int32_t coord_prev = 0;
		int32_t ring_prev = 0;
		for (idx_t g = 0; g < num_geom_offs; g++) {
			v->geom_start(v, GEOARROW_GEOMETRY_TYPE_POLYGON, dims);
			int32_t ring_end_idx = geom_offs[g];
			for (int32_t r = ring_prev; r < ring_end_idx; r++) {
				v->ring_start(v);
				int32_t coord_end = ring_offs[r];
				auto cv =
				    MakeCoordView(coords, static_cast<idx_t>(coord_prev), static_cast<idx_t>(coord_end - coord_prev));
				v->coords(v, &cv);
				v->ring_end(v);
				coord_prev = coord_end;
			}
			v->geom_end(v);
			ring_prev = ring_end_idx;
		}
		v->geom_end(v);
		break;
	}
	default:
		throw InvalidInputException("st_geomfromgeoarrow: unsupported geometry type %d", geom_type);
	}
}

// Read a LIST(DOUBLE) child of the flat struct for one row.
static const double *FlatDoubleList(Vector &list_vec, idx_t row, idx_t &length) {
	auto entry = FlatVector::GetData<list_entry_t>(list_vec)[row];
	length = entry.length;
	return FlatVector::GetData<double>(ListChild(list_vec)) + entry.offset;
}

static void StGeomFromGeoArrowFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto count = args.size();
	if (count == 0) {
		return;
	}

	// Flatten so the STRUCT children are addressable by row index: a constant or
	// dictionary-encoded input has children shorter than `count`.
	auto &input = args.data[0];
	FlattenVector(input, count);

	auto dims = DimensionsFromFlatStruct(input.GetType());
	auto &child_names = StructType::GetChildTypes(input.GetType());
	auto &in_children = StructVector::GetEntries(input);

	// Locate children by name so field order does not matter
	optional_ptr<Vector> type_vec, xs_vec, ys_vec, zs_vec, ms_vec, ring_off_vec, geom_off_vec;
	for (idx_t c = 0; c < child_names.size(); c++) {
		auto name = StringUtil::Lower(FieldName(child_names[c].first));
		auto &vec = StructEntry(in_children[c]);
		if (name == "geometry_type") {
			type_vec = &vec;
		} else if (name == "xs") {
			xs_vec = &vec;
		} else if (name == "ys") {
			ys_vec = &vec;
		} else if (name == "zs") {
			zs_vec = &vec;
		} else if (name == "ms") {
			ms_vec = &vec;
		} else if (name == "ring_offsets") {
			ring_off_vec = &vec;
		} else if (name == "geom_offsets") {
			geom_off_vec = &vec;
		}
	}
	if (!type_vec || !xs_vec || !ys_vec || !ring_off_vec || !geom_off_vec) {
		throw InvalidInputException("st_geomfromgeoarrow: input struct must have geometry_type, xs, ys, "
		                            "ring_offsets and geom_offsets fields");
	}
	auto &input_validity = FlatVector::Validity(input);

	// One writer for the whole chunk; null rows become null features.
	WKBWriterGuard writer;
	struct GeoArrowVisitor visitor;
	GeoArrowWKBWriterInitVisitor(&writer.writer, &visitor);

	struct GeoArrowError ga_error;
	memset(&ga_error, 0, sizeof(ga_error));
	visitor.error = &ga_error;

	for (idx_t i = 0; i < count; i++) {
		visitor.feat_start(&visitor);
		if (!input_validity.RowIsValid(i)) {
			visitor.null_feat(&visitor);
			visitor.feat_end(&visitor);
			continue;
		}

		uint8_t geom_type = FlatVector::GetData<uint8_t>(*type_vec)[i];

		CoordArrays coords;
		coords.dims = dims;
		idx_t ys_len = 0;
		coords.xs = FlatDoubleList(*xs_vec, i, coords.count);
		coords.ys = FlatDoubleList(*ys_vec, i, ys_len);
		if (zs_vec) {
			idx_t zs_len = 0;
			coords.zs = FlatDoubleList(*zs_vec, i, zs_len);
			if (zs_len < coords.count) {
				throw InvalidInputException("st_geomfromgeoarrow: zs has %llu values but xs has %llu",
				                            static_cast<uint64_t>(zs_len), static_cast<uint64_t>(coords.count));
			}
		}
		if (ms_vec) {
			idx_t ms_len = 0;
			coords.ms = FlatDoubleList(*ms_vec, i, ms_len);
			if (ms_len < coords.count) {
				throw InvalidInputException("st_geomfromgeoarrow: ms has %llu values but xs has %llu",
				                            static_cast<uint64_t>(ms_len), static_cast<uint64_t>(coords.count));
			}
		}
		if (ys_len < coords.count) {
			throw InvalidInputException("st_geomfromgeoarrow: ys has %llu values but xs has %llu",
			                            static_cast<uint64_t>(ys_len), static_cast<uint64_t>(coords.count));
		}

		auto ro_entry = FlatVector::GetData<list_entry_t>(*ring_off_vec)[i];
		auto ro_data = FlatVector::GetData<int32_t>(ListChild(*ring_off_vec)) + ro_entry.offset;

		auto go_entry = FlatVector::GetData<list_entry_t>(*geom_off_vec)[i];
		auto go_data = FlatVector::GetData<int32_t>(ListChild(*geom_off_vec)) + go_entry.offset;

		DriveVisitor(&visitor, geom_type, coords, ro_data, ro_entry.length, go_data, go_entry.length);
		visitor.feat_end(&visitor);
	}

	ArrowArrayGuard wkb;
	if (GeoArrowWKBWriterFinish(&writer.writer, &wkb.array, &ga_error) != GEOARROW_OK) {
		throw InvalidInputException("st_geomfromgeoarrow: WKB writer failed - " + string(ga_error.message));
	}

	WKBArrayToVector(wkb.array, result, count, "st_geomfromgeoarrow");
}

// --- GeoArrow native encoding -> GEOMETRY ---
//
// Thin wrappers around geoarrow-c. DuckDB exports the input vector through its own Arrow
// bridge, then GeoArrowArrayViewVisitNative walks it and the WKB writer serializes it.
// Nesting, offsets, null handling and dimension logic all live in the library; the only
// code here is the two conversions at the edges.
//
// Two entry points share one implementation:
//   st_geomfromgeoarrow<type>(value)         - one name per geometry type
//   st_geomfromgeoarrow('<type>', value)     - generic, type given as a string
// Both resolve to a single GeoArrowType at bind time. The generic form exists because the
// DuckDB types collide: LineString and MultiPoint are both LIST(STRUCT(x, y)), as are
// Polygon and MultiLineString, so a value alone cannot say which it is. Dimensions are
// inferred from the coordinate struct's fields, so Z/M/ZM need no extra argument.

struct NativeReadBindData : public FunctionData {
	explicit NativeReadBindData(enum GeoArrowType ga_type_p, string fn_name_p)
	    : ga_type(ga_type_p), fn_name(std::move(fn_name_p)) {
	}

	enum GeoArrowType ga_type;
	string fn_name;

	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<NativeReadBindData>(ga_type, fn_name);
	}
	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<NativeReadBindData>();
		return ga_type == other.ga_type && fn_name == other.fn_name;
	}
};

// Resolve (geometry type, value type) to a native GeoArrowType, validating that the
// value's nesting depth matches the geometry type.
static enum GeoArrowType ResolveNativeType(enum GeoArrowGeometryType geometry_type, const LogicalType &value_type,
                                           enum GeoArrowDimensions expected_dims, const string &fn_name) {
	int depth = 0;
	enum GeoArrowDimensions dims = GEOARROW_DIMENSIONS_UNKNOWN;
	if (!InspectNativeType(value_type, depth, dims)) {
		throw BinderException(fn_name + ": " + value_type.ToString() +
		                      " is not a GeoArrow native encoding (expected a nesting of "
		                      "STRUCT(x, y), STRUCT(x, y, z), STRUCT(x, y, m) or STRUCT(x, y, z, m))");
	}
	auto want_depth = NativeNestingDepth(geometry_type);
	if (depth != want_depth) {
		throw BinderException(fn_name + ": " + string(GeoArrowGeometryTypeString(geometry_type)) + " expects " +
		                      NativeType(geometry_type, dims).ToString() + ", got " + value_type.ToString());
	}
	if (expected_dims != GEOARROW_DIMENSIONS_UNKNOWN && expected_dims != dims) {
		throw BinderException(fn_name + ": type name says " + string(GeoArrowDimensionsString(expected_dims)) +
		                      " but the value is " + string(GeoArrowDimensionsString(dims)));
	}
	auto ga_type = GeoArrowMakeType(geometry_type, dims, GEOARROW_COORD_TYPE_SEPARATE);
	if (ga_type == GEOARROW_TYPE_UNINITIALIZED) {
		throw BinderException(fn_name + ": unsupported combination of geometry type and dimensions");
	}
	return ga_type;
}

// Bind for the per-type names: the geometry type is fixed, dimensions come from the value.
template <enum GeoArrowGeometryType GEOMETRY_TYPE>
static unique_ptr<FunctionData> NativeReadBind(BindInput &input) {
	auto &bound_function = BindFunction(input);
	auto &fn_name = BoundName(bound_function);
	auto ga_type =
	    ResolveNativeType(GEOMETRY_TYPE, BoundArgumentTypes(bound_function)[0], GEOARROW_DIMENSIONS_UNKNOWN, fn_name);
	return make_uniq<NativeReadBindData>(ga_type, fn_name);
}

// Bind for the generic form: the geometry type comes from a constant string argument.
static unique_ptr<FunctionData> GenericNativeReadBind(BindInput &input) {
	auto &arguments = BindArguments(input);
	auto &bound_function = BindFunction(input);
	auto &fn_name = BoundName(bound_function);
	if (!arguments[0]->IsFoldable()) {
		throw BinderException(fn_name + ": the geometry type must be a constant string");
	}
	auto type_value = ExpressionExecutor::EvaluateScalar(BindContext(input), *arguments[0]);
	if (type_value.IsNull()) {
		throw BinderException(fn_name + ": the geometry type must not be NULL");
	}

	auto type_name = type_value.ToString();
	enum GeoArrowGeometryType geometry_type;
	enum GeoArrowDimensions named_dims;
	if (!ParseGeometryTypeName(type_name, geometry_type, named_dims)) {
		throw BinderException(fn_name + ": unknown geometry type '" + type_name +
		                      "' (expected point, linestring, polygon, multipoint, multilinestring or "
		                      "multipolygon, optionally suffixed with z, m or zm)");
	}

	auto ga_type = ResolveNativeType(geometry_type, BoundArgumentTypes(bound_function)[1], named_dims, fn_name);
	return make_uniq<NativeReadBindData>(ga_type, fn_name);
}

// Shared execution: the GeoArrowType was resolved at bind time.
static void StGeomFromNativeFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto count = args.size();
	if (count == 0) {
		return;
	}

	auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
	auto &info = BindData(func_expr).Cast<NativeReadBindData>();
	auto fn_name = info.fn_name.c_str();
	// The generic form takes the type name first; the value is always the last argument
	auto &input = args.data[args.ColumnCount() - 1];

	ArrowArrayGuard exported;
	ExportVectorToArrow(input, count, exported);

	struct GeoArrowError ga_error;
	memset(&ga_error, 0, sizeof(ga_error));

	struct GeoArrowArrayView view;
	if (GeoArrowArrayViewInitFromType(&view, info.ga_type) != GEOARROW_OK) {
		throw InternalException(string(fn_name) + ": GeoArrowArrayViewInitFromType failed");
	}
	if (GeoArrowArrayViewSetArray(&view, exported.array.children[0], &ga_error) != GEOARROW_OK) {
		throw InvalidInputException(string(fn_name) + ": " + string(ga_error.message));
	}

	WKBWriterGuard writer;
	struct GeoArrowVisitor visitor;
	GeoArrowWKBWriterInitVisitor(&writer.writer, &visitor);
	visitor.error = &ga_error;
	if (GeoArrowArrayViewVisitNative(&view, 0, static_cast<int64_t>(count), &visitor) != GEOARROW_OK) {
		throw InvalidInputException(string(fn_name) + ": " + string(ga_error.message));
	}

	ArrowArrayGuard wkb;
	if (GeoArrowWKBWriterFinish(&writer.writer, &wkb.array, &ga_error) != GEOARROW_OK) {
		throw InvalidInputException(string(fn_name) + ": WKB writer failed - " + string(ga_error.message));
	}

	WKBArrayToVector(wkb.array, result, count, fn_name);
}

// --- st_asgeoarrowpoint: WKB (BLOB/GEOMETRY) → STRUCT(x DOUBLE, y DOUBLE) ---
// Matches GeoArrow native encoding for Point: separated coordinates as Struct<x, y>.

static void StAsGeoArrowPointFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto count = args.size();

	UnifiedVectorFormat input_data;
	ToUnified(args.data[0], count, input_data);
	auto input_entries = UnifiedVectorFormat::GetData<string_t>(input_data);

	struct GeoArrowWKBReader wkb_reader;
	GeoArrowWKBReaderInit(&wkb_reader);

	CoordExtractor extractor;
	struct GeoArrowVisitor visitor;
	InitExtractVisitor(visitor, extractor);

	struct GeoArrowError ga_error;

	auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
	auto wanted_dims = BindData(func_expr).Cast<DimensionsBindData>().dims;

	static const char *ordinate_names[] = {"x", "y", "z", "m"};
	double *ordinate_data[4] = {nullptr, nullptr, nullptr, nullptr};
	for (idx_t d = 0; d < 4; d++) {
		if (auto child = StructChild(result, ordinate_names[d])) {
			ordinate_data[d] = MutableData<double>(*child);
		}
	}
	auto &result_validity = MutableValidity(result);

	for (idx_t i = 0; i < count; i++) {
		auto input_idx = input_data.sel->get_index(i);
		if (!input_data.validity.RowIsValid(input_idx)) {
			result_validity.SetInvalid(i);
			for (auto data : ordinate_data) {
				if (data) {
					data[i] = 0;
				}
			}
			continue;
		}

		auto wkb_blob = input_entries[input_idx];

		struct GeoArrowBufferView wkb_buf;
		wkb_buf.data = reinterpret_cast<const uint8_t *>(wkb_blob.GetData());
		wkb_buf.size_bytes = static_cast<int64_t>(wkb_blob.GetSize());

		memset(&ga_error, 0, sizeof(ga_error));
		visitor.error = &ga_error;

		visitor.feat_start(&visitor);
		int rc = GeoArrowWKBReaderVisit(&wkb_reader, wkb_buf, &visitor);
		if (rc != GEOARROW_OK) {
			GeoArrowWKBReaderReset(&wkb_reader);
			throw InvalidInputException("st_asgeoarrowpoint: invalid WKB - " + string(ga_error.message));
		}
		visitor.feat_end(&visitor);

		if (extractor.geometry_type != GEOARROW_GEOMETRY_TYPE_POINT) {
			GeoArrowWKBReaderReset(&wkb_reader);
			throw InvalidInputException("st_asgeoarrowpoint: expected POINT, got geometry type %d",
			                            extractor.geometry_type);
		}
		if (extractor.xs.size() != 1) {
			GeoArrowWKBReaderReset(&wkb_reader);
			throw InvalidInputException("st_asgeoarrowpoint: POINT must have exactly one coordinate");
		}

		CheckFeatureDimensions(extractor, wanted_dims, "st_asgeoarrowpoint");
		const vector<double> *sources[] = {&extractor.xs, &extractor.ys, &extractor.zs, &extractor.ms};
		for (idx_t d = 0; d < 4; d++) {
			if (ordinate_data[d]) {
				ordinate_data[d][i] = (*sources[d])[0];
			}
		}
	}

	GeoArrowWKBReaderReset(&wkb_reader);
}

// --- GeoArrow native nested-list outputs (LineString/Polygon/Multi*) ---

// Append n (x,y) coords into the STRUCT(x,y) child of `coord_list`.
// Returns the (offset, length) entry in the coord-struct index space.
static list_entry_t AppendCoordBlock(Vector &coord_list, const CoordExtractor &ext, idx_t offset, idx_t n) {
	auto current = ListVector::GetListSize(coord_list);
	auto new_size = current + n;
	ListVector::Reserve(coord_list, new_size);
	auto &coord_struct = ListChildMutable(coord_list);

	// Copy whichever ordinates the output coordinate struct declares
	static const char *ordinate_names[] = {"x", "y", "z", "m"};
	const vector<double> *sources[] = {&ext.xs, &ext.ys, &ext.zs, &ext.ms};
	for (idx_t d = 0; d < 4; d++) {
		auto child = StructChild(coord_struct, ordinate_names[d]);
		if (!child) {
			continue;
		}
		auto data = MutableData<double>(*child);
		auto &src = *sources[d];
		for (idx_t k = 0; k < n; k++) {
			data[current + k] = src[offset + k];
		}
	}

	ListVector::SetListSize(coord_list, new_size);
	return list_entry_t {current, n};
}

// Parse one WKB feature into `extractor`. Throws on parse error. Caller must reset reader.
static void ParseWKBRow(GeoArrowWKBReader &reader, GeoArrowVisitor &visitor, GeoArrowError &err, string_t wkb,
                        const char *fn_name) {
	GeoArrowBufferView buf;
	buf.data = reinterpret_cast<const uint8_t *>(wkb.GetData());
	buf.size_bytes = static_cast<int64_t>(wkb.GetSize());
	memset(&err, 0, sizeof(err));
	visitor.error = &err;
	visitor.feat_start(&visitor);
	int rc = GeoArrowWKBReaderVisit(&reader, buf, &visitor);
	if (rc != GEOARROW_OK) {
		throw InvalidInputException(string(fn_name) + ": invalid WKB - " + string(err.message));
	}
	visitor.feat_end(&visitor);
}

// Template: LIST(STRUCT(x,y)) output — used by LineString (one run of coords) and
// MultiPoint (one coord per sub-point; we still emit a single flat list of verts).
// `take_all_as_one_ring` = true for LineString (use xs/ys directly as one list);
// MultiPoint also flattens to one list of coords, so same logic applies.
static void WriteListOfCoords(Vector &result, idx_t row, const CoordExtractor &ext) {
	auto entry = AppendCoordBlock(result, ext, 0, ext.xs.size());
	MutableData<list_entry_t>(result)[row] = entry;
}

// LIST(LIST(STRUCT(x,y))) — used by Polygon (rings) and MultiLineString (linestrings).
// `ring_offsets` gives the end index (in xs/ys) of each inner list.
static void WriteListOfListOfCoords(Vector &result, idx_t row, const CoordExtractor &ext) {
	idx_t num_inner = ext.ring_offsets.size();
	idx_t outer_start = ListVector::GetListSize(result);
	ListVector::Reserve(result, outer_start + num_inner);
	auto &inner_list = ListChildMutable(result);
	auto inner_entries = MutableData<list_entry_t>(inner_list);

	int32_t prev = 0;
	for (idx_t r = 0; r < num_inner; r++) {
		int32_t end = ext.ring_offsets[r];
		int32_t n = end - prev;
		auto coord_entry = AppendCoordBlock(inner_list, ext, static_cast<idx_t>(prev), static_cast<idx_t>(n));
		inner_entries[outer_start + r] = coord_entry;
		prev = end;
	}
	ListVector::SetListSize(result, outer_start + num_inner);
	MutableData<list_entry_t>(result)[row] = list_entry_t {outer_start, num_inner};
}

// LIST(LIST(LIST(STRUCT(x,y)))) — MultiPolygon.
static void WriteMultiPolygon(Vector &result, idx_t row, const CoordExtractor &ext) {
	idx_t num_polys = ext.geom_offsets.size();
	idx_t poly_start = ListVector::GetListSize(result);
	ListVector::Reserve(result, poly_start + num_polys);
	auto &poly_list = ListChildMutable(result);
	auto poly_entries = MutableData<list_entry_t>(poly_list);

	int32_t ring_idx_start = 0;
	int32_t coord_idx_start = 0;
	for (idx_t p = 0; p < num_polys; p++) {
		int32_t ring_idx_end = ext.geom_offsets[p];
		int32_t rings_in_poly = ring_idx_end - ring_idx_start;

		idx_t ring_start = ListVector::GetListSize(poly_list);
		ListVector::Reserve(poly_list, ring_start + static_cast<idx_t>(rings_in_poly));
		auto &ring_list = ListChildMutable(poly_list);
		auto ring_entries = MutableData<list_entry_t>(ring_list);

		for (int32_t r = ring_idx_start; r < ring_idx_end; r++) {
			int32_t coord_end = ext.ring_offsets[r];
			int32_t n = coord_end - coord_idx_start;
			auto coord_entry =
			    AppendCoordBlock(ring_list, ext, static_cast<idx_t>(coord_idx_start), static_cast<idx_t>(n));
			ring_entries[ring_start + (r - ring_idx_start)] = coord_entry;
			coord_idx_start = coord_end;
		}
		ListVector::SetListSize(poly_list, ring_start + static_cast<idx_t>(rings_in_poly));
		poly_entries[poly_start + p] = list_entry_t {ring_start, static_cast<idx_t>(rings_in_poly)};
		ring_idx_start = ring_idx_end;
	}
	ListVector::SetListSize(result, poly_start + num_polys);
	MutableData<list_entry_t>(result)[row] = list_entry_t {poly_start, num_polys};
}

// Generic driver: parse each row, type-check, dispatch to the per-shape writer.
template <enum GeoArrowGeometryType EXPECTED, void (*WRITER)(Vector &, idx_t, const CoordExtractor &)>
static void StAsGeoArrowNestedFun(DataChunk &args, ExpressionState &state, Vector &result, const char *fn_name) {
	auto count = args.size();

	auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
	auto wanted_dims = BindData(func_expr).Cast<DimensionsBindData>().dims;

	UnifiedVectorFormat input_data;
	ToUnified(args.data[0], count, input_data);
	auto input_entries = UnifiedVectorFormat::GetData<string_t>(input_data);

	struct GeoArrowWKBReader wkb_reader;
	GeoArrowWKBReaderInit(&wkb_reader);

	CoordExtractor extractor;
	struct GeoArrowVisitor visitor;
	InitExtractVisitor(visitor, extractor);

	struct GeoArrowError ga_error;
	auto &result_validity = MutableValidity(result);

	for (idx_t i = 0; i < count; i++) {
		auto input_idx = input_data.sel->get_index(i);
		if (!input_data.validity.RowIsValid(input_idx)) {
			result_validity.SetInvalid(i);
			MutableData<list_entry_t>(result)[i] = list_entry_t {ListVector::GetListSize(result), 0};
			continue;
		}

		try {
			ParseWKBRow(wkb_reader, visitor, ga_error, input_entries[input_idx], fn_name);
		} catch (...) {
			GeoArrowWKBReaderReset(&wkb_reader);
			throw;
		}

		if (extractor.geometry_type != EXPECTED) {
			GeoArrowWKBReaderReset(&wkb_reader);
			throw InvalidInputException("%s: expected geometry type %d, got %d", fn_name, static_cast<int>(EXPECTED),
			                            extractor.geometry_type);
		}
		try {
			CheckFeatureDimensions(extractor, wanted_dims, fn_name);
		} catch (...) {
			GeoArrowWKBReaderReset(&wkb_reader);
			throw;
		}

		WRITER(result, i, extractor);
	}

	GeoArrowWKBReaderReset(&wkb_reader);
}

// st_asgeoarrow<type>(geom[, dimensions]): keep the registered nesting depth and swap the
// coordinate struct for the requested dimensions. Depth alone determines the output shape,
// so this one bind serves all six geometry types.
static unique_ptr<FunctionData> StAsGeoArrowNativeBind(BindInput &input) {
	auto dims = BindDimensionsArgument(input, 1);
	auto &bound_function = BindFunction(input);
	int depth = 0;
	enum GeoArrowDimensions registered_dims = GEOARROW_DIMENSIONS_UNKNOWN;
	if (!InspectNativeType(BoundReturnType(bound_function), depth, registered_dims)) {
		throw InternalException("st_asgeoarrow<type>: registered return type is not a native encoding");
	}
	auto out = CoordStructType(dims);
	for (int i = 0; i < depth; i++) {
		out = LogicalType::LIST(out);
	}
	SetBoundReturnType(bound_function, std::move(out));
	return make_uniq<DimensionsBindData>(dims);
}

static void StAsGeoArrowLineStringFun(DataChunk &args, ExpressionState &state, Vector &result) {
	StAsGeoArrowNestedFun<GEOARROW_GEOMETRY_TYPE_LINESTRING, WriteListOfCoords>(args, state, result,
	                                                                            "st_asgeoarrowlinestring");
}

static void StAsGeoArrowPolygonFun(DataChunk &args, ExpressionState &state, Vector &result) {
	StAsGeoArrowNestedFun<GEOARROW_GEOMETRY_TYPE_POLYGON, WriteListOfListOfCoords>(args, state, result,
	                                                                               "st_asgeoarrowpolygon");
}

static void StAsGeoArrowMultiPointFun(DataChunk &args, ExpressionState &state, Vector &result) {
	StAsGeoArrowNestedFun<GEOARROW_GEOMETRY_TYPE_MULTIPOINT, WriteListOfCoords>(args, state, result,
	                                                                            "st_asgeoarrowmultipoint");
}

static void StAsGeoArrowMultiLineStringFun(DataChunk &args, ExpressionState &state, Vector &result) {
	StAsGeoArrowNestedFun<GEOARROW_GEOMETRY_TYPE_MULTILINESTRING, WriteListOfListOfCoords>(
	    args, state, result, "st_asgeoarrowmultilinestring");
}

static void StAsGeoArrowMultiPolygonFun(DataChunk &args, ExpressionState &state, Vector &result) {
	StAsGeoArrowNestedFun<GEOARROW_GEOMETRY_TYPE_MULTIPOLYGON, WriteMultiPolygon>(args, state, result,
	                                                                              "st_asgeoarrowmultipolygon");
}

// --- duck_geoarrow_version: returns extension + geoarrow-c version ---

static void DuckGeoarrowVersionFun(DataChunk &args, ExpressionState &state, Vector &result) {
	string version_str;
#ifdef EXT_VERSION_DUCK_GEOARROW
	version_str = EXT_VERSION_DUCK_GEOARROW;
#else
	version_str = "unknown";
#endif
	version_str += " (geoarrow-c " + string(GeoArrowVersion()) + ")";
	result.SetValue(0, Value(version_str));
}

// --- Extension registration ---

// The six GeoArrow geometry types, with the per-type function-name suffix and the bind
// callback that pins the geometry type for the read direction.
struct GeometryTypeEntry {
	enum GeoArrowGeometryType geometry_type;
	const char *suffix;
	scalar_function_t write_fn;
	bind_scalar_function_t read_bind;
};

// Every conversion function throws on malformed input, so each must be marked fallible;
// DuckDB v2 turns an execution error from an unmarked function into an internal error.
static ScalarFunction Fallible(ScalarFunction function) {
	function.SetFallible();
	return function;
}

static void LoadInternal(ExtensionLoader &loader) {
	static const enum GeoArrowDimensions ALL_DIMS[] = {GEOARROW_DIMENSIONS_XY, GEOARROW_DIMENSIONS_XYZ,
	                                                   GEOARROW_DIMENSIONS_XYM, GEOARROW_DIMENSIONS_XYZM};

	const GeometryTypeEntry types[] = {
	    {GEOARROW_GEOMETRY_TYPE_POINT, "point", StAsGeoArrowPointFun,
	     DUCK_GEOARROW_BIND(NativeReadBind<GEOARROW_GEOMETRY_TYPE_POINT>)},
	    {GEOARROW_GEOMETRY_TYPE_LINESTRING, "linestring", StAsGeoArrowLineStringFun,
	     DUCK_GEOARROW_BIND(NativeReadBind<GEOARROW_GEOMETRY_TYPE_LINESTRING>)},
	    {GEOARROW_GEOMETRY_TYPE_POLYGON, "polygon", StAsGeoArrowPolygonFun,
	     DUCK_GEOARROW_BIND(NativeReadBind<GEOARROW_GEOMETRY_TYPE_POLYGON>)},
	    {GEOARROW_GEOMETRY_TYPE_MULTIPOINT, "multipoint", StAsGeoArrowMultiPointFun,
	     DUCK_GEOARROW_BIND(NativeReadBind<GEOARROW_GEOMETRY_TYPE_MULTIPOINT>)},
	    {GEOARROW_GEOMETRY_TYPE_MULTILINESTRING, "multilinestring", StAsGeoArrowMultiLineStringFun,
	     DUCK_GEOARROW_BIND(NativeReadBind<GEOARROW_GEOMETRY_TYPE_MULTILINESTRING>)},
	    {GEOARROW_GEOMETRY_TYPE_MULTIPOLYGON, "multipolygon", StAsGeoArrowMultiPolygonFun,
	     DUCK_GEOARROW_BIND(NativeReadBind<GEOARROW_GEOMETRY_TYPE_MULTIPOLYGON>)},
	};

	// --- st_asgeoarrow(geom[, dimensions]) -> flat GeoArrow STRUCT ---
	// The return type is computed in the bind, so the one-argument form stays XY.
	ScalarFunctionSet st_asgeoarrow_set("st_asgeoarrow");
	const vector<LogicalType> wkb_inputs = {LogicalType::GEOMETRY(), LogicalType::BLOB};
	for (auto &input : wkb_inputs) {
		st_asgeoarrow_set.AddFunction(
		    Fallible(ScalarFunction(vector<LogicalType> {input}, GeoArrowStructType(), StAsGeoArrowWKBFun,
		                            DUCK_GEOARROW_BIND(StAsGeoArrowBind))));
		st_asgeoarrow_set.AddFunction(
		    Fallible(ScalarFunction(vector<LogicalType> {input, LogicalType::VARCHAR}, GeoArrowStructType(),
		                            StAsGeoArrowWKBFun, DUCK_GEOARROW_BIND(StAsGeoArrowBind))));
	}
	loader.RegisterFunction(st_asgeoarrow_set);

	// --- st_geomfromgeoarrow(...) -> GEOMETRY ---
	// Three shapes share one name:
	//   (flat struct)              the original flat representation, any dimensions
	//   ('<type>', native value)   generic native reader, the form asked for in issue #7
	ScalarFunctionSet st_geomfromgeoarrow_set("st_geomfromgeoarrow");
	for (auto dims : ALL_DIMS) {
		st_geomfromgeoarrow_set.AddFunction(Fallible(ScalarFunction(vector<LogicalType> {GeoArrowStructType(dims)},
		                                                            LogicalType::GEOMETRY(), StGeomFromGeoArrowFun)));
	}
	// One signature per (nesting depth, dimensions); the type name argument picks which
	// geometry type a given shape means, since e.g. LineString and MultiPoint collide.
	for (auto dims : ALL_DIMS) {
		auto native = CoordStructType(dims);
		for (int depth = 0; depth <= 3; depth++) {
			st_geomfromgeoarrow_set.AddFunction(
			    Fallible(ScalarFunction(vector<LogicalType> {LogicalType::VARCHAR, native}, LogicalType::GEOMETRY(),
			                            StGeomFromNativeFun, DUCK_GEOARROW_BIND(GenericNativeReadBind))));
			native = LogicalType::LIST(native);
		}
	}
	loader.RegisterFunction(st_geomfromgeoarrow_set);

	// --- st_asgeoarrow<type>(geom[, dimensions]) and st_geomfromgeoarrow<type>(value) ---
	// GeoArrow native encodings, separated coordinates per the spec:
	//   Point           → STRUCT(x, y)
	//   LineString      → LIST(STRUCT(x, y))
	//   Polygon         → LIST(LIST(STRUCT(x, y)))
	//   MultiPoint      → LIST(STRUCT(x, y))
	//   MultiLineString → LIST(LIST(STRUCT(x, y)))
	//   MultiPolygon    → LIST(LIST(LIST(STRUCT(x, y))))
	// with z / m ordinates added to the coordinate struct for the other dimensions.
	for (auto &entry : types) {
		ScalarFunctionSet write_set(FunctionSetName(string("st_asgeoarrow") + entry.suffix));
		for (auto &input : wkb_inputs) {
			auto xy_out = NativeType(entry.geometry_type, GEOARROW_DIMENSIONS_XY);
			write_set.AddFunction(Fallible(ScalarFunction(vector<LogicalType> {input}, xy_out, entry.write_fn,
			                                              DUCK_GEOARROW_BIND(StAsGeoArrowNativeBind))));
			write_set.AddFunction(Fallible(ScalarFunction(vector<LogicalType> {input, LogicalType::VARCHAR}, xy_out,
			                                              entry.write_fn, DUCK_GEOARROW_BIND(StAsGeoArrowNativeBind))));
		}
		loader.RegisterFunction(write_set);

		ScalarFunctionSet read_set(FunctionSetName(string("st_geomfromgeoarrow") + entry.suffix));
		for (auto dims : ALL_DIMS) {
			read_set.AddFunction(
			    Fallible(ScalarFunction(vector<LogicalType> {NativeType(entry.geometry_type, dims)},
			                            LogicalType::GEOMETRY(), StGeomFromNativeFun, entry.read_bind)));
		}
		loader.RegisterFunction(read_set);
	}

	// duck_geoarrow_version: returns version info
	auto version_func = ScalarFunction("duck_geoarrow_version", {}, LogicalType::VARCHAR, DuckGeoarrowVersionFun);
	loader.RegisterFunction(version_func);
}

void DuckGeoarrowExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}

std::string DuckGeoarrowExtension::Name() {
	return "duck_geoarrow";
}

std::string DuckGeoarrowExtension::Version() const {
#ifdef EXT_VERSION_DUCK_GEOARROW
	return EXT_VERSION_DUCK_GEOARROW;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(duck_geoarrow, loader) {
	duckdb::LoadInternal(loader);
}
}
