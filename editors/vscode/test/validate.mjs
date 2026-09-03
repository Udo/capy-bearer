import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const root = join(dirname(fileURLToPath(import.meta.url)), "..");
const loadJson = async (path) => JSON.parse(await readFile(path, "utf8"));
const manifest = await loadJson(join(root, "package.json"));
const language = await loadJson(join(root, "language-configuration.json"));
const grammar = await loadJson(join(root, "syntaxes", "capy.tmLanguage.json"));

assert.equal(manifest.publisher, "udo");
assert.equal(manifest.main, "./extension.js");
assert.equal(manifest.contributes.languages[0].id, "capy");
assert.ok(manifest.contributes.languages[0].extensions.includes(".capy"));
assert.equal(manifest.contributes.grammars[0].scopeName, "source.capy");
assert.equal(manifest.contributes.grammars[0].embeddedLanguages["meta.embedded.block.html.capy"], "html");
assert.equal(manifest.contributes.grammars[0].embeddedLanguages["meta.embedded.block.javascript.capy"], "javascript");
assert.equal(manifest.contributes.grammars[0].embeddedLanguages["meta.embedded.block.css.capy"], "css");
assert.equal(manifest.dependencies["vscode-languageclient"], "8.1.0");
assert.ok(!Object.hasOwn(manifest, "extensionKind"));
const settings = manifest.contributes.configuration.properties;
assert.equal(settings["capy.server.path"].default, "");
assert.deepEqual(settings["capy.trace.server"].enum, ["off", "messages", "verbose"]);
assert.equal(grammar.scopeName, "source.capy");
assert.equal(language.comments.lineComment, "//");
assert.ok(!Object.hasOwn(language.comments, "blockComment"));

const regexFields = new Set(["match", "begin", "end", "while"]);
const internalIncludes = [];
function validatePatterns(value, path = "grammar") {
  if (Array.isArray(value)) {
    value.forEach((item, index) => validatePatterns(item, `${path}[${index}]`));
    return;
  }
  if (!value || typeof value !== "object") return;
  for (const [key, child] of Object.entries(value)) {
    if (regexFields.has(key)) assert.doesNotThrow(() => new RegExp(child), `${path}.${key} is not a valid regular expression`);
    if (key === "include" && child.startsWith("#")) internalIncludes.push(child.slice(1));
    validatePatterns(child, `${path}.${key}`);
  }
}
validatePatterns(grammar.patterns);
validatePatterns(grammar.repository);
for (const include of internalIncludes) assert.ok(grammar.repository[include], `missing grammar repository entry: ${include}`);
for (const value of Object.values(language.indentationRules)) assert.doesNotThrow(() => new RegExp(value));
assert.doesNotThrow(() => new RegExp(language.wordPattern));
assert.ok(new RegExp(language.indentationRules.increaseIndentPattern).test("function CLI(request : dval) {"));
assert.ok(!new RegExp(language.indentationRules.increaseIndentPattern).test("var value := call()"));

const generated = grammar.capyGeneratedWords;
assert.deepEqual(generated.directives.sort(), ["callsite", "compile", "exports", "import"]);
assert.ok(generated.types.includes("dval") && generated.types.includes("module"));
assert.ok(generated.handlers.includes("SERVE_HTTP") && generated.handlers.includes("TASK"));
for (const invented of ["new", "enum", "match", "when", "block", "from", "is", "and", "or", "not", "u128", "s128", "f16", "isize", "usize", "Any"]) {
  assert.ok(!Object.values(generated).flat().includes(invented), `obsolete grammar word remains: ${invented}`);
}
const serialized = JSON.stringify(grammar);
for (const removedScope of ["comment.block.capy", "comment.line.number-sign.capy", "string.quoted.single.capy"]) {
  assert.ok(!serialized.includes(removedScope), `obsolete scope remains: ${removedScope}`);
}

console.log("Capy VS Code extension files are valid.");
