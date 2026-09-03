# Capy editor support

This extension adds TextMate highlighting and language server support for `.capy` files.

TextMate highlighting starts as soon as a file opens. The language server then adds diagnostics, symbols, hover text, completion, signatures, definitions, and semantic tokens.

## Visual Studio Code

Open `editors/vscode/` in Visual Studio Code. Press `F5` to start an Extension Development Host.

The extension starts `capyc --lsp` through stdio. It checks these locations in order:

1. `${workspaceFolder}/bin/capyc`
2. `capyc` on `PATH`
3. `/usr/lib/bearer/bin/capyc`

Set `capy.server.path` to use another executable. Set `capy.trace.server` to `messages` or `verbose` to inspect protocol traffic.

Run the extension tests from this directory:

```bash
npm install
npm test
```

## Neovim

Neovim 0.11 and later can start the server without a plugin:

```lua
vim.lsp.config.capy = {
  cmd = { "capyc", "--lsp" },
  filetypes = { "capy" },
  root_markers = { ".git" },
}
vim.lsp.enable("capy")
```

Associate `.capy` files with the `capy` file type in your file type configuration.

## Helix

Add this configuration to `languages.toml`:

```toml
[language-server.capy]
command = "capyc"
args = ["--lsp"]

[[language]]
name = "capy"
scope = "source.capy"
file-types = ["capy"]
language-servers = ["capy"]
```

## Zed

Add a custom language server to Zed settings after you register the Capy language:

```json
{
  "lsp": {
    "capy": {
      "binary": {
        "path": "capyc",
        "arguments": ["--lsp"]
      }
    }
  },
  "languages": {
    "Capy": {
      "language_servers": ["capy"]
    }
  }
}
```

## Emacs eglot

Register the server for your Capy major mode:

```elisp
(add-to-list 'eglot-server-programs
             '(capy-mode . ("capyc" "--lsp")))
(add-to-list 'auto-mode-alist '("\\.capy\\'" . capy-mode))
```

Open a Capy file and run `M-x eglot`.
