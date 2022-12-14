#!/bin/bash
. $(dirname $0)/common.inc

cat <<EOF | cc -o $t/a.o -fcommon -c -xc -
int foo;
__attribute__((aligned(4096))) int bar;
EOF

cat <<EOF | cc -o $t/b.o -c -xc -
#include <stdio.h>
#include <stdint.h>

extern int foo;
extern int bar;

int main() {
  printf("%lu %lu\n", (uintptr_t)&foo % 4, (uintptr_t)&bar % 4096);
}
EOF

cc --ld-path=./ld64 -o $t/exe $t/a.o $t/b.o
$t/exe | grep -q '^0 0$'
