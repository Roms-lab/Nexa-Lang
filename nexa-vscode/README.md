# Nexa Syntax Highlighting

VS Code / Cursor extension for Nexa language syntax highlighting.

## Installation

### Option 1: Copy to extensions folder

```bash
cp -r nexa-vscode ~/.vscode/extensions/nexa-0.1.0
```

Restart VS Code/Cursor. `.nxa` files will get syntax highlighting.

### Option 2: Run from folder (development)

1. Open the `nexa-vscode` folder in VS Code
2. Press F5 to launch Extension Development Host
3. Open a `.nxa` file in the new window

### Package as VSIX (optional)

```bash
npm install -g @vscode/vsce
cd nexa-vscode
vsce package
```

Then install the generated `.vsix` file via **Extensions: Install from VSIX...**.

## Features

- Syntax highlighting for `.nxa` files
- Keywords: fn, main, let, if, else, while, return, break, continue, true, false
- Comments: // and /* */
- Strings, numbers, operators
- #include preprocessor
- Bracket matching and auto-closing
