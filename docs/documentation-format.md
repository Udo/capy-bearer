# Documentation page format

Contributor reference for the source files under `site/doc/`. This is not part of
the published API reference — it describes how those pages are written and
validated.

## Where the sources live

- `site/doc/pages/*.txt` contains function pages.
- `site/doc/pages/type/*.txt` contains type pages.
- `site/doc/pages/handler/*.txt` contains handler pages.
- `site/doc/capy/*.txt` — the numbered Learn Capy guides.
- `site/doc/areas/*.txt` — per-area name listings used for grouping.

`scripts/generate_capy_docs.py` reads these and writes the `site/doc/content-*.capy`
units the site actually serves, plus `site/doc/lib/capy_signatures.generated.h`.
Run it after editing any page; the generated units are what the runtime loads.

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

API page examples are handler **bodies**, not whole handlers — no
`function RENDER(...)` wrapper, and they cannot redeclare `request`. Guide
examples are the opposite: they are complete units.

Examples may not reference host-language types or private `__bearer` APIs.

Every guide in `site/doc/capy/` needs one runnable render example with an exact
`:output` block; `scripts/test_capy_guide_examples.sh` runs it over HTTP and
compares byte for byte. The guide set is pinned in `CANONICAL_GUIDES` in
`scripts/check_capy_doc_examples.py`, so adding a guide means adding it there too.

A page whose name does not match a real function or handler will show up in the
capability manifest as unsupported. Every page should document something that
exists.

## Adding a page

1. Write `site/doc/pages/<function>.txt`, named after the function.
2. Add a route in `site/doc/index.capy` and a link in
   `site/doc/components/index.capy` and `all.capy`.
3. Add the name to the relevant `site/doc/areas/*.txt`.
4. Add an entry to `src/capy/parity_manifest.h` with runtime evidence, keeping
   the array size correct.
5. Run `python3 scripts/generate_capy_docs.py`, then the doc gates.
