"""Verify st_geomfromgeoarrow<type> reads canonical GeoArrow native arrays.

The arrays here are built by geoarrow-pyarrow, an independent implementation, so this
checks we consume third-party GeoArrow rather than only our own output. For each of the
six geometry types we:

  1. build a native (separated-coordinate) array with geoarrow.pyarrow
  2. hand its plain nested storage to DuckDB
  3. read it with st_geomfromgeoarrow<type>
  4. write it straight back out with st_asgeoarrow<type>
  5. assert the array that comes back is identical to the one we started with
"""

from pathlib import Path

import duckdb
import pyarrow as pa
import geoarrow.pyarrow as ga

REPO_ROOT = Path(__file__).resolve().parents[2]
EXT_PATH = REPO_ROOT / "build/release/extension/duck_geoarrow/duck_geoarrow.duckdb_extension"


def _arrow_table(con, sql: str) -> pa.Table:
    reader = con.execute(sql).arrow()
    return reader.read_all() if hasattr(reader, "read_all") else reader


CASES = [
    (
        "point",
        ga.point().with_coord_type(ga.CoordType.SEPARATED),
        ["POINT(30 10)", "POINT(-1.5 2.25)"],
    ),
    (
        "linestring",
        ga.linestring().with_coord_type(ga.CoordType.SEPARATED),
        ["LINESTRING(0 0, 1 1, 2 2)", "LINESTRING(3 4, 5 6)"],
    ),
    (
        "polygon",
        ga.polygon().with_coord_type(ga.CoordType.SEPARATED),
        [
            "POLYGON((0 0, 4 0, 4 4, 0 4, 0 0))",
            "POLYGON((0 0, 10 0, 10 10, 0 10, 0 0), (2 2, 3 2, 3 3, 2 3, 2 2))",
        ],
    ),
    (
        "multipoint",
        ga.multipoint().with_coord_type(ga.CoordType.SEPARATED),
        ["MULTIPOINT((1 1), (2 2), (3 3))", "MULTIPOINT((0 0))"],
    ),
    (
        "multilinestring",
        ga.multilinestring().with_coord_type(ga.CoordType.SEPARATED),
        [
            "MULTILINESTRING((0 0, 1 1), (2 2, 3 3, 4 4))",
            "MULTILINESTRING((5 5, 6 6))",
        ],
    ),
    (
        "multipolygon",
        ga.multipolygon().with_coord_type(ga.CoordType.SEPARATED),
        [
            "MULTIPOLYGON(((0 0, 1 0, 1 1, 0 1, 0 0)), ((2 2, 3 2, 3 3, 2 3, 2 2)))",
            "MULTIPOLYGON(((0 0, 10 0, 10 10, 0 10, 0 0), (2 2, 3 2, 3 3, 2 3, 2 2)))",
        ],
    ),
]


def main() -> None:
    assert EXT_PATH.exists(), f"extension not built at {EXT_PATH}"

    con = duckdb.connect(config={"allow_unsigned_extensions": "true"})
    con.execute(f"LOAD '{EXT_PATH}'")

    for name, ext_type, wkts in CASES:
        read_fn = f"st_geomfromgeoarrow{name}"
        write_fn = f"st_asgeoarrow{name}"
        print(f"\n=== {read_fn} ===")

        source = ga.as_geoarrow(pa.array(wkts), type=ext_type)
        assert source is not None, f"{name}: geoarrow.pyarrow produced no array"
        storage = pa.chunked_array([source.storage]).combine_chunks()
        con.register("geoarrow_input", pa.table({"geom": storage}))
        print("  arrow type in:", storage.type)

        # The geometries as DuckDB sees them, for eyeballing
        for (wkt,) in con.execute(f"SELECT {read_fn}(geom)::VARCHAR FROM geoarrow_input").fetchall():
            print("   ", wkt)

        # Read into GEOMETRY and write straight back out: must be a lossless identity
        back = (
            _arrow_table(con, f"SELECT {write_fn}({read_fn}(geom)) AS geom FROM geoarrow_input")
            .column("geom")
            .combine_chunks()
        )
        # Compare values, not types: geoarrow marks the coordinate children "not null"
        # while DuckDB's Arrow export leaves them nullable. The data must be identical.
        assert (
            back.to_pylist() == storage.to_pylist()
        ), f"{read_fn} mismatch:\n  got:  {back.to_pylist()}\n  want: {storage.to_pylist()}"
        print("  arrow type out:", back.type)
        print("  round-trip OK")

        # NULLs survive the trip
        row = con.execute(
            f"SELECT count(*) FILTER (WHERE {read_fn}(geom) IS NULL) FROM ("
            f"  SELECT geom FROM geoarrow_input UNION ALL SELECT NULL"
            f")"
        ).fetchone()
        assert row is not None
        nulls = row[0]
        assert nulls == 1, f"{read_fn}: expected 1 NULL, got {nulls}"
        print("  NULL handling OK")

        con.unregister("geoarrow_input")


if __name__ == "__main__":
    main()
