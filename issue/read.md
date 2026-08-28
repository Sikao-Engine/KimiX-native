# issue/read.md — read tool: blockers for the remaining Python-only branches

The pure-CPU kernels of the `read` tool were ported to C++
(`src/builtin_tools/read_tool.h/.cpp` — line rendering, char windowing,
line-hash/repeated-line collapse, V8 `.cpuprofile` summarizer, macOS `sample`
parser, `markdown_to_text`; see `src/builtin_tools/reports/read.md`).

The branches below **cannot** be ported with the libraries vendored in
`src/ext` (`cpp-httplib`, `mbedtls`, `mimalloc`, `pybind11`, `reproc`,
`xxHash`, `yyjson`). Per the task rules no new third-party library may be
vendored, so these stay in Python in kimi-agent.

## 1. Archive member extraction — no decompressor vendored

Python: `read_archive.py` (`ArchiveReader`) reads zip/tar/tar.gz/tgz/tar.bz2/
tar.xz members and bare gz/bz2/xz files via the C-backed stdlib
`zipfile`/`tarfile`/`gzip`/`bz2`/`lzma`.

* **What is missing:** an inflate/deflate (zip, gz) and bzip2/xz decoder.
  Nothing in `src/ext` decompresses anything.
* **What the library buys:** member content extraction (the core of the
  `archive_member` parameter) and archive listing with sizes.
* **Candidates evaluated (none vendored):**
  * `libdeflate` — small (~4 files, MIT), fastest raw inflate/deflate; covers
    zip + gzip but not tar metadata, bz2, or xz.
  * `zlib` — the classic (zlib license, ~150 KB source); same format coverage
    as libdeflate.
  * `miniz` / `minizip-ng` — single-file inflate + zip reader (public domain /
    MIT); minimal integration cost but zip-only.
  * `libarchive` — BSD-2-Clause, one library for zip/tar/gz/bz2/xz/7z; the
    only candidate covering the full Python matrix, but the largest
    (~1.5 MB source) and needs its own crypto/codec configuration.
* **Ships instead:** nothing on the C++ side for archives; the Python path
  keeps full behavior. The plan already keeps `normalize_member_path` /
  `sniff_archive` / `is_binary_data` in Python as trivial boundary code.

## 2. PDF — no renderer/rasterizer vendored

Python: `read_pdf_pages.py::render_pdf_page` uses **PyMuPDF** (`fitz`) to
rasterize a page to PNG and the image pipeline to compress it.

* **What is missing:** a PDF interpreter + rasterizer and a PNG encoder.
* **What the library buys:** `pdf_page=N` screenshots (the `image_in` model
  capability path).
* **Candidates evaluated:** `PyMuPDF`/AGPL `mupdf` (AGPL — license-incompatible
  for vendoring here), `poppler` (GPL-2+, depends on freetype/fontconfig,
  far too heavy). No permissive, self-contained PDF renderer is acceptable.
* **Ships instead:** PDF reading stays in Python; plain-text PDF fallbacks
  (when no `pdf_page` is requested) already go through the normal text render
  engine, which is ported.

## 3. Document extraction — DOCX / XLSX / XLS / PPTX / IPYNB

Python: `read_extract.py` + `read_markit.py` use `python-docx`, `openpyxl`,
`xlrd`, `python-pptx`, `nbformat`.

* **What is missing, in two layers:**
  1. **All Office formats are zip containers** (`.docx`/`.xlsx`/`.pptx` are
     OOXML zip packages) — blocked by item 1 first: without an inflate
     implementation no member can be read at all.
  2. On top of that, an XML reader (we have yyjson but no XML parser) and
     format-specific document models (styles, shared strings, cell refs,
     slide layouts, notebook cell JSON).
* **What the libraries buy:** `python-docx` (~250 KB, MIT) paragraphs/tables/
  heading styles; `openpyxl` (MIT) worksheet cells + shared strings;
  `xlrd` (BSD) legacy `.xls` binary BIFF parsing; `python-pptx` (MIT) slide
  text; `nbformat` (BSD) notebook cell normalization. `.ipynb` is plain JSON
  (yyjson *could* parse it) but the plan keeps it with the other extractors
  because the markdown post-processing depends on the same Python objects.
* **Ships instead:** extraction stays in Python. The *markdown post-processing*
  that is pure text (`markdown_to_text`) and the generic document render path
  (`_read_content` ≙ `render_forward` over split lines) **are** ported, so
  once Python hands over extracted text, everything downstream runs natively.

## 4. HTML → text — owned by fetch_url

Python: `read_markit.py::html_to_text` uses `markdownify` (HTML → markdown)
then `markdown_to_text`.

* **Why not ported here:** the plan/README assign the HTML pipeline to the
  `fetch_url` agent (cross-tool ownership map); porting an HTML tokenizer
  twice would duplicate work and risk divergent output. `markdown_to_text`
  itself *is* ported, so fetch_url only needs the HTML→markdown half.

## 5. Image / media rendering — no codecs vendored

Python: image reading is rejected with a dedicated ToolError; PDF pages are
rasterized (item 2). There is no image decode path to port.

* **What is missing:** JPEG/PNG/WebP codecs + a renderer. Candidates
  (`stb_image`, public domain) exist but are out of scope for this task and
  were not requested by the plan (read refuses images by design).

## Summary

| Branch | Missing library class | Status |
|---|---|---|
| Archive member read | decompressor (libdeflate/zlib/miniz/libarchive) | blocked — Python keeps it |
| PDF page render | PDF interpreter + rasterizer (mupdf/poppler) | blocked — Python keeps it |
| DOCX/XLSX/XLS/PPTX | zip decompressor + XML + format models | blocked (zip blocker first) |
| IPYNB | nbformat (kept with the document group per plan) | Python keeps it |
| HTML → text | markdownify | owned by fetch_url |
| Images | codecs + renderer | out of scope by design |

Everything not listed above — the CPU-bound text rendering core, char
windowing, profile summarizers, and markdown conversion — ships in C++ in
this change.
