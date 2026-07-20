# miniz vendored for BEARER

BEARER vendors miniz 3.0.2 from <https://github.com/richgel999/miniz> for ZIP archive support.

Files are kept under `src/3rdparty/miniz/` and are included by `src/lib/zip.cpp` so BEARER can expose `zip_create()`, `zip_list()`, `zip_read()`, `zip_extract()`, `gz_compress()`, and `gz_uncompress()` without adding a runtime package dependency.

Local patch:

- `miniz_export.h` is provided as a tiny static-build compatibility header defining `MINIZ_EXPORT`.
- `miniz_tdef.c` moves the `s_tdefl_num_probes` initializer before its first use so the vendored C source can be included in BEARER's single C++ translation-unit build.

The upstream license is preserved in `LICENSE`.
