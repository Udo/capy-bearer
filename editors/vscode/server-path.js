const fs = require("node:fs");
const path = require("node:path");

function executable(candidate) {
  try {
    return fs.statSync(candidate).isFile() && (fs.accessSync(candidate, fs.constants.X_OK), true);
  } catch {
    return false;
  }
}

function pathExecutable(name, pathValue) {
  for (const directory of (pathValue || "").split(path.delimiter)) {
    if (!directory) continue;
    const candidate = path.join(directory, name);
    if (executable(candidate)) return candidate;
  }
  return "";
}

function resolveServer({configuredPath = "", workspaceFolder = "", pathValue = process.env.PATH, installedPath = "/usr/lib/bearer/bin/capyc"} = {}) {
  if (configuredPath) {
    const command = configuredPath.includes(path.sep) ? (executable(configuredPath) ? configuredPath : "") : pathExecutable(configuredPath, pathValue);
    return command
      ? {command, attempts: [configuredPath]}
      : {command: "", attempts: [configuredPath], configured: true};
  }
  const workspacePath = workspaceFolder ? path.join(workspaceFolder, "bin", "capyc") : "${workspaceFolder}/bin/capyc";
  const attempts = [workspacePath, "capyc on PATH", installedPath];
  if (workspaceFolder && executable(workspacePath)) return {command: workspacePath, attempts};
  const fromPath = pathExecutable("capyc", pathValue);
  if (fromPath) return {command: fromPath, attempts};
  if (executable(installedPath)) return {command: installedPath, attempts};
  return {command: "", attempts};
}

module.exports = {resolveServer};
