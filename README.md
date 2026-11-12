# db

A small SQLite-like database written in C, following the
[Let's Build a Simple Database](https://cstack.github.io/db_tutorial/) tutorial.

## Build

```sh
gcc main.c -o db
```

## Usage

```
$ ./db
db > insert 1 user1 person1@example.com
Executed.
db > select
(1, user1, person1@example.com)
Executed.
db > .exit
```

## Test

```sh
./test.sh
```
