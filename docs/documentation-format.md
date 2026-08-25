# Documentation page format

Contributor reference for the source files under `site/doc/`. This is not part of
the published API reference — it describes how those pages are written and
validated.

## Where the sources live

- `site/doc/source/api/*.txt` contains function pages.
- `site/doc/source/type/*.txt` contains type pages.
- `site/doc/source/handler/*.txt` contains handler pages.
- `site/doc/source/guide/*.txt` contains the numbered Learn Capy guides.
- `site/doc/source/how-to/*.txt` contains task-focused how-to articles.
- `site/doc/areas/*.txt` — per-area name listings used for grouping.

`scripts/generate_capy_docs.py` reads these and writes route modules under
`site/doc/content/`. Each route module prints its complete static detail HTML.
It also writes the index, all-pages, and search catalogs. Run it after editing
any page. The runtime loads the generated route modules.

`scripts/generate_capy_doc_signatures.py` writes
`site/doc/lib/capy_signatures.generated.h`.

## Directives

A directive starts a line with `:` and its body runs to the next directive.

| Directive | Purpose |
| --- | --- |
| `:title` | Display title, when it differs from the page name |
| `:sig` | The Capy signature, one per line for overloads |
| `:params` | One `name : description` per line |
| `:returns` | What the call evaluates to |
| `:errors` | Failure behaviour |
| `:content` | Prose, rendered as Markdown |
| `:example capy ENTRY [caption]` | A runnable example |
| `:output` | Exact expected output, following a render example |
| `:see` | Related page names, one per line |

`ENTRY` is one of `render`, `cli`, `component`, `init`, `once`, or `ws`.

`:content` may appear more than once; the parts are concatenated in order, which
lets prose sit between examples.

## Rules the gates enforce

`:sig` must match the signature `capyc` generates from `src/capy/stdlib.capy`.
Editing a signature by hand without changing the stdlib fails
`scripts/generate_capy_doc_signatures.py --check`.

API and how-to examples are handler **bodies**, not whole handlers — no
`function RENDER(...)` wrapper, and they cannot redeclare `request`. Guide
examples are the opposite: they are complete units.

Examples may not reference host-language types or private `__bearer` APIs.

Every guide in `site/doc/source/guide/` needs one runnable render example with an exact
`:output` block; `scripts/test_capy_guide_examples.sh` runs it over HTTP and
compares byte for byte. The guide set is pinned in `CANONICAL_GUIDES` in
`scripts/check_capy_doc_examples.py`, so adding a guide means adding it there too.

API pages must document a public Capy function. Type and handler pages must
document the matching language form. Use a how-to page for task guidance that
does not document a callable API.

## Adding a page

1. Write `site/doc/source/api/<function>.txt`, named after the public function.
2. Write task guidance in `site/doc/source/how-to/<slug>.txt`.
3. Add API capability evidence to `src/capy/parity_manifest.h`, and keep the array size correct.
4. Run `python3 scripts/generate_capy_docs.py`, then the doc gates.
