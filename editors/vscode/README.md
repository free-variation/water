# logicforth — VS Code support

Syntax highlighting, indentation, and bracket matching for logicforth (`.l4`).

- **Highlighting** — comments (`\ …`, `( … )`), strings (`""` escape), numbers,
  `:symbol` / `/path` literals, capitalized logic variables, control flow,
  defining words, the logic words, and the built-in vocabulary.
- **Indentation** — increases inside `:` … `;`, `[:` … `:]`, and
  `if`/`begin`/`else`; dedents lines that start with `;`/`:]`/`then`/`else`/
  `until`/`again`/`repeat`. One-line definitions and quotations don't indent.
- **Bracket matching / auto-close** — `[ ]`, `{ }`, `( )`, and `"`.

## Loading it locally

Symlink (or copy) this folder into your VS Code extensions directory, then
reload the window:

```sh
ln -s "$PWD/editors/vscode" ~/.vscode/extensions/logicforth-0.1.0
```

In VS Code: **Developer: Reload Window** (Cmd/Ctrl-Shift-P). Open any `.l4`
file — the status bar should show **logicforth**.

(For VS Code Insiders use `~/.vscode-insiders/extensions/`; for a portable
install, its `data/extensions/`.)

## Packaging (optional)

```sh
npm install -g @vscode/vsce
cd editors/vscode && vsce package          # produces logicforth-0.1.0.vsix
code --install-extension logicforth-0.1.0.vsix
```
