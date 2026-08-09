# DOCS-TODO — `/doc` documentation, round 2

Briefing for the second documentation pass. Read this fully before touching anything.

The bar is unchanged and non-negotiable: **the PHP manual**, matched on scope, tone,
usability, and attention to detail. Round 1 built the page *skeleton* to that standard.
Round 2 fixes what the skeleton exposed: missing pages, thin examples, uncalled-out
failure modes, and a cross-reference layer that is currently generated rather than
curated.

The docs live under `site/doc/`:
- `index.uce` — the renderer (index + per-page detail view).
- `lib/doc_page.h` — page parsing (`load_doc_page`) + label/kind helpers.
- `style.css` — theme + layout.
- `pages/*.txt` — 274 page source files (the reference corpus).
- `capy/*.txt` — 12 guide chapters ("Learn Capy").
- `areas/*.txt` — area groupings; first line is the area title, following lines are
  page slugs (or `>area` references) that populate the index sidebar.
- `search.uce`, `singlepage.uce` — search + single-page render.

---

## Round 1 — DONE, do not redo

Verified against the live service on 2026-08-07:

- **Title bug fixed.** `load_doc_page` drops a `:title` that equals the page slug
  (`lib/doc_page.h:233`), so `0_StringList` renders as "StringList". The redundant
  `:title` blocks still sitting in ~15 page files are inert; delete them as cleanup
  only, not as a fix.
- **Live examples work.** 269 of 287 rendered pages carry a `:example` that is compiled
  and executed at render time with its real captured output shown beneath it.
- **Structure landed.** 270 pages have signatures, 229 have Return Values, 211 have
  Parameters, 242 have a paired Capy + C++ example.
- **Sidebar wrapping fixed** (`overflow-wrap` present in `style.css`).
- **Format spec page exists** (`pages/3_Documentation format.txt`).
- **Gate exists** (`site/tests/cli_runner.uce`, plus `site/tests/api_coverage.uce`).
- **`concat.txt` culled.** `json_consume_space.txt` was not — see task 8.

---

## Principles (unchanged from round 1)

1. **Usage first.** The opening `:content` sentence says what the function does and
   when you'd reach for it, in plain terms. No membrane/ABI talk up top.
2. **A real example, always** — executed at render time, never hand-written output.
3. **Cross-link deliberately.** A reader should never have to search alone.
4. **Demote the drivel.** Cross-membrane behaviour, lock semantics, and PHP/JS
   equivalents are at most a short trailing note — never the body.

## Canonical page template (unchanged)

```
:sig
<one or more real signatures, exactly as declared>

:params
<name> : <what it is>
return value : <what comes back>

:content
<Usage-first prose. What it does, when to use it. 2–5 tight sentences.>

:example
<runnable code that print()s something illustrative>

:see
><area>
<closest sibling API slugs>
```

---

## Task 1 — Write the missing reference pages  ★ highest priority

The guide currently teaches functions the reference does not document.
`capy/09-web-handlers-and-requests.txt` uses `response_header`, `response_status`,
`response_cookie`, `session_set`, `string()`, `s32()` and `bool()` in its examples, and
**not one of them has a page**. Its `:see` block cannot link them because there is
nothing to link to. A reader cannot write a login form from these docs: nothing
documents setting a cookie, setting a status code, or writing a session value.

**Public stdlib functions with no page** (verified against `src/capy/stdlib.capy`):

| slug | why it matters |
|---|---|
| `response_status` | HTTP status code — `stdlib.capy:470` |
| `response_header` | response headers — `stdlib.capy:575` |
| `response_cookie` | cookies — `stdlib.capy:475` |
| `session_set` | session write — `stdlib.capy:473` |
| `session_remove` | session delete — `stdlib.capy:474` |
| `component_capture` | render a component to a string — `stdlib.capy:581` |
| `call` | module call — `stdlib.capy:589` |
| `dval_put` | dval write |
| `bool` `s32` `s64` `u64` `f64` `string` | the scalar constructors the guide uses everywhere |

**Compiler intrinsics with no page** (verified against `src/capy/compiler.cpp`):

