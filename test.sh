#!/usr/bin/env bash
# Usage: ./test.sh   (build ./db first)
# Pipes commands into ./db and diffs the actual output against the expected.

DB=./db
pass=0
fail=0

# check <name> <input> <expected output>
check() {
  local name=$1 input=$2 expected=$3 actual
  actual=$(printf '%s\n' "$input" | "$DB")
  if [ "$actual" == "$expected" ]; then
    echo "PASS  $name"
    pass=$((pass + 1))
  else
    echo "FAIL  $name"
    diff <(echo "$expected") <(echo "$actual") | sed 's/^/      /'
    fail=$((fail + 1))
  fi
}

check "inserts and retrieves a row" \
  "insert 1 user1 person1@example.com
select
.exit" \
  "db > Executed.
db > (1, user1, person1@example.com)
Executed.
db > "

check "reports a syntax error" \
  "insert 1 user1
.exit" \
  "db > Syntax error. Could not parse statement.
db > "

check "rejects an unrecognized statement" \
  "foo
.exit" \
  "db > Unrecognized keyword at start of 'foo'.
db > "

check "rejects an unrecognized meta-command" \
  ".foo
.exit" \
  "db > Unrecognized command '.foo'
db > "

long_username=$(printf 'a%.0s' $(seq 32))
long_email=$(printf 'a%.0s' $(seq 255))

check "allows strings of maximum length" \
"insert 1 $long_username $long_email
select
.exit" \
"db > Executed.
db > (1, $long_username, $long_email)
Executed.
db > "

check "rejects strings that are too long" \
"insert 1 ${long_username}a ${long_email}a
select
.exit" \
"db > String is too long.
db > Executed.
db > "

check "rejects a negative id" \
"insert -1 cstack foo@bar.com
select
.exit" \
"db > ID must be positive.
db > Executed.
db > "

# Table full: insert 1401 rows and check only the last two lines of output
input=$(
  for i in $(seq 1 1401); do echo "insert $i user$i person$i@example.com"; done
  echo ".exit"
)
actual=$(printf '%s\n' "$input" | "$DB" | tail -n 2)
expected="db > Error: Table full.
db > "
if [ "$actual" == "$expected" ]; then
  echo "PASS  reports an error when the table is full"
  pass=$((pass + 1))
else
  echo "FAIL  reports an error when the table is full"
  diff <(echo "$expected") <(echo "$actual") | sed 's/^/      /'
  fail=$((fail + 1))
fi

echo
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
