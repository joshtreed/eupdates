# Contributing

Thanks for your interest in eupdates.

## Build & test

```sh
make CFLAGS="-O2 -std=c11 -Wall -Wextra -Werror"
./eupdates --help
./eupdates check && ./eupdates status
```

The core is a single zero-dependency C file (`src/eupdates.c`) targeting C11 on
glibc **and** musl. Keep it dependency-free and POSIX (`_POSIX_C_SOURCE`
200809L); avoid GNU-isms so the musl build stays clean.

## Style

- Run `clang-format` (config in `.clang-format`) before submitting:
  `clang-format -i src/*.c`.
- Match the surrounding idiom; no dead code, keep functions small.

## Commits

- [Conventional Commits](https://www.conventionalcommits.org/): `feat:`,
  `fix:`, `refactor:`, `perf:`, `docs:`, `build:`, `chore:`, `test:`; `!` for
  breaking changes.
- Sign off your commits (`git commit -s`) — this project uses the Developer
  Certificate of Origin, which also eases any future move into a Gentoo overlay.

## Scope

eupdates is deliberately small: count pending portage updates, cheaply, for a
status bar. Please open an issue before large features so we can keep it lean.
