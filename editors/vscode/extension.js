const vscode = require("vscode");
const {LanguageClient} = require("vscode-languageclient/node");
const {clientConfig} = require("./client-config");
const {resolveServer} = require("./server-path");

let client;

async function activate() {
  const folder = vscode.workspace.workspaceFolders?.[0];
  const configuredPath = vscode.workspace.getConfiguration("capy").get("server.path", "");
  const server = resolveServer({
    configuredPath,
    workspaceFolder: folder?.uri.fsPath,
    pathValue: process.env.PATH
  });
  if (!server.command) {
    const message = server.configured
      ? `Capy cannot execute ${configuredPath}. Set capy.server.path to an executable capyc.`
      : `Capy language server was not found. Tried ${server.attempts.join(", ")}. Build capyc, install Bearer, or set capy.server.path.`;
    vscode.window.showErrorMessage(message);
    return;
  }
  const options = clientConfig(server.command, folder?.uri.fsPath);
  client = new LanguageClient(options.id, options.name, options.serverOptions, options.clientOptions);
  try {
    await client.start();
  } catch (error) {
    client = undefined;
    vscode.window.showErrorMessage(`Capy could not start ${server.command}: ${error.message}`);
  }
}

function deactivate() {
  return client?.stop();
}

module.exports = {activate, deactivate};
