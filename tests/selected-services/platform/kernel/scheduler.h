#pragma once
#include <common.h>
namespace skyline::kernel {using CoreMask=std::bitset<4>;struct Priority{i8 min,max;bool Valid(i8 value)const{return value>=min&&value<=max;}};}
