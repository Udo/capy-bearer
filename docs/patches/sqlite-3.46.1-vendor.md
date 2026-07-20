# SQLite 3.46.1 Vendor Import

Imported the official SQLite amalgamation without local source modifications.

- Version: SQLite 3.46.1
- Upstream archive: https://www.sqlite.org/2024/sqlite-amalgamation-3460100.zip
- Import date: 2026-05-29
- Vendored files:
  - `src/3rdparty/sqlite/sqlite3.c`
  - `src/3rdparty/sqlite/sqlite3.h`
  - `src/3rdparty/sqlite/sqlite3ext.h`

## Checksums

```text
77823cb110929c2bcb0f5d48e4833b5c59a8a6e40cdea3936b99e199dbbe5784  sqlite-amalgamation-3460100.zip
6c35bc5f7f85eac9c49928bacbb02bb694b547aabf69197e058cca245ad80e83  sqlite3.c
89b62c671c5964e137409ce034941b7b05a3af2c9875aba41f47f9483d0c2515  sqlite3.h
b184dd1586d935133d37ad76fa353faf0a1021ff2fdedeedcc3498fff74bbb94  sqlite3ext.h
```

## Runtime compile flags

The amalgamation is included by `src/lib/uce_lib.cpp` with:

```cpp
#define SQLITE_THREADSAFE 1
#define SQLITE_OMIT_LOAD_EXTENSION 1
#define SQLITE_DQS 0
#define SQLITE_DEFAULT_FOREIGN_KEYS 1
#define SQLITE_DEFAULT_WAL_SYNCHRONOUS 1
```

No patch diff is needed because the vendored SQLite files are unmodified.
