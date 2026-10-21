#!/usr/bin/env python3
"""Cross-check owned image digest against Python's independent SHA-256 implementation."""
from pathlib import Path
import hashlib
import os
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[2]


def main():
    source = r'''
#include "image_digest.h"
#include <cstdio>
#include <cstdlib>
#include <vector>
int main(int argc,char **argv) {
    if(argc!=2)return 1;
    const size_t count=std::strtoull(argv[1],nullptr,10);
    std::vector<uint8_t> bytes(count);
    for(size_t i{};i<count;++i)bytes[i]=uint8_t(i*37+(i>>3)+11);
    const auto result=skyline::diagnostics::image::Sha256(bytes);
    for(auto byte:result)std::printf("%02x",byte);
    std::puts("");
}
'''
    with tempfile.TemporaryDirectory(prefix='pine-image-digest-') as directory:
        out=Path(directory);(out/'digest.cpp').write_text(source)
        subprocess.run([os.environ.get('CXX','c++'),'-std=c++20','-O2','-Wall','-Wextra','-Werror',
                        '-I'+str(ROOT/'app/src/main/cpp/skyline/diagnostics'),str(out/'digest.cpp'),'-o',str(out/'digest')],check=True)
        sizes=list(range(130))+[255,256,257,4095,4096,4097,65535,65536,1048576,1000000]
        for size in sizes:
            expected=hashlib.sha256(bytes((i*37+(i>>3)+11)&255 for i in range(size))).hexdigest()
            actual=subprocess.check_output([str(out/'digest'),str(size)],text=True).strip()
            assert actual==expected,(size,expected,actual)
        print(f'PASS image digest: {len(sizes)} independent SHA-256 padding/block/megabyte comparisons')


if __name__=='__main__':main()
