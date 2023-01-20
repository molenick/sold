#!/bin/bash
. $(dirname $0)/common.inc

cat <<EOF | cc -o $t/a.o -c -xc -
void foo() {}
EOF

mkdir -p $t/x/y

cc --ld-path=./ld64 -shared -o $t/x/y/libfoo.dylib $t/a.o -Wl,-install_name,@executable_path/x/y/libfoo.dylib

cat <<EOF | cc -o $t/b.o -c -xc -
void bar() {}
EOF

cc --ld-path=./ld64 -shared -o $t/libbar.dylib $t/b.o -Wl,-reexport_library,$t/x/y/libfoo.dylib

objdump --macho --dylibs-used $t/libbar.dylib | grep -q 'libfoo.*reexport'

cat <<EOF | cc -o $t/d.o -c -xc -
void foo();
void bar();

int main() {
  foo();
  bar();
}
EOF

cc --ld-path=./ld64 -o $t/exe $t/d.o -L$t -lbar
