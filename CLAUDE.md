# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

This is the `shellfs` DuckDB extension that enables Unix pipes for input/output operations. It allows DuckDB to:
- Read from shell commands by appending `|` to a filename (e.g., `read_csv('seq 1 100 |')`)
- Write to shell commands by prefixing `|` to a filename (e.g., `COPY ... TO '| grep 6 > output.csv'`)

The extension is a DuckDB Community Extension using `popen()` for process pipes.

## Build Commands

This extension does not use vcpkg.

```bash
# Build release version
GEN=ninja make release

# Build debug version
GEN=ninja make debug

# Run all tests (requires building first)
make test              # runs against release build
make test_debug        # runs against debug build

# Format code
make format

```

Build outputs:
- `./build/release/duckdb` - DuckDB shell with extension loaded
- `./build/release/test/unittest` - Test runner
- `./build/release/extension/shellfs/shellfs.duckdb_extension` - Loadable extension

All extension functions should be documented inside of DuckDB with CreateScalarFunctionInfo or CreateAggregateFunctionInfo or the appropriate type for the function.  This documentation of the function should include examples, parameter types and parameter names.  The function should be categorized.

When making changes the version should always be updated to the current date plus an ordinal counter in the form of YYYYMMDDCC.


## Running a Single Test

```bash
./build/release/test/unittest "test/sql/shellfs.test"
```

## Architecture

The extension registers a custom filesystem (`ShellFileSystem`) with DuckDB:

- **`src/shellfs_extension.cpp`**: Extension entry point. Registers `ShellFileSystem` and the `ignore_sigpipe` config option.
- **`src/shell_file_system.cpp`**: Core implementation. `ShellFileSystem` handles pipe detection (paths starting or ending with `|`) and uses `popen()`/`pclose()` for process I/O. `ShellFileHandle` manages pipe lifecycle and exit code validation.
- **`src/include/shell_file_system.hpp`**: Header defining `ShellFileSystem` class.

Key behavior:
- Paths ending with `|` are read pipes; paths starting with `|` are write pipes
- Exit code specification via `{allowed_exit_codes=N,M}|` suffix allows non-zero exits (useful for `grep`)
- `ignore_sigpipe` setting prevents crashes when writing to pipes that close early

## Test Structure

Tests use DuckDB's SQLLogicTest format in `test/sql/`. Tests require the `shellfs` extension via `require shellfs` directive.
