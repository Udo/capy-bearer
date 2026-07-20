# DOCS-TODO — `/doc` documentation overhaul

Briefing for the doc-system overhaul. Read this fully before touching anything.
The approved design and the two locked decisions are: **(1) examples are LIVE and
SELF-VERIFYING** — real BEARER code executed at render time, showing source + actual
captured output, gated by the test suite; **(2) FULL SWEEP** of all 257 pages.

The docs live under `site/doc/`:
- `index.uce` — the renderer (index + per-page detail view).
- `lib/doc_page.h` — page parsing (`load_doc_page`) + label/kind helpers.
- `style.css` — theme + layout.
- `pages/*.txt` — 257 page source files (the corpus).
- `areas/*.txt` — area groupings; first line is the area title, following lines
  are page slugs (or `>area` references) that populate the index sidebar.
- `search.uce`, `singlepage.uce` — search + single-page render.

## Why this work exists (the problems)

- **Title bug:** 15 pages carry a `:title` that literally repeats the raw filename,
  so `0_StringList` renders as "0_StringList" instead of "StringList". The `:title`
  override defeats the prefix-stripping label logic already in `doc_page.h`
  (`doc_default_title` / `doc_index_label` / `doc_method_label`).
- **31 slop/stub pages:** the newest APIs (`file_*`, `crypto_equal`, `sha256*`,
  `hmac*`, `random_bytes`, `http_request*`, `job_*`, `shell_spawn`) are throwaway
  `# name` + one-line markdown with NO `:sig`/`:params`/`:see`/`:content`.
- **134 of 257 pages have no code example.** We want PHP-manual-style
  code-to-output examples on EVERY page.
- **Sidebar overflow:** long monospace identifiers in the 280px index sidebar and
  220px detail sidebar have no wrapping → they spill.
- **Relevancy:** pages lead with cross-membrane mechanics and "Related Concepts"
  PHP/JS filler instead of usage; cross-references between related pages are missing.

## Principles for every page

1. **Usage first.** The opening `:content` sentence says what the function does and
   when you'd reach for it — in plain terms. No membrane/ABI talk up top.
2. **A real example, always.** Every page has at least one `:example` whose output
   is produced by actually running it. Examples are the primary teaching tool.
3. **Cross-link.** Every page's `:see` references its area (`>area`) and its closest
   sibling APIs, so the reader is never left to search alone.
4. **Demote the drivel.** Cross-membrane behavior, lock semantics, and PHP/JS
   equivalents are at most a short trailing note — never the body. Delete filler
   that teaches nothing.

---

## Phase 1 — Infrastructure (do this FIRST, verify on host before any content)

### 1a. Title bug
- In `lib/doc_page.h` `load_doc_page`: when a `:title` value, trimmed, **equals the
  page slug**, ignore it (leave `result.title` empty) so the renderer falls through
  to `doc_default_title`, which already strips `0_/1_/2_/3_` prefixes and renders
  `Class::method` for `2_*`.
- Strip the 15 redundant `:title` blocks from their pages (`0_StringList`, `cli_arg`,
  `cli_input`, `list_filter`, `list_map`, `map`, `request_base_url`,
  `request_query_path`, `request_query_route`, `request_script_url`,
  `route_path_is_safe`, `route_path_normalize`, `route_path_sanitize`, `brb_encode`,
  `brb_decode`).
- Fix garbage `:sig` lines on struct pages (e.g. `0_StringList`'s sig is literally
  `0_StringList`): give struct pages a real one-line type summary or drop the sig box.

### 1b. Live example mechanism — EXACT SPEC

Primitives (already confirmed to exist): `unit_compile(path)`, `unit_render(path)`,
`ob_start()`, `ob_get_close()`. Relative paths in the doc unit resolve against
`site/doc/`.

- **Parse:** add `:example` handling to `load_doc_page` → `DocPage.examples`
  (a list; multiple `:example` blocks per page allowed). Capture the raw body lines
  verbatim (this is both the displayed source AND the executed code).
- **Materialize:** for each example, write the body wrapped in a minimal unit to a
  **stable** path `examples/_gen/<slug>_<n>.uce`:
  ```
  RENDER(Request& context)
  {
      <example body>
  }
  ```
  Write **only if the content changed** (compare to existing file) so the runtime's
  compiled-unit cache stays warm. The `_gen` dir must be writable by the worker
  (www-data) — created/`chown`ed once on the host (see Setup below).
