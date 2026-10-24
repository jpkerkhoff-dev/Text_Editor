# Terminal Editor (C)

A small, single-file terminal text editor written in C using POSIX terminal APIs and ANSI escape sequences.

## Controls

| Key | Action |
| --- | --- |
| Arrow keys | Move the cursor |
| Home / End | Move to the start / end of the line |
| Page Up / Page Down | Move by a screenful |
| Enter | Insert a newline |
| Backspace | Delete the character before the cursor; at the start of a line, join it to the previous line |
| Delete | Delete the character under the cursor; at the end of a line, join the next line |
| Tab | Insert a tab character (displayed as four spaces) |
| `Ctrl+S` | Save |
| `Ctrl+Q` | Quit; if there are unsaved changes, press it again to discard them |
| `Esc` | Cancel a filename prompt |

## Features

- C and C++ syntax highlighting for common source/header extensions
- Line numbers, status bar, filename, modified indicator, line/column display
- Vertical and horizontal scrolling
- Insert, delete, split, and join lines
- Save and open text files
- Preserves whether an opened file ends with a newline
- Uses the terminal's alternate screen and restores terminal settings on exit

## Notes

This is a compact educational editor, not a replacement for a mature editor. It does not yet include undo/redo, search, mouse support, or full Unicode display-width handling. It expects an interactive POSIX terminal (Linux, macOS, or a POSIX environment such as WSL).
