/* Encode via the public TIFF writer, then independently decode each JXL strip. */
#include "td_test_util.h"
#ifdef TINYDNG_TEST_JXL
#include <jxl/decode.h>
#endif

static uint16_t get16(const uint8_t *p,int be) {
  return be ? (uint16_t)((p[0]<<8)|p[1]) : (uint16_t)((p[1]<<8)|p[0]);
}
static uint32_t get32(const uint8_t *p,int be) {
  return be ? ((uint32_t)get16(p,be)<<16)|get16(p+2,be)
            : ((uint32_t)get16(p+2,be)<<16)|get16(p,be);
}
static const uint8_t *find_tag(const uint8_t *b,uint16_t tag,int be) {
  uint32_t off=get32(b+4,be); unsigned count=get16(b+off,be);
  for(unsigned i=0;i<count;i++) {
    const uint8_t *e=b+off+2+12*i;
    if(get16(e,be)==tag) return e;
  }
  return NULL;
}
#ifdef TINYDNG_TEST_JXL
static void decode_strip(const uint8_t *data,size_t size,const uint16_t *expected,
                          uint32_t width,uint32_t height) {
  JxlDecoder *d=JxlDecoderCreate(NULL);
  JxlPixelFormat format={1,JXL_TYPE_UINT16,JXL_NATIVE_ENDIAN,0};
  JxlBasicInfo info;
  size_t bytes=(size_t)width*height*2;
  uint16_t *pixels=(uint16_t*)malloc(bytes);
  int complete=0;
  CHECK(d && pixels,"decoder allocation");
  if(!d || !pixels) { if(d) JxlDecoderDestroy(d); free(pixels); return; }
  CHECK(JxlDecoderSubscribeEvents(d,JXL_DEC_BASIC_INFO|JXL_DEC_FULL_IMAGE)==JXL_DEC_SUCCESS,"subscribe");
  CHECK(JxlDecoderSetInput(d,data,size)==JXL_DEC_SUCCESS,"input");
  JxlDecoderCloseInput(d);
  for(unsigned i=0;i<8;i++) {
    JxlDecoderStatus st=JxlDecoderProcessInput(d);
    if(st==JXL_DEC_BASIC_INFO) {
      CHECK(JxlDecoderGetBasicInfo(d,&info)==JXL_DEC_SUCCESS,"basic info");
      CHECK(info.xsize==width && info.ysize==height && info.bits_per_sample==16 &&
            info.num_color_channels==1,"JXL raster geometry");
    } else if(st==JXL_DEC_NEED_IMAGE_OUT_BUFFER) {
      size_t required=0;
      CHECK(JxlDecoderImageOutBufferSize(d,&format,&required)==JXL_DEC_SUCCESS && required==bytes,"decoded buffer size");
      CHECK(JxlDecoderSetImageOutBuffer(d,&format,pixels,bytes)==JXL_DEC_SUCCESS,"output buffer");
    } else if(st==JXL_DEC_FULL_IMAGE) {
      complete=1; CHECK(!memcmp(pixels,expected,bytes),"lossless RAW16 samples");
    } else if(st==JXL_DEC_SUCCESS) break;
    else { CHECK(0,"JXL decode status %d",st); break; }
  }
  CHECK(complete,"complete JXL frame");
  free(pixels); JxlDecoderDestroy(d);
}
static void check_file(const uint8_t *b,size_t n,const uint16_t *pixels,
                       uint32_t width,uint32_t height,uint32_t rps,int be) {
  const uint8_t *oe=find_tag(b,273,be), *ce=find_tag(b,279,be);
  const uint8_t *comp=find_tag(b,259,be), *ver=find_tag(b,50706,be), *back=find_tag(b,50707,be);
  CHECK(oe && ce && comp && ver && back,"TIFF/JXL tags exist");
  if(!oe || !ce || !comp || !ver || !back) return;
  CHECK(get16(comp+8,be)==52546,"JPEG XL compression");
  CHECK(!memcmp(ver+8,"\1\7\0\0",4) && !memcmp(back+8,"\1\7\0\0",4),"DNG 1.7");
  uint32_t count=get32(oe+4,be);
  CHECK(count==(height+rps-1)/rps,"strip count");
  for(uint32_t i=0;i<count;i++) {
    uint32_t off=count==1 ? get32(oe+8,be) : get32(b+get32(oe+8,be)+i*4,be);
    uint32_t len=count==1 ? get32(ce+8,be) : get32(b+get32(ce+8,be)+i*4,be);
    uint32_t rows=height-i*rps; if(rows>rps) rows=rps;
    CHECK(off<=n && len<=n-off,"strip bounds");
    if(off<=n && len<=n-off) decode_strip(b+off,len,pixels+(size_t)i*rps*width,width,rows);
  }
}
#endif
int main(void) {
  tinydng_context *ctx=tinydng_context_create(NULL,NULL); tinydng_error err;
  enum { W=257,H=129 }; uint16_t *pixels=(uint16_t*)malloc(W*H*2);
  CHECK(ctx && pixels,"test allocation"); if(!ctx || !pixels) return 1;
  uint32_t state=17;
  for(unsigned i=0;i<W*H;i++) { state=state*1664525u+1013904223u; pixels[i]=(uint16_t)(state>>16); }
  tinydng_write_image img={0}; img.width=W; img.height=H; img.bits_per_sample=16;
  img.data=(uint8_t*)pixels; img.data_size=W*H*2;
  tinydng_write_options opts={0}; opts.compression=TINYDNG_COMPRESSION_JPEG_XL;
  uint8_t *b=NULL; size_t n=0;
#ifdef TINYDNG_TEST_JXL
  CHECK(tinydng_has_jxl_encoder(),"JXL available");
  for(int be=0;be<2;be++) {
    opts.big_endian=(uint8_t)be; opts.jxl_num_threads=be ? 2 : 1;
    CHECK(tinydng_write_memory(ctx,&img,&opts,&b,&n,&err)==TINYDNG_OK,"encode: %s",err.message);
    if(b) check_file(b,n,pixels,W,H,H,be);
    tinydng_buffer_free(ctx,b); b=NULL;
    tinydng_write_io io={0}; tinydng_writer *w=NULL; tinydng_tiling tiling={0}; tiling.rows_per_strip=17;
    CHECK(tinydng_write_io_open_memory(ctx,&io,&err)==TINYDNG_OK,"memory sink");
    CHECK(tinydng_writer_create(ctx,io,&img,&opts,&tiling,&w,&err)==TINYDNG_OK,"create strips");
    if(w) {
      /* Random strip ordering, final partial strip, and cached output scratch. */
      for(int i=(H+16)/17-1;i>=0;i--) CHECK(tinydng_writer_write_strip(w,(uint32_t)i,pixels+i*17*W,&err)==TINYDNG_OK,"encode strip");
      CHECK(tinydng_writer_finish(w,&err)==TINYDNG_OK,"finish strips");
    }
    CHECK(tinydng_write_io_memory_take(ctx,&io,&b,&n,&err)==TINYDNG_OK,"take buffer");
    if(b) check_file(b,n,pixels,W,H,17,be);
    if(io.close) io.close(&io);
    tinydng_buffer_free(ctx,b); b=NULL;
  }
  opts.jxl_effort=10;
  CHECK(tinydng_write_memory(ctx,&img,&opts,&b,&n,&err)==TINYDNG_E_INVALID_ARG,"bad effort rejected");
#else
  CHECK(!tinydng_has_jxl_encoder(),"JXL unavailable");
  CHECK(tinydng_write_memory(ctx,&img,&opts,&b,&n,&err)==TINYDNG_E_UNSUPPORTED,"disabled encoder reports unsupported");
#endif
  CHECK(tinydng_context_memory_used(ctx)==0,"JXL allocations released");
  tinydng_context_destroy(ctx); free(pixels); return g_fail ? 1 : 0;
}
