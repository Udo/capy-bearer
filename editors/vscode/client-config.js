function clientConfig(command, workspaceFolder = "") {
  return {
    id: "capy",
    name: "Capy language server",
    serverOptions: {
      command,
      args: ["--lsp"],
      options: workspaceFolder ? {cwd: workspaceFolder} : undefined
    },
    clientOptions: {
      documentSelector: [{language: "capy", scheme: "file"}]
    }
  };
}

module.exports = {clientConfig};