| slug | why it matters |
|---|---|
| `length` | referenced on 22 corpus pages, documented on none — `compiler.cpp:5026` |
| `trap` | how you abort; referenced on 9 pages |
| `trusted_markup` | the XSS escape hatch. Mentioned **once** in the whole corpus (`capy/06-strings-and-markup.txt:92`). Needs its own page with an explicit warning. |
| `clone` | value copy semantics |
| `dval` | dval construction — `compiler.cpp:5051` |

Skip `mysql_escape_default` and `string_list_find` unless they are genuinely public;
if they are internal, mark them so in the coverage manifest (task 9) instead.

Each new page gets the full canonical template with a live, executed example.

**Gate:** every new slug renders 200 with a non-error Output block; the guide chapter
that uses each function gains it in `:see`.

## Task 2 — Second and third examples where behaviour is undemonstrated

268 of 287 pages have exactly **one** example section. The PHP manual routinely runs
3–5 numbered examples per function with descriptive captions.

`substr` is the model case: its Description correctly documents negative start and
negative length, and its one example is `substr("documentation", 0, 3)`. The two
behaviours most likely to bite a reader are described but never shown.

Rule: **if the Description names an edge behaviour, an example must demonstrate it.**
Sweep the corpus for Description text describing negative indices, out-of-range input,
empty input, failure returns, or optional-argument forms, and add an example for each.

Also rename example headings. `Example: RENDER` / `Example: CLI` names the invocation
mode, not what the example teaches. Use PHP-style captions —
`Example #2 — substr() with a negative start` — keeping the mode as a secondary label.

## Task 3 — Callouts for failure behaviour

Only 6 pages have an Errors section (`http_request`, `mysql_connect`, `regex_replace`,
`shell_spawn`, `sqlite_query`, `task`). There are **no** Notes, Warnings, or Cautions
anywhere in the corpus.

- Add `:note` / `:warning` block support to `load_doc_page` + `style.css` (PHP-manual
  styling: coloured left border, labelled).
- Then sweep for silent-failure functions and flag them. Known starting set:
  - `json_decode` — *"Malformed JSON returns empty or partial data without an error."*
    That is silent data corruption, currently rendered at the same weight as the
    sentence about booleans, with no example of the malformed case. Warning + example.
  - `file_get_contents` — returns `""` both for an unreadable file and an empty file.
    The two are indistinguishable. Note + `file_exists` guidance.
  - `password_hash` — no Errors section, no note on output length for column sizing
    (the PHP manual states this explicitly), no prose link to `password_needs_rehash`.
  - `trusted_markup` — warning, once it has a page (task 1).

## Task 4 — Replace the generated "Related" list with a curated See Also

The sidebar currently dumps the **entire area file**. Distribution runs up to **79
links**; 28 pages carry 54. The PHP manual's See Also is 3–8 hand-picked entries.

Consequences to fix:
- **213 pages link to themselves.** Exclude the current page unconditionally — that is
  a renderer fix in `index.uce`, do it first.
- Curate `:see` per page: its area link plus its closest siblings, 3–8 entries.
- Fix 4 dangling cross-references: `2_Request_set_status → set_cookie`,
  `usleep → sleep`, `zip_create → DValue`, `zip_list → DValue`.

## Task 5 — Make search usable

Server-side substring match only, so any multi-word query returns **zero** results:
`"string length"`, `"sort array"`, `"how to set a cookie"` all return nothing.
`"strlen"` returns nothing. `?q=cookie` returns four incidental mentions and no way to
actually set one.

- Tokenize the query; rank by number of terms matched, title hits above body hits.
- Add a PHP-name alias table. The function names are deliberately PHP-shaped
  (`substr`, `str_starts_with`, `json_decode`, `password_hash`), so PHP-familiar
  readers will search PHP names — `strlen` should find `length`.
- Render markdown in snippets; they currently show raw backticks.
- Consider a "Coming from PHP" info page alongside the existing
  `pages/3_Coming from React.txt`.

## Task 6 — Deepen the guide

All twelve chapters total **6,476 words**, ~540 each. Chapter 9 — web request handling,
the chapter a web developer reads first — is the **shortest** at 238 words, with no
POST example, no form handling, and no upload example despite `files` being a
documented member of the request snapshot.

