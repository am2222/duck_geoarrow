#define DUCKDB_EXTENSION_MAIN

#include "duck_geoarrow_extension.hpp"
#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/function/function_set.hpp"
#include "duckdb/function/scalar_function.hpp"
#include <duckdb/parser/parsed_data/create_scalar_function_info.hpp>

#include "geoarrow/geoarrow.h"

namespace duckdb {

// The GeoArrow struct type used as output for st_asgeoarrow and input for st_geomfromgeoarrow
static LogicalType GeoArrowStructType() {
	return LogicalType::STRUCT({{"geometry_type", LogicalType::UTINYINT},
	                            {"xs", LogicalType::LIST(LogicalType::DOUBLE)},
	                            {"ys", LogicalType::LIST(LogicalType::DOUBLE)},
	                            {"ring_offsets", LogicalType::LIST(LogicalType::INTEGER)},
	                            {"geom_offsets", LogicalType::LIST(LogicalType::INTEGER)}});
}

// --- Coordinate extraction visitor (WKB → struct) ---

struct CoordExtractor {
	vector<double> xs;
	vector<double> ys;
	vector<int32_t> ring_offsets;
	vector<int32_t> geom_offsets;
	uint8_t geometry_type = 0;
	int depth = 0;
};

static int ExtractFeatStart(struct GeoArrowVisitor *v) {
	auto *ext = static_cast<CoordExtractor *>(v->private_data);
	ext->xs.clear();
	ext->ys.clear();
	ext->ring_offsets.clear();
	ext->geom_offsets.clear();
	ext->depth = 0;
	ext->geometry_type = 0;
	return GEOARROW_OK;
}

static int ExtractGeomStart(struct GeoArrowVisitor *v, enum GeoArrowGeometryType type, enum GeoArrowDimensions dims) {
	(void)dims;
	auto *ext = static_cast<CoordExtractor *>(v->private_data);
	if (ext->depth == 0) {
		ext->geometry_type = static_cast<uint8_t>(type);
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
	for (int64_t i = 0; i < coords->n_coords; i++) {
		ext->xs.push_back(GEOARROW_COORD_VIEW_VALUE(coords, i, 0));
		ext->ys.push_back(GEOARROW_COORD_VIEW_VALUE(coords, i, 1));
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

	auto &child = ListVector::GetEntry(list_vec);
	auto child_data = FlatVector::GetData<double>(child);
	for (idx_t j = 0; j < values.size(); j++) {
		child_data[current_size + j] = values[j];
	}

	auto list_data = FlatVector::GetData<list_entry_t>(list_vec);
	list_data[row].offset = current_size;
	list_data[row].length = values.size();
	ListVector::SetListSize(list_vec, new_size);
}

// Helper: populate a DuckDB LIST(INTEGER) vector entry
static void SetIntList(Vector &list_vec, idx_t row, const vector<int32_t> &values) {
	auto current_size = ListVector::GetListSize(list_vec);
	auto new_size = current_size + values.size();
	ListVector::Reserve(list_vec, new_size);

	auto &child = ListVector::GetEntry(list_vec);
	auto child_data = FlatVector::GetData<int32_t>(child);
	for (idx_t j = 0; j < values.size(); j++) {
		child_data[current_size + j] = values[j];
	}

	auto list_data = FlatVector::GetData<list_entry_t>(list_vec);
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

// Helper: write extractor results into the output STRUCT vectors for row i
static void WriteExtractorOutput(CoordExtractor &extractor, Vector &result, idx_t i) {
	auto &children = StructVector::GetEntries(result);
	FlatVector::GetData<uint8_t>(*children[0])[i] = extractor.geometry_type;
	SetDoubleList(*children[1], i, extractor.xs);
	SetDoubleList(*children[2], i, extractor.ys);
	SetIntList(*children[3], i, extractor.ring_offsets);
	SetIntList(*children[4], i, extractor.geom_offsets);
}

// Helper: write NULL into the output STRUCT vectors for row i
static void WriteNullOutput(Vector &result, idx_t i) {
	FlatVector::Validity(result).SetInvalid(i);
	auto &children = StructVector::GetEntries(result);
	FlatVector::GetData<uint8_t>(*children[0])[i] = 0;
	SetDoubleList(*children[1], i, {});
	SetDoubleList(*children[2], i, {});
	SetIntList(*children[3], i, {});
	SetIntList(*children[4], i, {});
}

// --- st_asgeoarrow: WKB (BLOB/GEOMETRY) → GeoArrow STRUCT ---

static void StAsGeoArrowWKBFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto count = args.size();

	UnifiedVectorFormat input_data;
	args.data[0].ToUnifiedFormat(count, input_data);
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

		WriteExtractorOutput(extractor, result, i);
	}

	GeoArrowWKBReaderReset(&wkb_reader);
}

// --- st_geomfromgeoarrow: GeoArrow STRUCT → WKB BLOB ---

// Helper: build a GeoArrowCoordView from separated x/y arrays
static struct GeoArrowCoordView MakeCoordView(const double *xs, const double *ys, int64_t n) {
	struct GeoArrowCoordView cv;
	memset(&cv, 0, sizeof(cv));
	cv.values[0] = xs;
	cv.values[1] = ys;
	cv.n_coords = n;
	cv.n_values = 2;
	cv.coords_stride = 1;
	return cv;
}

// Drive visitor callbacks to produce WKB from extracted coordinate data
static void DriveVisitor(struct GeoArrowVisitor *v, uint8_t geom_type, const double *xs, const double *ys,
                         idx_t num_coords, const int32_t *ring_offs, idx_t num_ring_offs, const int32_t *geom_offs,
                         idx_t num_geom_offs) {
	auto gt = static_cast<enum GeoArrowGeometryType>(geom_type);
	auto dims = GEOARROW_DIMENSIONS_XY;

	switch (gt) {
	case GEOARROW_GEOMETRY_TYPE_POINT:
	case GEOARROW_GEOMETRY_TYPE_LINESTRING: {
		v->geom_start(v, gt, dims);
		auto cv = MakeCoordView(xs, ys, static_cast<int64_t>(num_coords));
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
			auto cv = MakeCoordView(xs + prev, ys + prev, ring_end - prev);
			v->coords(v, &cv);
			v->ring_end(v);
			prev = ring_end;
		}
		v->geom_end(v);
		break;
	}
	case GEOARROW_GEOMETRY_TYPE_MULTIPOINT: {
		v->geom_start(v, gt, dims);
		int32_t prev = 0;
		for (idx_t r = 0; r < num_ring_offs; r++) {
			v->geom_start(v, GEOARROW_GEOMETRY_TYPE_POINT, dims);
			int32_t seg_end = ring_offs[r];
			auto cv = MakeCoordView(xs + prev, ys + prev, seg_end - prev);
			v->coords(v, &cv);
			v->geom_end(v);
			prev = seg_end;
		}
		v->geom_end(v);
		break;
	}
	case GEOARROW_GEOMETRY_TYPE_MULTILINESTRING: {
		v->geom_start(v, gt, dims);
		int32_t prev = 0;
		for (idx_t r = 0; r < num_ring_offs; r++) {
			v->geom_start(v, GEOARROW_GEOMETRY_TYPE_LINESTRING, dims);
			int32_t seg_end = ring_offs[r];
			auto cv = MakeCoordView(xs + prev, ys + prev, seg_end - prev);
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
				auto cv = MakeCoordView(xs + coord_prev, ys + coord_prev, coord_end - coord_prev);
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

static void StGeomFromGeoArrowFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto count = args.size();

	// Input STRUCT children
	auto &input = args.data[0];
	auto &in_children = StructVector::GetEntries(input);
	auto &type_vec = *in_children[0];
	auto &xs_vec = *in_children[1];
	auto &ys_vec = *in_children[2];
	auto &ring_off_vec = *in_children[3];
	auto &geom_off_vec = *in_children[4];

	UnifiedVectorFormat input_format;
	input.ToUnifiedFormat(count, input_format);
	auto &result_validity = FlatVector::Validity(result);

	for (idx_t i = 0; i < count; i++) {
		auto input_idx = input_format.sel->get_index(i);
		if (!input_format.validity.RowIsValid(input_idx)) {
			result_validity.SetInvalid(i);
			continue;
		}

		// Read geometry_type
		uint8_t geom_type = FlatVector::GetData<uint8_t>(type_vec)[i];

		// Read xs
		auto xs_entry = FlatVector::GetData<list_entry_t>(xs_vec)[i];
		auto &xs_child = ListVector::GetEntry(xs_vec);
		auto xs_data = FlatVector::GetData<double>(xs_child) + xs_entry.offset;

		// Read ys
		auto ys_entry = FlatVector::GetData<list_entry_t>(ys_vec)[i];
		auto &ys_child = ListVector::GetEntry(ys_vec);
		auto ys_data = FlatVector::GetData<double>(ys_child) + ys_entry.offset;

		// Read ring_offsets
		auto ro_entry = FlatVector::GetData<list_entry_t>(ring_off_vec)[i];
		auto &ro_child = ListVector::GetEntry(ring_off_vec);
		auto ro_data = FlatVector::GetData<int32_t>(ro_child) + ro_entry.offset;

		// Read geom_offsets
		auto go_entry = FlatVector::GetData<list_entry_t>(geom_off_vec)[i];
		auto &go_child = ListVector::GetEntry(geom_off_vec);
		auto go_data = FlatVector::GetData<int32_t>(go_child) + go_entry.offset;

		// Set up WKB writer + visitor
		struct GeoArrowWKBWriter wkb_writer;
		GeoArrowWKBWriterInit(&wkb_writer);

		struct GeoArrowVisitor visitor;
		GeoArrowWKBWriterInitVisitor(&wkb_writer, &visitor);

		struct GeoArrowError ga_error;
		memset(&ga_error, 0, sizeof(ga_error));
		visitor.error = &ga_error;

		// Drive the visitor to produce WKB
		visitor.feat_start(&visitor);
		DriveVisitor(&visitor, geom_type, xs_data, ys_data, xs_entry.length, ro_data, ro_entry.length, go_data,
		             go_entry.length);
		visitor.feat_end(&visitor);

		// Extract WKB from the writer's output
		struct ArrowArray arr;
		memset(&arr, 0, sizeof(arr));
		int rc = GeoArrowWKBWriterFinish(&wkb_writer, &arr, &ga_error);
		if (rc != GEOARROW_OK) {
			GeoArrowWKBWriterReset(&wkb_writer);
			throw InvalidInputException("st_geomfromgeoarrow: WKB writer failed - " + string(ga_error.message));
		}

		auto offsets = static_cast<const int32_t *>(arr.buffers[1]);
		auto data = static_cast<const char *>(arr.buffers[2]);
		int32_t start = offsets[0];
		int32_t len = offsets[1] - start;

		FlatVector::GetData<string_t>(result)[i] =
		    StringVector::AddStringOrBlob(result, data + start, static_cast<idx_t>(len));

		if (arr.release) {
			arr.release(&arr);
		}
		GeoArrowWKBWriterReset(&wkb_writer);
	}
}

// --- st_asgeoarrowpoint: WKB (BLOB/GEOMETRY) → STRUCT(x DOUBLE, y DOUBLE) ---
// Matches GeoArrow native encoding for Point: separated coordinates as Struct<x, y>.

static void StAsGeoArrowPointFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto count = args.size();

	UnifiedVectorFormat input_data;
	args.data[0].ToUnifiedFormat(count, input_data);
	auto input_entries = UnifiedVectorFormat::GetData<string_t>(input_data);

	struct GeoArrowWKBReader wkb_reader;
	GeoArrowWKBReaderInit(&wkb_reader);

	CoordExtractor extractor;
	struct GeoArrowVisitor visitor;
	InitExtractVisitor(visitor, extractor);

	struct GeoArrowError ga_error;

	auto &children = StructVector::GetEntries(result);
	auto x_data = FlatVector::GetData<double>(*children[0]);
	auto y_data = FlatVector::GetData<double>(*children[1]);
	auto &result_validity = FlatVector::Validity(result);

	for (idx_t i = 0; i < count; i++) {
		auto input_idx = input_data.sel->get_index(i);
		if (!input_data.validity.RowIsValid(input_idx)) {
			result_validity.SetInvalid(i);
			x_data[i] = 0;
			y_data[i] = 0;
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

		x_data[i] = extractor.xs[0];
		y_data[i] = extractor.ys[0];
	}

	GeoArrowWKBReaderReset(&wkb_reader);
}

// --- GeoArrow native nested-list outputs (LineString/Polygon/Multi*) ---

// Append n (x,y) coords into the STRUCT(x,y) child of `coord_list`.
// Returns the (offset, length) entry in the coord-struct index space.
static list_entry_t AppendCoordBlock(Vector &coord_list, const double *xs, const double *ys, idx_t n) {
	auto current = ListVector::GetListSize(coord_list);
	auto new_size = current + n;
	ListVector::Reserve(coord_list, new_size);
	auto &coord_struct = ListVector::GetEntry(coord_list);
	auto &coord_children = StructVector::GetEntries(coord_struct);
	auto x_data = FlatVector::GetData<double>(*coord_children[0]);
	auto y_data = FlatVector::GetData<double>(*coord_children[1]);
	for (idx_t k = 0; k < n; k++) {
		x_data[current + k] = xs[k];
		y_data[current + k] = ys[k];
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
	auto entry = AppendCoordBlock(result, ext.xs.data(), ext.ys.data(), ext.xs.size());
	FlatVector::GetData<list_entry_t>(result)[row] = entry;
}

// LIST(LIST(STRUCT(x,y))) — used by Polygon (rings) and MultiLineString (linestrings).
// `ring_offsets` gives the end index (in xs/ys) of each inner list.
static void WriteListOfListOfCoords(Vector &result, idx_t row, const CoordExtractor &ext) {
	idx_t num_inner = ext.ring_offsets.size();
	idx_t outer_start = ListVector::GetListSize(result);
	ListVector::Reserve(result, outer_start + num_inner);
	auto &inner_list = ListVector::GetEntry(result);
	auto inner_entries = FlatVector::GetData<list_entry_t>(inner_list);

	int32_t prev = 0;
	for (idx_t r = 0; r < num_inner; r++) {
		int32_t end = ext.ring_offsets[r];
		int32_t n = end - prev;
		auto coord_entry =
		    AppendCoordBlock(inner_list, ext.xs.data() + prev, ext.ys.data() + prev, static_cast<idx_t>(n));
		inner_entries[outer_start + r] = coord_entry;
		prev = end;
	}
	ListVector::SetListSize(result, outer_start + num_inner);
	FlatVector::GetData<list_entry_t>(result)[row] = list_entry_t {outer_start, num_inner};
}

// LIST(LIST(LIST(STRUCT(x,y)))) — MultiPolygon.
static void WriteMultiPolygon(Vector &result, idx_t row, const CoordExtractor &ext) {
	idx_t num_polys = ext.geom_offsets.size();
	idx_t poly_start = ListVector::GetListSize(result);
	ListVector::Reserve(result, poly_start + num_polys);
	auto &poly_list = ListVector::GetEntry(result);
	auto poly_entries = FlatVector::GetData<list_entry_t>(poly_list);

	int32_t ring_idx_start = 0;
	int32_t coord_idx_start = 0;
	for (idx_t p = 0; p < num_polys; p++) {
		int32_t ring_idx_end = ext.geom_offsets[p];
		int32_t rings_in_poly = ring_idx_end - ring_idx_start;

		idx_t ring_start = ListVector::GetListSize(poly_list);
		ListVector::Reserve(poly_list, ring_start + static_cast<idx_t>(rings_in_poly));
		auto &ring_list = ListVector::GetEntry(poly_list);
		auto ring_entries = FlatVector::GetData<list_entry_t>(ring_list);

		for (int32_t r = ring_idx_start; r < ring_idx_end; r++) {
			int32_t coord_end = ext.ring_offsets[r];
			int32_t n = coord_end - coord_idx_start;
			auto coord_entry = AppendCoordBlock(ring_list, ext.xs.data() + coord_idx_start,
			                                    ext.ys.data() + coord_idx_start, static_cast<idx_t>(n));
			ring_entries[ring_start + (r - ring_idx_start)] = coord_entry;
			coord_idx_start = coord_end;
		}
		ListVector::SetListSize(poly_list, ring_start + static_cast<idx_t>(rings_in_poly));
		poly_entries[poly_start + p] = list_entry_t {ring_start, static_cast<idx_t>(rings_in_poly)};
		ring_idx_start = ring_idx_end;
	}
	ListVector::SetListSize(result, poly_start + num_polys);
	FlatVector::GetData<list_entry_t>(result)[row] = list_entry_t {poly_start, num_polys};
}

// Generic driver: parse each row, type-check, dispatch to the per-shape writer.
template <enum GeoArrowGeometryType EXPECTED, void (*WRITER)(Vector &, idx_t, const CoordExtractor &)>
static void StAsGeoArrowNestedFun(DataChunk &args, ExpressionState &state, Vector &result, const char *fn_name) {
	auto count = args.size();

	UnifiedVectorFormat input_data;
	args.data[0].ToUnifiedFormat(count, input_data);
	auto input_entries = UnifiedVectorFormat::GetData<string_t>(input_data);

	struct GeoArrowWKBReader wkb_reader;
	GeoArrowWKBReaderInit(&wkb_reader);

	CoordExtractor extractor;
	struct GeoArrowVisitor visitor;
	InitExtractVisitor(visitor, extractor);

	struct GeoArrowError ga_error;
	auto &result_validity = FlatVector::Validity(result);

	for (idx_t i = 0; i < count; i++) {
		auto input_idx = input_data.sel->get_index(i);
		if (!input_data.validity.RowIsValid(input_idx)) {
			result_validity.SetInvalid(i);
			FlatVector::GetData<list_entry_t>(result)[i] = list_entry_t {ListVector::GetListSize(result), 0};
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

		WRITER(result, i, extractor);
	}

	GeoArrowWKBReaderReset(&wkb_reader);
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

static void LoadInternal(ExtensionLoader &loader) {
	auto geoarrow_struct = GeoArrowStructType();

	// st_asgeoarrow: accepts GEOMETRY or BLOB (WKB)
	ScalarFunctionSet st_asgeoarrow_set("st_asgeoarrow");
	st_asgeoarrow_set.AddFunction(ScalarFunction({LogicalType::GEOMETRY()}, geoarrow_struct, StAsGeoArrowWKBFun));
	st_asgeoarrow_set.AddFunction(ScalarFunction({LogicalType::BLOB}, geoarrow_struct, StAsGeoArrowWKBFun));
	loader.RegisterFunction(st_asgeoarrow_set);

	// st_geomfromgeoarrow: GeoArrow STRUCT → GEOMETRY
	auto st_geomfromgeoarrow_func =
	    ScalarFunction("st_geomfromgeoarrow", {geoarrow_struct}, LogicalType::GEOMETRY(), StGeomFromGeoArrowFun);
	loader.RegisterFunction(st_geomfromgeoarrow_func);

	// GeoArrow native-encoding output types (separated coords per spec):
	//   Point           → STRUCT(x, y)
	//   LineString      → LIST(STRUCT(x, y))
	//   Polygon         → LIST(LIST(STRUCT(x, y)))
	//   MultiPoint      → LIST(STRUCT(x, y))
	//   MultiLineString → LIST(LIST(STRUCT(x, y)))
	//   MultiPolygon    → LIST(LIST(LIST(STRUCT(x, y))))
	auto coord_type = LogicalType::STRUCT({{"x", LogicalType::DOUBLE}, {"y", LogicalType::DOUBLE}});
	auto linestring_type = LogicalType::LIST(coord_type);
	auto polygon_type = LogicalType::LIST(linestring_type);
	auto multipoint_type = LogicalType::LIST(coord_type);
	auto multilinestring_type = LogicalType::LIST(linestring_type);
	auto multipolygon_type = LogicalType::LIST(polygon_type);

	auto register_native = [&](const char *name, LogicalType out, scalar_function_t fn) {
		ScalarFunctionSet set(name);
		set.AddFunction(ScalarFunction({LogicalType::GEOMETRY()}, out, fn));
		set.AddFunction(ScalarFunction({LogicalType::BLOB}, out, fn));
		loader.RegisterFunction(set);
	};
	register_native("st_asgeoarrowpoint", coord_type, StAsGeoArrowPointFun);
	register_native("st_asgeoarrowlinestring", linestring_type, StAsGeoArrowLineStringFun);
	register_native("st_asgeoarrowpolygon", polygon_type, StAsGeoArrowPolygonFun);
	register_native("st_asgeoarrowmultipoint", multipoint_type, StAsGeoArrowMultiPointFun);
	register_native("st_asgeoarrowmultilinestring", multilinestring_type, StAsGeoArrowMultiLineStringFun);
	register_native("st_asgeoarrowmultipolygon", multipolygon_type, StAsGeoArrowMultiPolygonFun);

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
