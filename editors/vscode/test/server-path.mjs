import assert from "node:assert/strict";
import {chmod, mkdir, rm, writeFile} from "node:fs/promises";
import {createRequire} from "node:module";
import {tmpdir} from "node:os";
import {delimiter, join} from "node:path";
import {mkdtemp} from "node:fs/promises";

const require = createRequire(import.meta.url);
const {clientConfig} = require("../client-config.js");
const {resolveServer} = require("../server-path.js");
const config = clientConfig("/workspace/bin/capyc", "/workspace");
assert.equal(config.id, "capy");
assert.deepEqual(config.serverOptions, {command: "/workspace/bin/capyc", args: ["--lsp"], options: {cwd: "/workspace"}});
assert.deepEqual(config.clientOptions.documentSelector, [{language: "capy", scheme: "file"}]);

const root = await mkdtemp(join(tmpdir(), "capy-server-path-"));
const workspace = join(root, "workspace");
const pathBin = join(root, "path-bin");
const installed = join(root, "installed", "capyc");
await mkdir(join(workspace, "bin"), {recursive: true});
await mkdir(pathBin, {recursive: true});
await mkdir(join(root, "installed"), {recursive: true});

async function executable(path) {
  await writeFile(path, "#!/bin/sh\n");
  await chmod(path, 0o755);
}

const workspaceCapyc = join(workspace, "bin", "capyc");
const pathCapyc = join(pathBin, "capyc");
await executable(pathCapyc);
await executable(installed);
assert.equal(resolveServer({workspaceFolder: workspace, pathValue: pathBin, installedPath: installed}).command, pathCapyc);
await executable(workspaceCapyc);
assert.equal(resolveServer({workspaceFolder: workspace, pathValue: pathBin, installedPath: installed}).command, workspaceCapyc);
assert.equal(resolveServer({configuredPath: installed, workspaceFolder: workspace, pathValue: pathBin}).command, installed);
assert.equal(resolveServer({configuredPath: "capyc", pathValue: pathBin}).command, pathCapyc);

const missingConfigured = resolveServer({configuredPath: join(root, "missing"), pathValue: ""});
assert.equal(missingConfigured.command, "");
assert.equal(missingConfigured.configured, true);
assert.deepEqual(missingConfigured.attempts, [join(root, "missing")]);
const missing = resolveServer({workspaceFolder: join(root, "empty"), pathValue: [join(root, "a"), join(root, "b")].join(delimiter), installedPath: join(root, "not-installed")});
assert.equal(missing.command, "");
assert.deepEqual(missing.attempts, [join(root, "empty", "bin", "capyc"), "capyc on PATH", join(root, "not-installed")]);
await rm(root, {recursive: true, force: true});

console.log("Capy language-server path resolution is valid.");
