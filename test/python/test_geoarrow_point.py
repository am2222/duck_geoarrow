"""Verify st_asgeoarrowpoint produces a column readable by geoarrow-pyarrow.

Runs:
  1. Load the built duck_geoarrow extension into an in-process duckdb.
  2. Build a table of POINTs, project via st_asgeoarrowpoint, export to Arrow.
  3. Stamp the `geoarrow.point` Arrow extension on the column and hand it to
     geoarrow.pyarrow to interpret.
  4. Assert the coordinates round-trip intact.
"""

from pathlib import Path

import duckdb
import pyarrow as pa
import geoarrow.pyarrow as ga


REPO_ROOT = Path(__file__).resolve().parents[2]
EXT_PATH = REPO_ROOT / "build/release/extension/duck_geoarrow/duck_geoarrow.duckdb_extension"


def main() -> None:
    assert EXT_PATH.exists(), f"extension not built at {EXT_PATH}"

    con = duckdb.connect(config={"allow_unsigned_extensions": "true"})
    con.execute(f"LOAD '{EXT_PATH}'")

    con.execute(
        """
        CREATE TABLE pts AS
        SELECT * FROM (VALUES
            ('POINT(30 10)'::GEOMETRY),
            ('POINT(-1.5 2.25)'::GEOMETRY),
            ('POINT(0 0)'::GEOMETRY)
        ) AS t(g)
        """
    )

    # Use arrow() to export the column as an Arrow table.
    reader = con.execute("SELECT st_asgeoarrowpoint(g) AS pt FROM pts").arrow()
    arrow_tbl: pa.Table = reader.read_all() if hasattr(reader, "read_all") else reader

    print("Raw duckdb Arrow schema:")
    print(arrow_tbl.schema)
    print(arrow_tbl.to_pydict())

    pt_col = arrow_tbl.column("pt").combine_chunks()

    # Canonical GeoArrow native Point is Struct<x:double, y:double>.
    assert pt_col.type == pa.struct([("x", pa.float64()), ("y", pa.float64())]), (
        f"unexpected type: {pt_col.type}"
    )

    # Stamp the geoarrow.point extension on the field (duckdb doesn't attach
    # Arrow extension metadata from a UDF return type).
    point_type = ga.point().with_coord_type(ga.CoordType.SEPARATED)
    print("\nGeoArrow extension type:", point_type)

    storage = pt_col
    # Build an ExtensionArray around the storage.
    ext_array = point_type.wrap_array(storage)
    print("\nInterpreted as GeoArrow:")
    print(ext_array)
    print("WKT:", ga.as_wkt(ext_array))

    # Round-trip check: extract x/y and compare to input.
    xs = pt_col.field("x").to_pylist()
    ys = pt_col.field("y").to_pylist()
    expected = [(30.0, 10.0), (-1.5, 2.25), (0.0, 0.0)]
    got = list(zip(xs, ys))
    assert got == expected, f"coords mismatch: {got} != {expected}"
    print("\nround-trip OK:", got)


if __name__ == "__main__":
    main()
