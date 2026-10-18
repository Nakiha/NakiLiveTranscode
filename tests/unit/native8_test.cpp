// Storage lifecycle, signed residual precision, and exact SIMD SAD at guard pages.
#include "Native8.h"
#if defined(__SSE2__)
#include "x86/Native8X86.h"
#endif
#include <cassert>
#include <cstdio>
#include <sys/mman.h>
#include <unistd.h>
#include <limits>
using namespace vvenc;
int main() {
  static_assert(std::numeric_limits<Residual>::is_signed,"residuals must stay signed");
  assert(Residual(-255)==-255 && Residual(1023)==1023);
  ReferencePlane8 ref;const int pitch=39,w=17,h=9,mx=5,my=3;
  std::vector<WorkingSample> input(pitch*(h+2*my));
  auto origin=input.data()+my*pitch+mx;
  for(int y=-my;y<h+my;y++)for(int x=-mx;x<w+mx;x++)origin[y*pitch+x]=(x*7+y*11+10000)&255;
  assert(!ref.data());ref.assign(origin,pitch,w,h,mx,my);auto bytes=ref.data();assert(bytes);
  for(int y=-my;y<h+my;y++)for(int x=-mx;x<w+mx;x++)assert(bytes[y*pitch+x]==origin[y*pitch+x]);
  ref.reset();assert(!ref.data());origin[0]=-1;
  bool rejected=false;try {ref.assign(origin,pitch,w,h,mx,my);}catch(const std::range_error&){rejected=true;}
  assert(rejected&&!ref.data());origin[0]=256;rejected=false;
  try {ref.assign(origin,pitch,w,h,mx,my);}catch(const std::range_error&){rejected=true;}
  assert(rejected&&!ref.data());origin[0]=255;
  ref.assign(origin,pitch,w,h,mx,my);ref.destroy();assert(!ref.data()&&!ref.bytes());
  // Exact end-of-row loads: any overread of a narrow/tail block faults.
  const long page=sysconf(_SC_PAGESIZE);assert(page>0);
  auto a=(Sample8*)mmap(nullptr,page*2,PROT_READ|PROT_WRITE,MAP_ANONYMOUS|MAP_PRIVATE,-1,0);
  auto b=(Sample8*)mmap(nullptr,page*2,PROT_READ|PROT_WRITE,MAP_ANONYMOUS|MAP_PRIVATE,-1,0);
  assert(a!=MAP_FAILED&&b!=MAP_FAILED);assert(!mprotect(a+page,page,PROT_NONE)&&!mprotect(b+page,page,PROT_NONE));
  uint32_t seed=0x12345678;
  for(int width:{1,2,4,7,8,15,16,31,32,64,128})for(int height:{1,2,4,8,16,64,128})for(int shift:{0,1,2}) {
    if(height%(1<<shift))continue;
    const ptrdiff_t stride=width+5;std::vector<Sample8> x(stride*height),y(stride*height);
    uint64_t expected=0;
    for(int j=0;j<height;j++)for(int i=0;i<width;i++) {
      seed=seed*1664525+1013904223;x[j*stride+i]=seed>>24;
      seed=seed*1664525+1013904223;y[j*stride+i]=seed>>24;
      if(j%(1<<shift)==0)expected+=unsigned(std::abs(int(x[j*stride+i])-int(y[j*stride+i])));
    }
    expected<<=shift;
    assert(sad8Scalar(x.data(),stride,y.data(),stride,width,height,shift)==expected);
#if defined(__SSE2__)
    assert(sad8X86<false>(x.data(),stride,y.data(),stride,width,height,shift)==expected);
#ifdef USE_AVX2
    assert(sad8X86<true>(x.data(),stride,y.data(),stride,width,height,shift)==expected);
#endif
#endif
    for(uint64_t bound:{uint64_t(0),expected/2,expected}) {
      auto valid=[&](uint64_t cost){assert(cost<=expected);assert(expected<=bound?cost==expected:cost>bound);};
      valid(sad8Scalar(x.data(),stride,y.data(),stride,width,height,shift,bound));
#if defined(__SSE2__)
      valid(sad8X86<false>(x.data(),stride,y.data(),stride,width,height,shift,bound));
#ifdef USE_AVX2
      valid(sad8X86<true>(x.data(),stride,y.data(),stride,width,height,shift,bound));
#endif
#endif
    }
    if(height==1&&!shift) {
      std::copy_n(x.data(),width,a+page-width);std::copy_n(y.data(),width,b+page-width);
#if defined(__SSE2__)
      assert(sad8X86<false>(a+page-width,width,b+page-width,width,width,1,0)==expected);
#ifdef USE_AVX2
      assert(sad8X86<true>(a+page-width,width,b+page-width,width,width,1,0)==expected);
#endif
#endif
    }
  }
  assert(!munmap(a,page*2)&&!munmap(b,page*2));
  puts("PASS byte reference lifecycle/margins, signed residuals, scalar/SIMD SAD and guard pages");
}