- Expand chapter 9 first: a real form round-trip (GET form → POST handler → validation
  → cookie/session → redirect), covering the task-1 functions.
- Then bring the other chapters up to a comparable depth.
- **Execute every guide code block, not just the first.** Chapters currently verify
  only their opening example: `capy-04` has 9 `<pre>` blocks and 1 Output block;
  `capy-03`, `-07` and `-08` have 10 and 1. Roughly 12% of guide code is verified.
  Convert the plain ` ```capy ` fences to `:example capy render` blocks.

## Task 7 — Renderer and presentation fixes

- **404 handling.** An unknown page returns HTTP 200 with a blank body and the raw slug
  in the breadcrumb — `?p=totally_bogus_page`. Render a real "page not found" with a
  search prompt and a link to the index. (Reflection *is* correctly HTML-escaped —
  script payloads were tested, there is no XSS. This is usability only.)
- **Sidebar ordering is arbitrary.** Areas sort by *filename*, so the reader sees
  Markup, Memcache, MySQL, Noise, … then String, then "File System Functions"
  (`sys.txt`), then Background tasks. Memcache outranks Strings and Types. Add an
  explicit order key to `areas/*.txt` and lead with the areas a newcomer needs.
- **No dark mode.** `style.css` has two `@media` rules, both `max-width`, no
  `prefers-color-scheme`.

## Task 8 — Prose and consistency cleanup

- **89 of 270 Descriptions open with the literal "In Capy,"**. The PHP manual opens
  with the verb: *"Returns part of a string."* Rewrite the openers.
- Eleven pages repeat *"The C++ .uce call has the same arguments."* verbatim; four
  repeat *"The C++ String function has the same input and result."* Cut or vary.
- **`json_decode`'s paired examples are not equivalent** — the C++ side computes
  `data["tags"].keys().size()`, the Capy side hardcodes the literal `2` and never
  demonstrates reading the array. Audit all paired examples for this.
- **6 examples are `Compile-only`** and therefore unverified: `backtrace_get_frames`,
  `http_request`, `mysql_query`, `redirect`, `unit_load`, `units_list`. Make them
  executable where possible; where a live service is genuinely required, say so
  explicitly in the page rather than in a generic footer.
- Finish the round-1 audit: `json_consume_space` is an internal helper still carrying a
  public page. Remove it or mark it internal.
- Delete the ~15 inert `:title` blocks that duplicate their slug.

## Task 9 — Make the coverage gap mechanical

`scripts/api_coverage_manifest.py` already gates "public API has a doc page" — but it
is a **hand-maintained list**, which is precisely why every function in task 1 slipped
through. A hand-maintained list cannot catch what nobody remembered to add.

Derive the required-doc set instead:
- parse public `function` declarations from `src/capy/stdlib.capy` (excluding `__`-prefixed internals),
- plus the intrinsic names dispatched in `src/capy/compiler.cpp`,
- diff against `site/doc/pages/`,
- fail with the missing list.

Keep the explicit internal/integration escape hatch, but make *omission* the thing that
fails rather than the thing that passes silently.

**This task is what stops round 3 from having the same task 1.** Land it in the same
pass, not after.

---

## Gate

The full host gate is the only trusted signal:

```sh
cd /Code/capy-bearer \
  && bash scripts/build_core_wasm.sh \
  && bash scripts/build_linux.sh \
  && systemctl restart bearer.service \
  && sleep 3 \
  && bash scripts/run_cli_tests.sh --include-wasm-kill
```

Expect 0 failed, and the suite count to grow with the new doc gates.

## Hard rules (non-negotiable)

- **NEVER `git commit` / push / tag.** Not pi, not its subagents, not anyone. Leave a
  clean working tree and report.
- **Always run the real host gate** and trust only its output. `g++ -fsyntax-only` and
  client-side builds do NOT count — there is no WASI SDK on the client and the doc unit
  must actually render.
- **Work incrementally; build and test after each task.** If a step cannot stay green,
  STOP and report rather than piling on.
- **Keep the diff proportional.** Reuse existing helpers and patterns instead of writing
  parallel ones. No speculative abstractions, no forwarding wrappers, no narration
  comments. Delete replaced code. A small fix must be a small diff.
- Report the diff stat and the suite Summary line at the end of each task.
