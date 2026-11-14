# db

A small SQLite-like database written in C, following the
[Let's Build a Simple Database](https://cstack.github.io/db_tutorial/) tutorial.

## Build

```sh
gcc main.c row.c pager.c node.c btree.c repl.c -o db
```

## Layout

| File | Contents |
| --- | --- |
| `util.h` | `node_at` helper |
| `row.h/.c` | `Row` and its serialized layout |
| `pager.h/.c` | page cache over the db file |
| `node.h/.c` | B-tree node layout, accessors, `.btree` / `.constants` output |
| `btree.h/.c` | `Table`, `Cursor`, search, insert/split, `db_open` / `db_close` |
| `repl.h/.c` | input buffer, meta commands, prepare/execute |
| `main.c` | REPL loop |

## Usage

```
$ ./db mydb.db
db > insert 1 user1 person1@example.com
Executed.
db > select
(1, user1, person1@example.com)
Executed.
db > .exit
```

## Test

```sh
gcc test.c -o test_db && ./test_db
```
