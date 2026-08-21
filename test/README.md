# Testing this extension
This directory contains all the tests for this extension. The `sql` directory holds tests that are written as [SQLLogicTests](https://duckdb.org/dev/sqllogictest/intro.html). DuckDB aims to have most its tests in this format as SQL statements, so for the duck_geoarrow extension, this should probably be the goal too.

The root makefile contains targets to build and run all of these tests. To run the SQLLogicTests:
```bash
make test
```
or 
```bash
make test_debug
```
The `data` directory holds fixture files the SQL tests read; see `data/README.md` for
their provenance.

## Python tests
The `python` directory holds checks that need a real Arrow/GeoArrow implementation on the
other side, which SQLLogicTest cannot express. They require `duckdb`, `pyarrow` and
`geoarrow-pyarrow`, and they load the built extension from
`build/release/extension/duck_geoarrow/`, so run `make release` first.

```bash
python test/python/test_geoarrow_point.py
python test/python/test_geoarrow_native.py       # GEOMETRY -> GeoArrow native
python test/python/test_geoarrow_native_read.py  # GeoArrow native -> GEOMETRY
```
