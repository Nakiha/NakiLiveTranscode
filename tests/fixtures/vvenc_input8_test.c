/* Packed byte input API: read-only planes, rejection boundaries and exact
 * payload/CTS equivalence to legacy 8-bit samples in 16-bit containers. */
#include <vvenc/vvenc.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#ifndef VVENC_LIVE_INPUT8_VERSION
#error Matching packed-byte VVenC fork required
#endif
#define WIDTH 96
#define HEIGHT 64
#define FRAMES 14
#define CAPACITY (4*1024*1024)
typedef struct Result {uint8_t data[CAPACITY];size_t used;int count;int64_t byte_blocks;} Result;
static void param(vvenc_config *c,const char *k,const char *v) {assert(!vvenc_set_param(c,k,v));}
static int native_ref=0, forced_ifp=0;
static vvencEncoder *open_encoder(int depth,int chroma,int extension) {
    vvenc_config c;assert(!vvenc_init_default(&c,WIDTH,HEIGHT,30,0,32,VVENC_FASTER));
    c.m_numThreads=forced_ifp?8:1;c.m_usePerceptQPA=1;c.m_inputBitDepth[0]=depth;c.m_inputBitDepth[1]=chroma;
    c.m_internalBitDepth[0]=depth;c.m_internalBitDepth[1]=chroma;c.m_MSBExtendedBitDepth[0]=depth+extension;c.m_MSBExtendedBitDepth[1]=chroma+extension;
    c.m_livePtsGop=1;c.m_liveCqfPoll=1;c.m_liveFrameIntra=1;c.m_TicksPerSecond=90000;c.m_RCNumPasses=1;
    c.m_DecodingRefreshType=VVENC_DRT_CRA;param(&c,"RefreshSec","2");
    param(&c,"GOPSize","4");param(&c,"IntraPeriod","60");param(&c,"POC0IDR","1");param(&c,"STA","0");
    param(&c,"MaxParallelFrames","0");param(&c,"LookAhead","0");param(&c,"MCTF","1");param(&c,"BIM","0");param(&c,"MCTFFutureReference","0");
    param(&c,"Frame1","P 4 0 0 0 0 0 1.0 0 0 0 0 0 0 0 1 1 4 0 0");
    param(&c,"Frame2","B 2 1 0 0 0 0 1.0 0 0 0 0 0 0 1 1 1 2 1 1 -2");
    param(&c,"Frame3","B 1 2 0 0 0 0 1.0 0 0 0 0 0 0 2 1 1 1 1 1 -1");
    param(&c,"Frame4","B 3 2 0 0 0 0 1.0 0 0 0 0 0 0 2 1 1 1 1 1 -1");
    if(forced_ifp)param(&c,"ifp","1");
    param(&c,"LiveNative8Ref",native_ref && depth==8 && chroma==8 && !extension?"1":"0");
    assert(c.m_liveNative8Ref==(native_ref && depth==8 && chroma==8 && !extension));
    vvencEncoder *encoder=vvenc_encoder_create();assert(encoder);int ret=vvenc_encoder_open(encoder,&c);
    if(ret)fprintf(stderr,"open error %d: %s\n",ret,vvenc_get_last_error(encoder));
    assert(!ret);return encoder;
}
static void collect(Result *r,const vvencAccessUnit *au) {
    if(!au->payloadUsedSize)return;
    r->byte_blocks+=au->liveTrace.byte_me_blocks;
    size_t bytes=(size_t)au->payloadUsedSize;assert(r->used+bytes+sizeof(au->cts)<CAPACITY);
    memcpy(r->data+r->used,&au->cts,sizeof(au->cts));r->used+=sizeof(au->cts);
    memcpy(r->data+r->used,au->payload,bytes);r->used+=bytes;r->count++;
}
static void run(int packed,Result *r) {
    vvencEncoder *encoder=open_encoder(8,8,0);vvencAccessUnit *au=vvenc_accessUnit_alloc();assert(au);
    vvenc_accessUnit_alloc_payload(au,1024*1024);assert(au->payload);
    vvencYUVBuffer input;memset(&input,0xa5,sizeof(input));vvenc_YUVBuffer_default(&input);
    assert(!input.liveTrace&&!input.planes8[0]&&!input.planes8[1]&&!input.planes8[2]);
    uint8_t *bytes[3];int16_t *wide[3];size_t sizes[3];long page=sysconf(_SC_PAGESIZE);assert(page>0);
    for(int c=0;c<3;c++) {
        int w=c?WIDTH/2:WIDTH,h=c?HEIGHT/2:HEIGHT,stride=w+7;
        input.planes[c].width=w;input.planes[c].height=h;input.planes[c].stride=stride;
        sizes[c]=((size_t)stride*h+(size_t)page-1)/(size_t)page*(size_t)page;
        bytes[c]=mmap(NULL,sizes[c],PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);assert(bytes[c]!=MAP_FAILED);
        wide[c]=calloc((size_t)stride*h,sizeof(int16_t));assert(wide[c]);
        if(packed)input.planes8[c]=bytes[c];else input.planes[c].ptr=wide[c];
    }
    bool done=false;
    if(packed) {
        vvencYUVBuffer bad=input;bad.planes[0].stride=WIDTH-1;assert(vvenc_encode(encoder,&bad,au,&done)!=0);
        bad=input;bad.planes8[2]=NULL;assert(vvenc_encode(encoder,&bad,au,&done)!=0);
        bad=input;bad.planes[1].height--;assert(vvenc_encode(encoder,&bad,au,&done)!=0);
        bad=input;bad.planes[0].ptr=wide[0];assert(vvenc_encode(encoder,&bad,au,&done)!=0);
        vvencEncoder *ten=open_encoder(10,10,0);assert(vvenc_encode(ten,&input,au,&done)!=0);assert(!vvenc_encoder_close(ten));
        vvencEncoder *mixed=open_encoder(8,10,0);assert(vvenc_encode(mixed,&input,au,&done)!=0);assert(!vvenc_encoder_close(mixed));
        vvencEncoder *extended=open_encoder(8,8,2);assert(vvenc_encode(extended,&input,au,&done)!=0);assert(!vvenc_encoder_close(extended));
    }
    for(int f=0;f<FRAMES;f++) {
        for(int c=0;c<3;c++) {
            assert(!mprotect(bytes[c],sizes[c],PROT_READ|PROT_WRITE));
            for(int y=0;y<input.planes[c].height;y++)for(int x=0;x<input.planes[c].stride;x++) {
                int p=y*input.planes[c].stride+x;bytes[c][p]=(uint8_t)((x*17+y*29+f*13+c*59)&255);wide[c][p]=bytes[c][p];
            }
            assert(!mprotect(bytes[c],sizes[c],PROT_READ));
        }
        input.liveTrace=true;input.cts=90000+f*3000;input.ctsValid=true;input.forceIntra=f==0||f==5||f==11;input.liveQpOffset=f%3-1;
        assert(!vvenc_encode(encoder,&input,au,&done));collect(r,au);
    }
    for(int i=0;!done&&i<100;i++) {assert(!vvenc_encode(encoder,NULL,au,&done));collect(r,au);}
    assert(done&&r->count==FRAMES);assert(!vvenc_encoder_close(encoder));vvenc_accessUnit_free(au,true);
    for(int c=0;c<3;c++) {assert(!munmap(bytes[c],sizes[c]));free(wide[c]);}
}
int main(int argc,char **argv) {
    if(argc==2)printf("SIMD: %s\n",vvenc_set_SIMD_extension(argv[1]));
    Result *legacy=calloc(1,sizeof(*legacy)),*packed=calloc(1,sizeof(*packed));assert(legacy&&packed);
    run(0,legacy);run(1,packed);assert(legacy->used==packed->used&&!memcmp(legacy->data,packed->data,legacy->used));
    memset(packed,0,sizeof(*packed));native_ref=1;run(1,packed);
    assert(legacy->used==packed->used&&!memcmp(legacy->data,packed->data,legacy->used));
    assert(packed->byte_blocks>0 && !legacy->byte_blocks);
    printf("Byte ME blocks: %lld\n",(long long)packed->byte_blocks);
    forced_ifp=1;native_ref=0;memset(legacy,0,sizeof(*legacy));run(1,legacy);
    native_ref=1;memset(packed,0,sizeof(*packed));run(1,packed);
    assert(packed->byte_blocks>0 && legacy->used==packed->used&&!memcmp(legacy->data,packed->data,legacy->used));
    free(legacy);free(packed);puts("PASS read-only packed byte input, rejection boundaries, forced I/EOF and legacy payload/CTS equivalence");return 0;
}
