#include "kiwiwaterfall.h"
#include <cassert>
#include <cmath>
int main(){
 auto v=KiwiWaterfall::view(7171000,1800000);assert(v && v->zoom==4 && v->lowHz<7171000 && v->highHz>7171000);
 assert(std::abs(v->highHz-v->lowHz-1875000)<1);
 auto edge=KiwiWaterfall::view(5000,1800000);assert(edge && edge->lowHz==0);
 assert(!KiwiWaterfall::view(50000000,1800000));
 std::vector<std::uint8_t> row(1040,155);row[0]='W';row[1]='/';row[2]='F';row[3]=0;
 row[4]=1;row[5]=0;row[6]=0;row[7]=0;row[8]=4;row[9]=0;row[10]=0;row[11]=0;
 auto parsed=KiwiWaterfall::decode(row.data(),row.size());assert(parsed && parsed->start==1 && parsed->zoom==4 && parsed->spectrum.db[0]==-100);
 row[10]=1;assert(!KiwiWaterfall::decode(row.data(),row.size())); // compressed is deliberately not treated as raw
 row.resize(1028);assert(KiwiWaterfall::decode(row.data(),row.size()));
}
