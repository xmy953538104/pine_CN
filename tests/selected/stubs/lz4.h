// Default controlled lane deliberately rejects compressed data. The true-AES
// lane places the pinned real LZ4 header/source before this declaration.
#pragma once
inline int LZ4_decompress_safe(const char*,char*,int,int){return -1;}
