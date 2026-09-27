#!/usr/bin/env python3
"""Asset reads are bounded, preserve bytes and reject premature EOF."""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parent
source=(root/'reimpl.c').read_text()
a=source.index('static u8 *pc_load_file(')
body=source[a:source.index('\n}',a)+2]
program=r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
typedef uint8_t u8;
typedef uint32_t u32;
static unsigned calls, short_read;
static size_t checked_read(void *data,size_t size,size_t count,FILE *f) {
    assert(size==1 && count<=32768); ++calls;
    if(short_read && calls==2) return 0;
    return fread(data,size,count,f);
}
#define fread checked_read
'''+body+r'''
int main(int argc,char **argv) {
    u32 size; short_read=argc>2;
    u8 *data=pc_load_file(argv[1],&size);
    assert(!short_read);
    assert(calls==(size+32767)/32768);
    for(u32 i=0;i<size;i++) assert(data[i]==(uint8_t)i);
    free(data);
}
'''
with tempfile.TemporaryDirectory() as directory:
    p=Path(directory);(p/'check.c').write_text(program)
    subprocess.run(['cc',str(p/'check.c'),'-o',str(p/'check')],check=True)
    for size in (208,32768,65553,10340864):
        data=(bytes(range(256))*((size+255)//256))[:size]
        (p/'asset.bin').write_bytes(data)
        subprocess.run([str(p/'check'),str(p/'asset.bin')],check=True)
    result=subprocess.run([str(p/'check'),str(p/'asset.bin'),'short'],capture_output=True)
    assert result.returncode==1 and b'short read' in result.stderr
print('Asset reads: bounded requests, exact bytes, partial tail and short-read failure passed')
