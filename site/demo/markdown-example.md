# Markdown Demo

This page exercises **strong**, *emphasis*, ~~strikethrough~~, `code spans`, and a bare URL: https://example.com/doc/index.uce

## Task List

- [x] Parse markdown into an AST
- [x] Render markdown into HTML
- [ ] Add even more extensions later

## Table

| Feature | Status | Notes |
| :--- | :---: | ---: |
| Headings | Ready | 1 |
| Tables | Ready | 2 |
| Components | Ready | 3 |

## Quote

> Markdown in BEARER should be composable.
>
> Components make that much more interesting.

:::warning title="Component-backed directive"
This `:::warning` block is rendered through a normal BEARER component selected from `options["components"]`.
:::

## Code

```cpp
RENDER(Request& context)
{
	print(markdown_to_html("# hello"));
}
```
