# Duck_Geoarrow

This extension, Duck_Geoarrow, converts between DuckDB `GEOMETRY` (or WKB `BLOB`) and the
[GeoArrow](https://geoarrow.org/) coordinate encodings, powered by the
[geoarrow-c](https://github.com/geoarrow/geoarrow-c) library.

Two GeoArrow representations are supported:

- **Native encodings** — nested lists of separated coordinates (`STRUCT(x, y)[][]` for a
  Polygon, and so on). This is what GeoParquet, geoarrow-pyarrow and geopandas produce, so
  it is the one to reach for when reading or writing files.
- **A flat struct** — one struct per geometry holding parallel coordinate and offset
  arrays. Convenient for hand-building geometries in SQL.

Both directions support XY, XYZ, XYM and XYZM.

## Quick start

Read a GeoArrow-encoded GeoParquet file written by geopandas. A geometry column written
with `encoding="polygon"` arrives in DuckDB as `STRUCT(x DOUBLE, y DOUBLE)[][]`:

```sql
SELECT county_name, st_geomfromgeoarrow('polygon', geometry) AS geom
FROM read_parquet('county_regions.parquet');
```

Write one back out:

```sql
COPY (SELECT county_name, st_asgeoarrowpolygon(geom) AS geometry FROM counties)
TO 'counties.parquet';
```

## Functions

| Function | Direction | Notes |
|---|---|---|
| `st_geomfromgeoarrow(type, value)` | GeoArrow → GEOMETRY | Generic: any native encoding, type given as a string |
| `st_geomfromgeoarrow<type>(value)` | GeoArrow → GEOMETRY | One name per geometry type |
| `st_geomfromgeoarrow(struct)` | GeoArrow → GEOMETRY | Flat struct form |
| `st_asgeoarrow<type>(geom[, dims])` | GEOMETRY → GeoArrow | Native encoding for one geometry type |
| `st_asgeoarrow(geom[, dims])` | GEOMETRY → GeoArrow | Flat struct form |
| `duck_geoarrow_version()` | — | Extension and geoarrow-c versions |

`<type>` is one of `point`, `linestring`, `polygon`, `multipoint`, `multilinestring` or
`multipolygon`.

## Native encodings

| Geometry type | DuckDB type | GEOMETRY → GeoArrow | GeoArrow → GEOMETRY |
|---|---|---|---|
| Point | `STRUCT(x, y)` | `st_asgeoarrowpoint` | `st_geomfromgeoarrowpoint` |
| LineString | `STRUCT(x, y)[]` | `st_asgeoarrowlinestring` | `st_geomfromgeoarrowlinestring` |
| Polygon | `STRUCT(x, y)[][]` | `st_asgeoarrowpolygon` | `st_geomfromgeoarrowpolygon` |
| MultiPoint | `STRUCT(x, y)[]` | `st_asgeoarrowmultipoint` | `st_geomfromgeoarrowmultipoint` |
| MultiLineString | `STRUCT(x, y)[][]` | `st_asgeoarrowmultilinestring` | `st_geomfromgeoarrowmultilinestring` |
| MultiPolygon | `STRUCT(x, y)[][][]` | `st_asgeoarrowmultipolygon` | `st_geomfromgeoarrowmultipolygon` |

These are the separated-coordinate layouts from the
[GeoArrow specification](https://geoarrow.org/format), so the values interoperate directly
with other GeoArrow implementations.

### `st_geomfromgeoarrow(type, value)`

**Signature**
```
GEOMETRY st_geomfromgeoarrow(geometry_type VARCHAR, value <native encoding>)
```

**Description**

Reads any GeoArrow native encoding through a single function, with the geometry type given
as a constant string. Names are case- and separator-insensitive and may carry an explicit
`z`, `m` or `zm` suffix — `'polygon'`, `'POLYGON'`, `'polygon_z'` and `'POLYGON ZM'` all
parse.

The type argument is needed because the DuckDB types collide: LineString and MultiPoint are
both `STRUCT(x, y)[]`, and Polygon and MultiLineString are both `STRUCT(x, y)[][]`, so a
value on its own cannot say which geometry it is. The name resolves the reader at bind time,
so an unknown type or a mismatched shape is a binder error rather than a runtime one.

**Examples**

```sql
SELECT st_geomfromgeoarrow('point', st_asgeoarrowpoint('POINT(30 10)'::GEOMETRY));
-- POINT (30 10)
```

The same value read two ways:
```sql
SELECT st_geomfromgeoarrow('linestring', [{'x': 0.0, 'y': 0.0}, {'x': 1.0, 'y': 1.0}]),
       st_geomfromgeoarrow('multipoint', [{'x': 0.0, 'y': 0.0}, {'x': 1.0, 'y': 1.0}]);
-- LINESTRING (0 0, 1 1)  |  MULTIPOINT (0 0, 1 1)
```

Over a whole column:
```sql
SELECT st_geomfromgeoarrow('polygon', geometry) FROM 'counties.parquet';
```

### `st_geomfromgeoarrow<type>(value)`

**Signature**
```
GEOMETRY st_geomfromgeoarrowpoint(value STRUCT(x DOUBLE, y DOUBLE))
GEOMETRY st_geomfromgeoarrowpolygon(value STRUCT(x DOUBLE, y DOUBLE)[][])
-- ... one per geometry type, each accepting any dimension combination
```

**Description**

The same conversion with the geometry type in the function name instead of an argument.
Useful when you want the shape checked without passing a string literal.

**Example**
```sql
SELECT st_geomfromgeoarrowpolygon([[{'x': 0.0, 'y': 0.0}, {'x': 1.0, 'y': 0.0},
                                    {'x': 1.0, 'y': 1.0}, {'x': 0.0, 'y': 0.0}]]);
-- POLYGON ((0 0, 1 0, 1 1, 0 0))
```

### `st_asgeoarrow<type>(geom[, dims])`

**Signature**
```
<native encoding> st_asgeoarrowpolygon(geom GEOMETRY[, dims VARCHAR])
<native encoding> st_asgeoarrowpolygon(wkb BLOB[, dims VARCHAR])
-- ... one per geometry type
```

**Description**

Converts a `GEOMETRY` (or WKB `BLOB`) into the GeoArrow native encoding for that geometry
type, erroring if the input is not that type. `dims` selects the dimensions and defaults to
`xy` — see [Z and M dimensions](#z-and-m-dimensions).

**Examples**
```sql
SELECT st_asgeoarrowpolygon('POLYGON((0 0, 1 0, 1 1, 0 0))'::GEOMETRY);
-- [[{'x': 0.0, 'y': 0.0}, {'x': 1.0, 'y': 0.0}, {'x': 1.0, 'y': 1.0}, {'x': 0.0, 'y': 0.0}]]

SELECT st_asgeoarrowmultipolygon('MULTIPOLYGON(((0 0, 1 0, 1 1, 0 0)))'::GEOMETRY);
-- [[[{'x': 0.0, 'y': 0.0}, {'x': 1.0, 'y': 0.0}, {'x': 1.0, 'y': 1.0}, {'x': 0.0, 'y': 0.0}]]]
```

Round-trip:
```sql
SELECT st_geomfromgeoarrowpolygon(st_asgeoarrowpolygon('POLYGON((0 0, 4 0, 4 4, 0 4, 0 0))'::GEOMETRY));
-- POLYGON ((0 0, 4 0, 4 4, 0 4, 0 0))
```

## Flat struct

One struct per geometry, holding all coordinates in parallel arrays with offsets marking the
boundaries. Unlike the native encodings, a single type covers every geometry type.

| Field | Type | Description |
|-------|------|-------------|
| `geometry_type` | `UTINYINT` | 1=Point, 2=LineString, 3=Polygon, 4=MultiPoint, 5=MultiLineString, 6=MultiPolygon |
| `xs` | `DOUBLE[]` | All X coordinates |
| `ys` | `DOUBLE[]` | All Y coordinates |
| `zs` | `DOUBLE[]` | All Z coordinates (present only for `xyz` / `xyzm`) |
| `ms` | `DOUBLE[]` | All M values (present only for `xym` / `xyzm`) |
| `ring_offsets` | `INTEGER[]` | Cumulative coordinate count at the end of each ring/part |
| `geom_offsets` | `INTEGER[]` | Cumulative ring count at the end of each sub-geometry (MultiPolygon only) |

### `st_asgeoarrow(geom[, dims])`

**Signature**
```
STRUCT(...) st_asgeoarrow(geom GEOMETRY[, dims VARCHAR])
STRUCT(...) st_asgeoarrow(wkb BLOB[, dims VARCHAR])
```

**Examples**
```sql
SELECT st_asgeoarrow('POINT(30 10)'::GEOMETRY);
-- {'geometry_type': 1, 'xs': [30.0], 'ys': [10.0], 'ring_offsets': [], 'geom_offsets': []}
```

A polygon with an interior ring — `ring_offsets` marks where each ring ends:
```sql
SELECT st_asgeoarrow('POLYGON((0 0, 4 0, 4 4, 0 4, 0 0),(1 1, 2 1, 2 2, 1 1))'::GEOMETRY);
-- {'geometry_type': 3,
--  'xs': [0.0, 4.0, 4.0, 0.0, 0.0, 1.0, 2.0, 2.0, 1.0],
--  'ys': [0.0, 0.0, 4.0, 4.0, 0.0, 1.0, 1.0, 2.0, 1.0],
--  'ring_offsets': [5, 9], 'geom_offsets': []}
```

Access individual fields:
```sql
SELECT (st_asgeoarrow('POINT(30 10)'::GEOMETRY)).xs;
-- [30.0]

SELECT (st_asgeoarrow('LINESTRING(0 0, 1 1, 2 2)'::GEOMETRY)).geometry_type;
-- 2
```

### `st_geomfromgeoarrow(struct)`

**Signature**
```
GEOMETRY st_geomfromgeoarrow(
    geom STRUCT(geometry_type UTINYINT, xs DOUBLE[], ys DOUBLE[],
                [zs DOUBLE[]], [ms DOUBLE[]],
                ring_offsets INTEGER[], geom_offsets INTEGER[])
)
```

**Description**

Converts a flat GeoArrow struct back into a `GEOMETRY`. Supports Point, LineString, Polygon,
MultiPoint, MultiLineString and MultiPolygon. Fields are matched by name, so their order in
the struct does not matter.

**Examples**

Build a LineString from coordinates:
```sql
SELECT st_geomfromgeoarrow({
    'geometry_type': 2::UTINYINT,
    'xs': [0.0, 1.0, 2.0],
    'ys': [0.0, 1.0, 2.0],
    'ring_offsets': []::INTEGER[],
    'geom_offsets': []::INTEGER[]
});
-- LINESTRING (0 0, 1 1, 2 2)
```

Round-trip a column:
```sql
SELECT st_geomfromgeoarrow(st_asgeoarrow(geom_column)) FROM my_table;
```

## Z and M dimensions

Every function supports XY, XYZ, XYM and XYZM. GeoArrow carries the extra ordinates in the
coordinate struct — `STRUCT(x, y, z)`, `STRUCT(x, y, m)`, `STRUCT(x, y, z, m)` — and the
flat struct gains matching `zs` / `ms` lists.

**Reading needs no extra argument.** The value's own type says which ordinates are present:
```sql
SELECT st_geomfromgeoarrow('point', {'x': 1.0, 'y': 2.0, 'z': 3.0});
-- POINT Z (1 2 3)

SELECT st_geomfromgeoarrowpoint({'x': 1.0, 'y': 2.0, 'm': 4.0});
-- POINT M (1 2 4)

SELECT st_geomfromgeoarrowpoint({'x': 1.0, 'y': 2.0, 'z': 3.0, 'm': 4.0});
-- POINT ZM (1 2 3 4)
```

**Writing takes the dimensions as an argument**, because a SQL function's return type has to
be fixed before any data is seen:
```sql
SELECT st_asgeoarrowpoint('POINT Z (1 2 3)'::GEOMETRY, 'xyz');
-- {'x': 1.0, 'y': 2.0, 'z': 3.0}

SELECT typeof(st_asgeoarrowpolygon('POLYGON Z ((0 0 1, 1 0 1, 1 1 1, 0 0 1))'::GEOMETRY, 'xyz'));
-- STRUCT(x DOUBLE, y DOUBLE, z DOUBLE)[][]

SELECT st_asgeoarrow('LINESTRING ZM (0 0 1 5, 1 1 2 6)'::GEOMETRY, 'xyzm');
-- {'geometry_type': 2, 'xs': [0.0, 1.0], 'ys': [0.0, 1.0],
--  'zs': [1.0, 2.0], 'ms': [5.0, 6.0], 'ring_offsets': [], 'geom_offsets': []}
```

Accepted dimension names are `xy`, `xyz`, `xym` and `xyzm` (`z`, `m` and `zm` also work).

Two things worth knowing:

- Omitting the argument means `xy`, so **every one-argument call keeps the exact type it
  always had** — existing queries are unaffected.
- Dropping ordinates is allowed, inventing them is not. `st_asgeoarrowpoint` on a 3D
  geometry projects down to XY happily, but asking for `'xyz'` from a 2D geometry raises an
  error rather than filling in NaNs.

## Notes

- `NULL` input produces `NULL` output. A `NULL` *inside* a list (a null ring, say) is read as
  an empty ring rather than an error, since the GeoArrow native encoding only carries
  validity at the top level.
- The GeoArrow → GEOMETRY functions are thin wrappers: the input vector is exported through
  DuckDB's own Arrow bridge and walked by geoarrow-c's `GeoArrowArrayViewVisitNative`, so all
  nesting, offset, dimension and geometry-type handling comes from the library rather than
  from this extension.
- GeometryCollection, and the `geoarrow.box` and interleaved-coordinate encodings, are not
  supported.

### `duck_geoarrow_version`

**Signature**
```
VARCHAR duck_geoarrow_version()
```

Returns the version of the loaded extension together with the version of the bundled
geoarrow-c library.

```sql
SELECT duck_geoarrow_version();
-- bca85b3 (geoarrow-c 0.2.0-SNAPSHOT)
```

## Building

### Build steps
To build the extension, run:
```sh
make
```

This uses ninja by default for faster builds. The main binaries that will be built are:
```sh
./build/release/duckdb
./build/release/test/unittest
./build/release/extension/duck_geoarrow/duck_geoarrow.duckdb_extension
```
- `duckdb` is the binary for the duckdb shell with the extension code automatically loaded.
- `unittest` is the test runner of duckdb. Again, the extension is already linked into the binary.
- `duck_geoarrow.duckdb_extension` is the loadable binary as it would be distributed.

## Running the tests

The SQL tests in `./test/sql` are the primary test suite:
```sh
make test
```

There are also Python tests that check interoperability against geoarrow-pyarrow, which
SQLLogicTest cannot express. See [`test/README.md`](test/README.md) for how to run those and
for the provenance of the fixture files in `test/data`.

Before pushing, the same checks CI runs:
```sh
make format-check   # clang-format 11.0.1 and black 24
make tidy-check     # clang-tidy
```

`format-check` needs **black 24** specifically (`pip install 'black==24.*'`); newer versions
format differently and CI will reject their output.

### Installing the deployed binaries
To install your extension binaries from S3, you will need to do two things. Firstly, DuckDB should be launched with the
`allow_unsigned_extensions` option set to true. How to set this will depend on the client you're using. Some examples:

CLI:
```shell
duckdb -unsigned
```

Python:
```python
con = duckdb.connect(':memory:', config={'allow_unsigned_extensions' : 'true'})
```

NodeJS:
```js
db = new duckdb.Database(':memory:', {"allow_unsigned_extensions": "true"});
```

After running these steps, you can install and load your extension using the regular INSTALL/LOAD commands in DuckDB:
```sql
INSTALL duck_geoarrow;
LOAD duck_geoarrow;
```
