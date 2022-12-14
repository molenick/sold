#!/bin/bash
. $(dirname $0)/common.inc

cat <<EOF | cc -o $t/a.o -c -xc -
#include <stdio.h>
void hello() { printf("Hello world\n"); }
EOF

cat <<EOF | cc -o $t/b.o -c -xc -
void hello();
int main() { hello(); }
EOF

cat <<EOF > $t/filelist
a.o
b.o
EOF

cc --ld-path=./ld64 -o $t/exe -Xlinker -filelist -Xlinker $t/filelist,$t
$t/exe | grep -q 'Hello world'
