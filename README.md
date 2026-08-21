# Duck_Geoarrow


This extension, Duck_Geoarrow, provides functions to convert between WKB (Well-Known Binary) geometry and the [GeoArrow](https://geoarrow.org/) coordinate encoding, powered by the [geoarrow-c](https://github.com/geoarrow/geoarrow-c) library.

## Functions

### `duck_geoarrow_version`

**Signature**
```
VARCHAR duck_geoarrow_version()
```

**Description**

Returns the version string of the loaded duck_geoarrow extension together with the version of the bundled geoarrow-c library.

**Example**
```sql
SELECT duck_geoarrow_version();
```
```
┌─────────────────────────────────────┐
│       duck_geoarrow_version()       │
│               varchar               │
├─────────────────────────────────────┤
│ 3909d51 (geoarrow-c 0.2.0-SNAPSHOT) │
└─────────────────────────────────────┘
```

### `st_asgeoarrow`

**Signature**
```
STRUCT(...) st_asgeoarrow(geom GEOMETRY)
STRUCT(...) st_asgeoarrow(wkb BLOB)
```

**Description**

Converts a DuckDB GEOMETRY (or WKB BLOB) into a GeoArrow struct with separated coordinate arrays. The output struct contains:

| Field | Type | Description |
|-------|------|-------------|
| `geometry_type` | `UTINYINT` | Geometry type (1=Point, 2=LineString, 3=Polygon, 4=MultiPoint, 5=MultiLineString, 6=MultiPolygon) |
| `xs` | `DOUBLE[]` | All X coordinates |
| `ys` | `DOUBLE[]` | All Y coordinates |
| `ring_offsets` | `INTEGER[]` | Cumulative coordinate count at the end of each ring/part |
| `geom_offsets` | `INTEGER[]` | Cumulative ring count at the end of each sub-geometry (MultiPolygon only) |

**Examples**

```sql
SELECT st_asgeoarrow('POINT(30 10)'::GEOMETRY);
```
```
┌─────────────────────────────────────────────────────────────────────────────────┐
│                     st_asgeoarrow(CAST('POINT(30 10)' AS GEOMETRY))            │
├─────────────────────────────────────────────────────────────────────────────────┤
│ {'geometry_type': 1, 'xs': [30.0], 'ys': [10.0], 'ring_offsets': [],          │
│  'geom_offsets': []}                                                           │
└─────────────────────────────────────────────────────────────────────────────────┘
```

Access individual fields:
```sql
SELECT (st_asgeoarrow('POINT(30 10)'::GEOMETRY)).xs;
-- [30.0]

SELECT (st_asgeoarrow('LINESTRING(0 0, 1 1, 2 2)'::GEOMETRY)).geometry_type;
-- 2
```

### `st_geomfromgeoarrow`

**Signature**
```
GEOMETRY st_geomfromgeoarrow(
    geom STRUCT(geometry_type UTINYINT, xs DOUBLE[], ys DOUBLE[],
                ring_offsets INTEGER[], geom_offsets INTEGER[])
)
```

**Description**

Converts a GeoArrow struct (as produced by `st_asgeoarrow`) back into a DuckDB GEOMETRY. Supports Point, LineString, Polygon, MultiPoint, MultiLineString, and MultiPolygon geometry types.

**Examples**

Construct a LineString from coordinates:
```sql
SELECT st_geomfromgeoarrow({
    'geometry_type': 2::UTINYINT,
    'xs': [0.0, 1.0, 2.0],
    'ys': [0.0, 1.0, 2.0],
    'ring_offsets': []::INTEGER[],
    'geom_offsets': []::INTEGER[]
});
```
```
┌──────────────────────────────┐
│   st_geomfromgeoarrow(...)   │
│           geometry           │
├──────────────────────────────┤
│ LINESTRING (0 0, 1 1, 2 2)  │
└──────────────────────────────┘
```

Roundtrip GEOMETRY through GeoArrow:
```sql
SELECT st_geomfromgeoarrow(st_asgeoarrow(geom_column)) FROM my_table;
```

### GeoArrow native encodings

The functions above use a single flattened struct for every geometry type. The functions
below use the [GeoArrow native encodings](https://geoarrow.org/format) instead — nested
lists of separated (struct-of-x/y) coordinates, the layout GeoParquet, geoarrow-pyarrow
and geopandas produce.

| Geometry type | DuckDB type | GEOMETRY → GeoArrow | GeoArrow → GEOMETRY |
|---|---|---|---|
| Point | `STRUCT(x, y)` | `st_asgeoarrowpoint` | `st_geomfromgeoarrowpoint` |
| LineString | `STRUCT(x, y)[]` | `st_asgeoarrowlinestring` | `st_geomfromgeoarrowlinestring` |
| Polygon | `STRUCT(x, y)[][]` | `st_asgeoarrowpolygon` | `st_geomfromgeoarrowpolygon` |
| MultiPoint | `STRUCT(x, y)[]` | `st_asgeoarrowmultipoint` | `st_geomfromgeoarrowmultipoint` |
| MultiLineString | `STRUCT(x, y)[][]` | `st_asgeoarrowmultilinestring` | `st_geomfromgeoarrowmultilinestring` |
| MultiPolygon | `STRUCT(x, y)[][][]` | `st_asgeoarrowmultipolygon` | `st_geomfromgeoarrowmultipolygon` |

The `st_asgeoarrow*` functions accept `GEOMETRY` or `BLOB` (WKB) and error if the input is
not the expected geometry type. The `st_geomfromgeoarrow*` functions return `GEOMETRY`.

```sql
SELECT st_geomfromgeoarrowpolygon(st_asgeoarrowpolygon('POLYGON((0 0, 4 0, 4 4, 0 4, 0 0))'::GEOMETRY));
-- POLYGON ((0 0, 4 0, 4 4, 0 4, 0 0))
```

### `st_geomfromgeoarrow` (generic form)

**Signature**
```
GEOMETRY st_geomfromgeoarrow(geometry_type VARCHAR, value <native encoding>)
```

**Description**

Reads any GeoArrow native encoding through a single function, with the geometry type given
as a constant string: `point`, `linestring`, `polygon`, `multipoint`, `multilinestring` or
`multipolygon`. Names are case- and separator-insensitive and may carry an explicit `z`,
`m` or `zm` suffix.

The type argument is required because the DuckDB types collide: LineString and MultiPoint
are both `STRUCT(x, y)[]`, and Polygon and MultiLineString are both `STRUCT(x, y)[][]`, so
a value on its own cannot say which geometry it is.

**Examples**

```sql
SELECT st_geomfromgeoarrow('polygon', geometry) FROM 'counties.parquet';

-- the same value read two ways
SELECT st_geomfromgeoarrow('linestring', [{'x': 0.0, 'y': 0.0}, {'x': 1.0, 'y': 1.0}]),
       st_geomfromgeoarrow('multipoint', [{'x': 0.0, 'y': 0.0}, {'x': 1.0, 'y': 1.0}]);
-- LINESTRING (0 0, 1 1)  |  MULTIPOINT (0 0, 1 1)
```

Reading a GeoArrow-encoded GeoParquet file written by geopandas — a column written with
`encoding="polygon"` arrives in DuckDB as `STRUCT(x DOUBLE, y DOUBLE)[][]`:
```sql
SELECT county_name, st_geomfromgeoarrow('polygon', geometry) AS geom
FROM read_parquet('county_regions.parquet');
```

### Z and M dimensions

All functions support XY, XYZ, XYM and XYZM. GeoArrow carries the extra ordinates in the
coordinate struct (`STRUCT(x, y, z)`, `STRUCT(x, y, m)`, `STRUCT(x, y, z, m)`), and the
flat struct gains matching `zs` / `ms` lists.

**Reading** needs no extra argument — the value's own type says which ordinates are
present:
```sql
SELECT st_geomfromgeoarrow('point', {'x': 1.0, 'y': 2.0, 'z': 3.0});   -- POINT Z (1 2 3)
SELECT st_geomfromgeoarrowpoint({'x': 1.0, 'y': 2.0, 'm': 4.0});       -- POINT M (1 2 4)
```

**Writing** takes the dimensions as a second argument, because a SQL function's return type
has to be fixed before any data is seen:
```sql
SELECT st_asgeoarrowpoint('POINT Z (1 2 3)'::GEOMETRY, 'xyz');
-- {'x': 1.0, 'y': 2.0, 'z': 3.0}

SELECT typeof(st_asgeoarrowpolygon('POLYGON Z ((0 0 1, 1 0 1, 1 1 1, 0 0 1))'::GEOMETRY, 'xyz'));
-- STRUCT(x DOUBLE, y DOUBLE, z DOUBLE)[][]

SELECT st_asgeoarrow('LINESTRING ZM (0 0 1 5, 1 1 2 6)'::GEOMETRY, 'xyzm');
-- {'geometry_type': 2, 'xs': [...], 'ys': [...], 'zs': [1.0, 2.0], 'ms': [5.0, 6.0], ...}
```

Accepted dimension names are `xy`, `xyz`, `xym`, `xyzm` (`z` and `zm` also work). Omitting
the argument means `xy`, so **every one-argument call keeps the exact type it always had**
and existing queries are unaffected.

Dropping ordinates is allowed, inventing them is not: asking for `xyz` from a 2D geometry
raises an error rather than filling in NaNs.

**Notes**

- `NULL` input produces `NULL` output. A `NULL` *inside* a list (a null ring, say) is read
  as an empty ring rather than an error, since the GeoArrow native encoding only carries
  validity at the top level.
- The `st_geomfromgeoarrow*` functions are thin wrappers: the input vector is exported
  through DuckDB's Arrow bridge and walked by `geoarrow-c`'s
  `GeoArrowArrayViewVisitNative`, so all nesting, offset, dimension and geometry-type
  handling comes from the library rather than from this extension.

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
Different tests can be created for DuckDB extensions. The primary way of testing DuckDB extensions should be the SQL tests in `./test/sql`. These SQL tests can be run using:
```sh
make test
```

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
