<p align="center">
  <a href="https://query.farm">
    <picture>
      <source media="(prefers-color-scheme: dark)" srcset="https://query.farm/media-kit/logo/wordmark-dark.svg">
      <img alt="Query.Farm" src="https://query.farm/media-kit/logo/wordmark-light.svg" height="64">
    </picture>
  </a>
</p>

# DuckDB Shellfs Extension

[![DuckDB](https://img.shields.io/badge/DuckDB-community_extension-fdf1e0?logo=duckdb&logoColor=fff000)](https://duckdb.org/community_extensions/extensions/shellfs.html)
[![v1.5 build](https://github.com/Query-farm/shellfs/actions/workflows/MainDistributionPipeline.yml/badge.svg?branch=v1.5)](https://github.com/Query-farm/shellfs/actions/workflows/MainDistributionPipeline.yml?query=branch%3Av1.5)

The `shellfs` extension for DuckDB enables the use of Unix pipes for input and output. By appending a pipe character `|` to a filename, DuckDB will treat it as a series of commands to execute and capture the output. Conversely, if you prefix a filename with `|`, DuckDB will treat it as an output pipe.

## Documentation

Full documentation, including installation, usage, the function reference, and cookbook examples, is available at:

**[https://query.farm/products/extensions/shellfs](https://query.farm/products/extensions/shellfs)**

## Installation

```sql
install shellfs from community;
load shellfs;
```

## Development

For instructions on building the extension from source and running its tests, see [BUILDING.md](BUILDING.md).

## Compress an output pipe once

When the command runs `gzip`, disable DuckDB's automatic compression explicitly.
Otherwise a destination ending in `.gz` can cause DuckDB to compress the CSV before
the pipe compresses it again.

```sql
COPY (SELECT * FROM range(3) AS t(n)) TO '| gzip > out.csv.gz'
  (FORMAT CSV, HEADER, COMPRESSION UNCOMPRESSED);
SELECT * FROM read_csv('out.csv.gz');
```
