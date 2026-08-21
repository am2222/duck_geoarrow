# Test data

## `ms_counties.parquet`

A real GeoParquet file for testing the GeoArrow read path against output from another
implementation, rather than only against this extension's own output.

| | |
|---|---|
| Contents | The 82 counties of Mississippi, one Polygon each (6,957 vertices total), EPSG:4326 |
| Written by | geopandas 1.1.3 / pyarrow 18.1.0 |
| GeoParquet | 1.1.0, `"primary_column": "geometry"`, `"encoding": "polygon"` |
| Arrow type | `geoarrow.polygon` — native GeoArrow, separated coordinates |
| DuckDB type | `STRUCT(x DOUBLE, y DOUBLE)[][]` |
| Size | ~103 KB, snappy |

The geometry is US Census TIGER county boundary data (public domain), as are `county_name`,
`geoid` and `aland`. `geometry_bbox` is the per-row bounding box geopandas computed and
stored as a GeoParquet `covering` — `test/sql/geoarrow_parquet.test` uses it as an
independent check that the coordinates we read back are the right ones, since it was
produced by geopandas rather than by us.

The file was derived from an internal dataset by selecting those columns; three unrelated
internal columns were dropped. The geometry and bbox values are byte-for-byte the
originals, and the `geo` metadata is unchanged. The `pandas` metadata block was dropped
because it described the removed columns.

This is the one encoding a `geoarrow.polygon` writer actually emits, so it exercises
`st_geomfromgeoarrowpolygon` end to end: file → Arrow → `GeoArrowArrayViewVisitNative` →
WKB → `GEOMETRY`.
