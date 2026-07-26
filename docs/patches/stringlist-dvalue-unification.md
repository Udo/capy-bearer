# StringList → DValue unification (2026-07-24)

`StringList` storage was removed. Native tokenization and compiler registries retain
`std::vector<String>` internally, but all `.uce` list boundaries now use list-shaped
`DValue`; `using StringList = DValue` is source compatibility only. DValue owns the
former string-list methods, range iteration, and `push_back(String)` compatibility.

Capy list operations use copied DValues and `__bearer_dv_apply_brrb`; no
`bearer_string_list` import is emitted. Legacy StringList documentation pages redirect
to DValue and the parity manifest marks the compatibility surface supported.
