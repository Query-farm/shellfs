# Shellfs repository notes

For installation and the full user documentation, see the
[Query Farm Shellfs documentation](https://query.farm/products/extensions/shellfs/).

## Compress an output pipe once

When the command runs `gzip`, disable DuckDB's automatic compression explicitly.
Otherwise a destination ending in `.gz` can cause DuckDB to compress the CSV before
the pipe compresses it again.

```sql
COPY (SELECT * FROM range(3) AS t(n)) TO '| gzip > out.csv.gz'
  (FORMAT CSV, HEADER, COMPRESSION UNCOMPRESSED);
SELECT * FROM read_csv('out.csv.gz');
```
