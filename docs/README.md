# DuckDB Shellfs Extension

![DuckDB Shellfs Extension logo](duckdb-shellfs.jpg)

The `shellfs` extension for DuckDB enables the use of Unix pipes for input and output.

By appending a pipe character `|` to a filename, DuckDB will treat it as a series of commands to execute and capture the output. Conversely, if you prefix a filename with `|`, DuckDB will treat it as an output pipe.

While the examples provided are simple, in practical scenarios, you might use this feature to run another program that generates CSV, JSON, or other formats to manage complexity that DuckDB cannot handle directly.

The implementation uses `popen()` to create the pipe between processes.

## Examples

### Reading input from a pipe

```sql
-- Generate a sequence only return numbers that contain a 2
SELECT * from read_csv('seq 1 100 | grep 2 |');
┌─────────┐
│ column0 │
│  int64  │
├─────────┤
│       2 │
│      12 │
│      20 │
│      21 │
│      22 │
└─────────┘

-- Get the first multiples of 7 between 1 and 3 5
-- demonstrate how commands can be chained together
SELECT * from read_csv('seq 1 35 | awk "\$1 % 7 == 0" | head -n 2 |');
┌─────────┐
│ column0 │
│  int64  │
├─────────┤
│       7 │
│      14 │
└─────────┘

-- Do some arbitrary curl
SELECT abbreviation, unixtime from
read_json('curl -s http://worldtimeapi.org/api/timezone/Etc/UTC  |');
┌──────────────┬────────────┐
│ abbreviation │  unixtime  │
│   varchar    │   int64    │
├──────────────┼────────────┤
│ UTC          │ 1715983565 │
└──────────────┴────────────┘
```


Create a program to generate CSV in Python:

```python
#!/usr/bin/env python3

print("counter1,counter2")
for i in range(10000000):
    print(f"{i},{i}")
```

Run that program and determine the number of distinct values it produces:

```sql
select count(distinct counter1)
from read_csv('./test-csv.py |');
┌──────────────────────────┐
│ count(DISTINCT counter1) │
│          int64           │
├──────────────────────────┤
│                 10000000 │
└──────────────────────────┘
```



### Writing output to a pipe

```sql
-- Write all numbers from 1 to 30 out, but then filter via grep
-- for only lines that contain 6.
COPY (select * from unnest(generate_series(1, 30)))
TO '| grep 6 > numbers.csv' (FORMAT 'CSV');
6
16
26

-- Copy the result set to the clipboard on Mac OS X using pbcopy
COPY (select 'hello' as type, from unnest(generate_series(1, 30)))
TO '| grep 3 | pbcopy' (FORMAT 'CSV');
type,"generate_series(1, 30)"
hello,3
hello,13
hello,23
hello,30

-- Write an encrypted file out via openssl
COPY (select 'hello' as type, * from unnest(generate_series(1, 30)))
TO '| openssl enc -aes-256-cbc -salt -in - -out example.enc -pbkdf2 -iter 1000 -pass pass:testing12345' (FORMAT 'JSON');

```

## Configuration

This extension introduces a new configuration option:

`ignore_sigpipe` - a boolean option that, when set to true, ignores the SIGPIPE signal. This is useful when writing to a pipe that stops reading input. For example:

```sql
COPY (select 'hello' as type, * from unnest(generate_series(1, 300))) TO '| head -n 100';
```

In this scenario, DuckDB attempts to write 300 lines to the pipe, but the `head` command only reads the first 100 lines. After `head` reads the first 100 lines and exits, it closes the pipe. The next time DuckDB tries to write to the pipe, it receives a SIGPIPE signal. By default, this causes DuckDB to exit. However, if `ignore_sigpipe` is set to true, the SIGPIPE signal is ignored, allowing DuckDB to continue without error even if the pipe is closed.

You can enable this option by setting it with the following command:

```sql
set ignore_sigpipe = true;
```

## Caveats

When using `read_text()` or `read_blob()` the contents of the data read from a pipe is limited to 2GB in size.  This is the maximum length of a single row's value.

When using `read_csv()` or `read_json()` the contents of the pipe can be unlimited as it is processed in a streaming fashion.

A demonstration of this would be:

```python
#!/usr/bin/env python3

print("counter1,counter2")
for i in range(10000000):
    print(f"{i},{i}")
```

```sql
select count(distinct counter1) from read_csv('./test-csv.py |');
┌──────────────────────────┐
│ count(DISTINCT counter1) │
│          int64           │
├──────────────────────────┤
│                 10000000 │
└──────────────────────────┘
```

If a `limit` clause is used you may see an error like this:

```sql
select * from read_csv('./test-csv.py |') limit 3;
┌──────────┬──────────┐
│ counter1 │ counter2 │
│  int64   │  int64   │
├──────────┼──────────┤
│        0 │        0 │
│        1 │        1 │
│        2 │        2 │
└──────────┴──────────┘
Traceback (most recent call last):
  File "/Users/rusty/Development/duckdb-shell-extension/./test-csv.py", line 5, in <module>
    print(f"{i},{i}")
BrokenPipeError: [Errno 32] Broken pipe
Exception ignored in: <_io.TextIOWrapper name='<stdout>' mode='w' encoding='utf-8'>
BrokenPipeError: [Errno 32] Broken pipe
```

DuckDB continues to run, but the program that was producing output received a SIGPIPE signal because DuckDB closed the pipe after reading the necessary number of rows.  It is up to the user of DuckDB to decide whether to suppress this behavior by setting the `ignore_sigpipe` configuration parameter.

## Building

### Build steps
Now to build the extension, run:
```sh
make
```
The main binaries that will be built are:
```sh
./build/release/duckdb
./build/release/test/unittest
./build/release/extension/shellfs/shellfs.duckdb_extension
```
- `duckdb` is the binary for the duckdb shell with the extension code automatically loaded.
- `unittest` is the test runner of duckdb. Again, the extension is already linked into the binary.
- `shellfs.duckdb_extension` is the loadable binary as it would be distributed.

## Running the extension
To run the extension code, simply start the shell with `./build/release/duckdb`.

Now we can use the features from the extension directly in DuckDB.

## Running the tests
Different tests can be created for DuckDB extensions. The primary way of testing DuckDB extensions should be the SQL tests in `./test/sql`. These SQL tests can be run using:
```sh
make test
```

### Installing the deployed binaries
To install your extension binaries from S3, you will need to do two things. Firstly, DuckDB should be launched with the
`allow_unsigned_extensions` option set to true. How to set this will depend on the client you're using. Some examples:

CLI:
```shell
duckdb -unsigned
```

Python:
```python
con = duckdb.connect(':memory:', config={'allow_unsigned_extensions' : 'true'})
```

NodeJS:
```js
db = new duckdb.Database(':memory:', {"allow_unsigned_extensions": "true"});
```

Secondly, you will need to set the repository endpoint in DuckDB to the HTTP url of your bucket + version of the extension
you want to install. To do this run the following SQL query in DuckDB:
```sql
SET custom_extension_repository='bucket.s3.eu-west-1.amazonaws.com/shellfs/latest';
```
Note that the `/latest` path will allow you to install the latest extension version available for your current version of
DuckDB. To specify a specific version, you can pass the version instead.

After running these steps, you can install and load your extension using the regular INSTALL/LOAD commands in DuckDB:

```sql
INSTALL shellfs
LOAD shellfs
```
