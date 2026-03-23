# slopsh

A very-minimal UNIX shell written in C. Project 1 from *Operating System Concepts* (Silberschatz et al., 2018).

## Features

- Command execution via `fork` + `execvp`
- Background execution (`&`)
- I/O redirection (`<`, `>`)
- Pipe (`|`)
- Command history with `!!` (36 entries, ↑/↓ to navigate)
- Cursor movement (←/→, Ctrl-A, Ctrl-E)
- Built-ins: `cd`, `pwd`, `export`, `unset`, `exit`

## Build

Requires [Meson](https://mesonbuild.com/) and a C compiler.

```sh
meson setup build
cd build && ninja
./slopsh
```

## License

[MIT](./LICENSE)
