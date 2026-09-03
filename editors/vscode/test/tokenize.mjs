import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import { createRequire } from "node:module";
import vscodeTextmate from "vscode-textmate";
import vscodeOniguruma from "vscode-oniguruma";

const { Registry, parseRawGrammar, INITIAL } = vscodeTextmate;
const { loadWASM, OnigScanner, OnigString } = vscodeOniguruma;
const root = join(dirname(fileURLToPath(import.meta.url)), "..");
const require = createRequire(import.meta.url);
const wasmPath = require.resolve("vscode-oniguruma/release/onig.wasm");
const wasm = await readFile(wasmPath);
await loadWASM(wasm.buffer.slice(wasm.byteOffset, wasm.byteOffset + wasm.byteLength));

const capyPath = join(root, "syntaxes", "capy.tmLanguage.json");
const embeddedGrammar = (scopeName) => ({scopeName, patterns: [{name: scopeName, match: "[^<]+|<"}]});
const registry = new Registry({
  onigLib: Promise.resolve({
    createOnigScanner: (sources) => new OnigScanner(sources),
    createOnigString: (text) => new OnigString(text)
  }),
  loadGrammar: async (scopeName) => {
    if (scopeName === "source.capy") return parseRawGrammar(await readFile(capyPath, "utf8"), capyPath);
    if (["text.html.basic", "source.js", "source.css"].includes(scopeName)) return embeddedGrammar(scopeName);
    return null;
  }
});
const grammar = await registry.loadGrammar("source.capy");
assert.ok(grammar, "TextMate did not load the Capy grammar");

function tokenize(line, ruleStack = INITIAL) {
  return grammar.tokenizeLine(line, ruleStack);
}
function scopesFor(result, line, text) {
  const start = line.indexOf(text);
  assert.notEqual(start, -1, `missing test text: ${text}`);
  const token = result.tokens.find((candidate) => candidate.startIndex <= start && candidate.endIndex >= start + text.length);
  assert.ok(token, `no token contains: ${text}`);
  return token.scopes;
}
function assertScope(line, text, scope) {
  const result = tokenize(line);
  assert.ok(scopesFor(result, line, text).includes(scope), `${text} does not have scope ${scope}`);
  return result;
}

const directive = assertScope('#import "types" as types', "import", "keyword.control.directive.capy");
assert.ok(!scopesFor(directive, '#import "types" as types', "import").some((scope) => scope.startsWith("comment.")));
assertScope("#exports RENDER", "exports", "keyword.control.directive.capy");
assertScope("function CLI(request : dval) {", "CLI", "entity.name.function.handler.capy");
assertScope("function COMPONENT:badge(request : dval) {", "COMPONENT", "entity.name.function.handler.capy");
assertScope("function COMPONENT:badge(request : dval) {", "badge", "entity.name.function.capy");
assertScope("trace host function backtrace(value : dval) module", "trace", "keyword.declaration.function.capy");
assertScope("dval", "dval", "support.type.builtin.capy");
assertScope("value : dval", "dval", "support.type.builtin.capy");
assertScope("trace host function backtrace(value : dval) module", "dval", "support.type.builtin.capy");
assertScope("trace host function backtrace(value : dval) module", "module", "support.type.builtin.capy");
assertScope("function value() s32 { -> item::size }", "->", "keyword.operator.yield.capy");
assertScope("function value() s32 { -> item::size }", "::", "keyword.operator.scope.capy");
assertScope('print("capy") // note', "capy", "string.quoted.double.capy");
assertScope('print("capy") // note', "note", "comment.line.double-slash.capy");
assertScope("var value : u32 = 42", "42", "constant.numeric.integer.capy");
assertScope("print(value)", "print", "entity.name.function.call.capy");

for (const line of [
  '<><p title="<?= caption ?>">Hello <?= value ?></p></>',
  '<><script>const value = <?= count ?>;</script></>',
  '<><style>.item { width: <?= size ?>px; }</style></>'
]) {
  const result = tokenize(line);
  const field = line.includes("caption") ? "caption" : line.includes("count") ? "count" : "size";
  assert.ok(scopesFor(result, line, field).includes("meta.embedded.expression.capy"));
  assert.ok(scopesFor(result, line, line.includes("script") ? "script" : line.includes("style") ? "style" : "Hello").includes("meta.embedded.block.html.capy"));
}
const rawLine = "<>before <?: trusted ?> after</>";
assert.ok(scopesFor(tokenize(rawLine), rawLine, "trusted").includes("meta.embedded.expression.capy"));

let stack = INITIAL;
for (const line of ["<>outer", "<><b><?= value ?></b></>", "still outer</>"]) {
  const result = tokenize(line, stack);
  stack = result.ruleStack;
  if (line.startsWith("still")) assert.ok(scopesFor(result, line, "still").includes("meta.embedded.block.html.capy"));
}

console.log("Capy TextMate tokenization is valid.");
