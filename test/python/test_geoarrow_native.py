"""Verify st_asgeoarrow<type> functions produce canonical GeoArrow native arrays.

For each of the six geometry types we:
  1. project a WKT literal through st_asgeoarrow<type>
  2. export to Arrow
  3. wrap in the matching geoarrow.pyarrow extension type
  4. read back WKT and check round-trip
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
        "st_asgeoarrowpoint",
        ga.point().with_coord_type(ga.CoordType.SEPARATED),
        ["POINT(30 10)", "POINT(-1.5 2.25)"],
    ),
    (
        "st_asgeoarrowlinestring",
        ga.linestring().with_coord_type(ga.CoordType.SEPARATED),
        ["LINESTRING(0 0, 1 1, 2 2)", "LINESTRING(3 4, 5 6)"],
    ),
    (
        "st_asgeoarrowpolygon",
        ga.polygon().with_coord_type(ga.CoordType.SEPARATED),
        [
            "POLYGON((0 0, 4 0, 4 4, 0 4, 0 0))",
            "POLYGON((0 0, 10 0, 10 10, 0 10, 0 0), (2 2, 3 2, 3 3, 2 3, 2 2))",
        ],
    ),
    (
        "st_asgeoarrowmultipoint",
        ga.multipoint().with_coord_type(ga.CoordType.SEPARATED),
        ["MULTIPOINT((1 1), (2 2), (3 3))", "MULTIPOINT((0 0))"],
    ),
    (
        "st_asgeoarrowmultilinestring",
        ga.multilinestring().with_coord_type(ga.CoordType.SEPARATED),
        [
            "MULTILINESTRING((0 0, 1 1), (2 2, 3 3, 4 4))",
            "MULTILINESTRING((5 5, 6 6))",
        ],
    ),
    (
        "st_asgeoarrowmultipolygon",
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

    for fn, ext_type, wkts in CASES:
        print(f"\n=== {fn} ===")
        values_sql = ",\n          ".join(f"('{w}'::GEOMETRY)" for w in wkts)
        sql = f"""
            SELECT {fn}(g) AS geom
            FROM (VALUES
                  {values_sql}
            ) AS t(g)
        """
        tbl = _arrow_table(con, sql)
        col = tbl.column("geom").combine_chunks()
        print("  arrow type:", col.type)

        wrapped = ext_type.wrap_array(col)
        back = ga.as_wkt(wrapped).to_pylist()

        # geoarrow normalizes WKT formatting (spaces, etc.); compare via
        # round-tripping the input WKT through the same normalizer.
        expected = ga.as_wkt(ga.as_geoarrow(pa.array(wkts), type=ext_type)).to_pylist()

        assert back == expected, f"{fn} mismatch:\n  got:  {back}\n  want: {expected}"
        print("  round-trip OK")
        for w in back:
            print("   ", w)


if __name__ == "__main__":
    main()