- **Execute + capture:** `unit_compile("examples/_gen/<slug>_<n>.uce");` then
  `ob_start(); unit_render("examples/_gen/<slug>_<n>.uce"); String out = ob_get_close();`
- **Render:** an "Example" `doc-section` showing the source block, then the captured
  output beneath it under an "Output" label (PHP-manual style). If the example
  traps or output is empty, **surface that visibly** (do not swallow it) — a broken
  example must be obvious so the gate catches it.
- Examples must be **self-contained and deterministic** — no wall-clock/random output
  shown as canonical (if a function is inherently non-deterministic, show a
  representative call and describe the shape, or seed it). The gate asserts presence
  of an output block + absence of an error marker, not an exact value, EXCEPT where
  the value is deterministic.

### 1c. Sidebar + example CSS (`style.css`)
- Add `overflow-wrap: anywhere; word-break: break-word;` to `.category li a`,
  `.func-item a`, and `.sidebar-card div`.
- Style the example Output block so it's visually distinct from the source (subtle
  border + an "Output" label), reusing existing `--bg-code`/`--border` tokens.

### 1d. Format spec page
- Create `pages/3_Documentation format.txt` (an `info` page) documenting the canonical
  page template below, so the format is self-describing inside the docs.

### Canonical page template

```
:sig
<one or more real C++ signatures, exactly as declared>

:params
<name> : <what it is>
return value : <what comes back>

:content
<Usage-first prose. What it does, when to use it. 2–5 tight sentences.>

:example
<runnable BEARER code that print()s something illustrative>

:see
><area>
<closest sibling API slugs>
```
Optional short trailing note in `:content` for membrane/PHP-JS equivalence — one line,
not a section. No `:title` unless it differs from the derived label.

### Setup (host, one-time, before content work)
```sh
ssh root@10.4.2.110 'cd /Code/bearer.openfu.com/bearer && mkdir -p site/doc/examples/_gen && chown -R www-data:www-data site/doc/examples/_gen && chmod 775 site/doc/examples/_gen'
```

---

## Phase 2 — Content sweep (batched by area; one area = one reviewable batch)

For each area file in `site/doc/areas/` (string, sys, types, sqlite, mysql, memcache,
session, websocket, task, time, uri, regex, ob, markup, socket, noise, runtime), bring
every listed page to the canonical template:

1. **Convert the 31 stubs** to full structured pages.
2. **Add a live `:example`** to every page missing one (≈134), each verified to render
   real output.
3. **Repair `:see`** — area link + sibling links on every page; demote membrane / PHP-JS
   filler to a one-line note.
4. **Cull dead pages** — remove `concat.txt` ("Removed. Not a current API") and audit
   internal-only helpers (e.g. `json_consume_space`) for removal; also remove culled
   slugs from the relevant `areas/*.txt`.

Per-batch bar: every touched page renders cleanly, its example produces real output,
and the suite stays green.

---

## Phase 3 — Gate

- Add a `site/tests/cli_runner.uce` regression test that renders **every** `?p=` page
  and asserts HTTP 200, no error/trap marker, and (for pages with `:example`) presence
  of a non-error Output block.
- Full host gate (this is the ONLY place builds/tests run — the sshfs mount is
  edit-only, no WASI SDK on the client):
  ```sh
  ssh root@10.4.2.110 'cd /Code/bearer.openfu.com/bearer && bash scripts/build_core_wasm.sh && bash scripts/build_linux.sh && systemctl restart bearer.service && sleep 3 && bash scripts/run_cli_tests.sh --include-wasm-kill'
  ```
  Expect the current 91 to grow by the new doc gate(s), 0 failed.

## Hard rules (non-negotiable)

- **NEVER `git commit`/push/tag.** Not pi, not its subagents, not anyone. Leave a clean
  working tree and report.
- **Always run the real host gate** (`bash scripts/build_core_wasm.sh && bash
  scripts/build_linux.sh && systemctl restart bearer.service && ...`) and trust only its
  output. `g++ -fsyntax-only` / client-side builds do NOT count — there is no WASI SDK
  on the client and the doc unit must actually render.
- Sub-delegation model is exactly `gpt-5.3-codex-spark`.
- Verify the example mechanism end-to-end on the host BEFORE authoring content against
  it — content written against an unproven mechanism is wasted.
