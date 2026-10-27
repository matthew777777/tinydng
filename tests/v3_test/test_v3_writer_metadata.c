/* Verify writer metadata and directory layout using independent TIFF reads. */
#include "td_test_util.h"

static uint16_t u16(const uint8_t *p, int be) {
  return be ? (uint16_t)(p[0]*256u+p[1]) : (uint16_t)(p[1]*256u+p[0]);
}
static uint32_t u32(const uint8_t *p, int be) {
  return be ? ((uint32_t)u16(p,1)<<16)|u16(p+2,1)
            : ((uint32_t)u16(p+2,0)<<16)|u16(p,0);
}
static const uint8_t *entry(const uint8_t *b, size_t n, uint32_t off,
                            uint16_t tag, int be) {
  if (off > n || n-off < 2) return NULL;
  unsigned count=u16(b+off,be);
  if ((size_t)count*12+6 > n-off) return NULL;
  unsigned last=0;
  for (unsigned i=0;i<count;i++) {
    const uint8_t *e=b+off+2+i*12;
    CHECK(i==0 || u16(e,be)>last,"IFD sorted without duplicates");
    last=u16(e,be);
    if (last==tag) return e;
  }
  return NULL;
}
static const uint8_t *value(const uint8_t *b,size_t n,const uint8_t *e,
                             size_t bytes,int be) {
  if (!e) return NULL;
  if (bytes<=4) return e+8;
  uint32_t off=u32(e+8,be);
  return off<=n && bytes<=n-off ? b+off : NULL;
}
static void bytes_equal(const uint8_t *b,size_t n,uint32_t ifd,uint16_t tag,
                        const void *expected,size_t bytes,int be) {
  const uint8_t *p=value(b,n,entry(b,n,ifd,tag,be),bytes,be);
  CHECK(p && !memcmp(p,expected,bytes),"tag %u bytes preserved",tag);
}
static void roundtrip(tinydng_context *ctx,const uint8_t *b,size_t n,
                       const void *pixels,size_t bytes) {
  tinydng_document *doc=NULL; tinydng_error err; tinydng_pixels out={0};
  CHECK(tinydng_open_memory(ctx,b,n,NULL,&doc,&err)==TINYDNG_OK,"open: %s",err.message);
  if (!doc) return;
  CHECK(tinydng_decode_image(ctx,doc,0,NULL,&out,&err)==TINYDNG_OK,"decode: %s",err.message);
  CHECK(out.data && !memcmp(out.data,pixels,bytes),"pixels preserved");
  tinydng_pixels_free(ctx,&out); tinydng_document_destroy(ctx,doc);
}

typedef struct { uint8_t data[16384]; size_t n; } sequential;
static size_t seq_write(tinydng_write_io *io,uint64_t at,const void *p,size_t n) {
  sequential *s=(sequential*)io->backend;
  if (at!=s->n || n>sizeof(s->data)-s->n) return 0;
  memcpy(s->data+s->n,p,n); s->n+=n; return n;
}
static uint64_t seq_size(tinydng_write_io *io) { return ((sequential*)io->backend)->n; }

static void metadata(int be,int head) {
  tinydng_context *ctx=tinydng_context_create(NULL,NULL);
  tinydng_error err; uint16_t pixels[15];
  for (unsigned i=0;i<15;i++) pixels[i]=(uint16_t)(i*1234);
  uint8_t active[16], noise[48], exposure[8], lat[24], iso[2];
  uint8_t op[100]={0}, opaque[20]={0};
  for(unsigned i=0;i<4;i++) tdt_pu32(active+i*4,i,be);
  for(unsigned i=0;i<6;i++) { double d=0.01*(i+1); uint64_t v; memcpy(&v,&d,8); tdt_pu64(noise+i*8,v,be); }
  tdt_pu32(exposure,1,be); tdt_pu32(exposure+4,100,be); tdt_pu16(iso,200,be);
  for(unsigned i=0;i<6;i++) tdt_pu32(lat+i*4,i+1,be);
  tdt_pu32(op,1,1); tdt_pu32(op+4,9,1); tdt_pu32(op+8,0x01030000,1);
  tdt_pu32(op+12,1,1); tdt_pu32(op+16,80,1);
  uint32_t rect[10]={0,0,3,5,0,1,2,2,1,1};
  for(unsigned i=0;i<10;i++) tdt_pu32(op+20+i*4,rect[i],1);
  tdt_pf64be(op+60,1); tdt_pf64be(op+68,1); tdt_pu32(op+92,1,1); tdt_pf32be(op+96,2);
  tdt_pu32(opaque,1,1); tdt_pu32(opaque+4,65000,1); tdt_pu32(opaque+8,0x01070000,1); tdt_pu32(opaque+12,1,1);
  tinydng_field fields[]={
    {50829,4,4,active,sizeof(active)}, {51041,12,6,noise,sizeof(noise)},
    {51008,7,sizeof(opaque),opaque,sizeof(opaque)},
    {51009,7,sizeof(op),op,sizeof(op)}, {51022,7,sizeof(opaque),opaque,sizeof(opaque)}};
  tinydng_field exif[]={{34855,3,1,iso,2},{33434,5,1,exposure,8}};
  uint8_t ref[2]={'N',0};
  tinydng_field gps[]={{2,5,3,lat,24},{1,2,2,ref,2}};
  tinydng_write_image img={0}; img.width=5; img.height=3; img.bits_per_sample=16;
  img.data=(uint8_t*)pixels; img.data_size=sizeof(pixels);
  img.fields=fields; img.field_count=5; img.exif_fields=exif; img.exif_field_count=2;
  img.gps_fields=gps; img.gps_field_count=2;
  tinydng_write_options opts={0}; opts.big_endian=(uint8_t)be; opts.header_first=(uint8_t)head;
  uint8_t *b=NULL; size_t n=0; sequential seq={0};
  if (head) {
    tinydng_write_io io={0}; io.write=seq_write; io.size=seq_size; io.backend=&seq;
    tinydng_tiling tiling={0}; tiling.rows_per_strip=1; tinydng_writer *w=NULL;
    CHECK(tinydng_writer_create(ctx,io,&img,&opts,&tiling,&w,&err)==TINYDNG_OK,"create: %s",err.message);
    if(w) {
      /* Reject out-of-order writes without advancing the writer. */
      CHECK(tinydng_writer_write_strip(w,1,pixels,&err)==TINYDNG_E_INVALID_ARG,"strip order");
      for(unsigned i=0;i<3;i++) CHECK(tinydng_writer_write_strip(w,i,pixels+i*5,&err)==TINYDNG_OK,"strip");
      CHECK(tinydng_writer_finish(w,&err)==TINYDNG_OK,"finish");
    }
    b=seq.data; n=seq.n;
  } else CHECK(tinydng_write_memory(ctx,&img,&opts,&b,&n,&err)==TINYDNG_OK,"write: %s",err.message);
  if (b && n>=8) {
    uint32_t ifd=u32(b+4,be);
    for(unsigned i=0;i<5;i++) bytes_equal(b,n,ifd,fields[i].tag,fields[i].data,fields[i].size,be);
    const uint8_t *e=entry(b,n,ifd,34665,be), *g=entry(b,n,ifd,34853,be);
    CHECK(e && g,"EXIF and GPS pointers");
    if(e) bytes_equal(b,n,u32(e+8,be),33434,exposure,8,be);
    if(g) { bytes_equal(b,n,u32(g+8,be),2,lat,24,be); bytes_equal(b,n,u32(g+8,be),1,ref,2,be); }
    roundtrip(ctx,b,n,pixels,sizeof(pixels));
    tinydng_document *doc=NULL;
    if(tinydng_open_memory(ctx,b,n,NULL,&doc,&err)==TINYDNG_OK) {
      const tinydng_image_info *im=tinydng_image_get(doc,0);
      CHECK(im && im->raw.gainmap_count==1 && im->raw.noise_profile_count==6,"gainmap and noise parsed");
      tinydng_document_destroy(ctx,doc);
    }
  }
  if(!head) tinydng_buffer_free(ctx,b);
  CHECK(tinydng_context_memory_used(ctx)==0,"metadata allocations released");
  /* Failed validation after one subdirectory was copied must release it. */
  gps[1].tag=2; b=NULL; n=0;
  CHECK(tinydng_write_memory(ctx,&img,&opts,&b,&n,&err)!=TINYDNG_OK,"duplicate GPS rejected");
  CHECK(tinydng_context_memory_used(ctx)==0,"failed create allocations released");
  fields[0].tag=256;
  CHECK(tinydng_write_memory(ctx,&img,&opts,&b,&n,&err)!=TINYDNG_OK,"structural duplicate rejected");
  CHECK(tinydng_context_memory_used(ctx)==0,"failed metadata allocations released");
  tinydng_context_destroy(ctx);
}
static void compressed(void) {
  tinydng_context *ctx=tinydng_context_create(NULL,NULL); tinydng_error err;
  /* Opaque bytes exercise copy-through; encoding/decoding is caller-owned. */
  uint8_t stream[]={0xff,0x0a,1,2,3,4,5};
  tinydng_write_image img={0}; img.width=32; img.height=24; img.bits_per_sample=16;
  img.data=stream; img.data_size=sizeof(stream);
  tinydng_write_options opts={0}; opts.compression=TINYDNG_COMPRESSION_JPEG_XL; opts.precompressed=1;
  for(int be=0;be<2;be++) {
    uint8_t *b=NULL; size_t n=0; opts.big_endian=(uint8_t)be;
    CHECK(tinydng_write_memory(ctx,&img,&opts,&b,&n,&err)==TINYDNG_OK,"compressed write: %s",err.message);
    if(b) {
      uint32_t ifd=u32(b+4,be); const uint8_t ver[]={1,7,0,0};
      bytes_equal(b,n,ifd,50706,ver,4,be); bytes_equal(b,n,ifd,50707,ver,4,be);
      const uint8_t *e=entry(b,n,ifd,273,be), *c=entry(b,n,ifd,279,be);
      CHECK(e && c && u32(c+8,be)==sizeof(stream),"codestream size");
      if(e) CHECK(!memcmp(b+u32(e+8,be),stream,sizeof(stream)),"codestream preserved verbatim");
      tinydng_buffer_free(ctx,b);
    }
  }
  uint8_t old_version[4]={1,4,0,0};
  tinydng_field old={50707,1,4,old_version,4};
  img.fields=&old; img.field_count=1;
  uint8_t *bad=NULL; size_t bad_size=0;
  CHECK(tinydng_write_memory(ctx,&img,&opts,&bad,&bad_size,&err)!=TINYDNG_OK,"old JXL backward version rejected");
  img.fields=NULL; img.field_count=0; opts.precompressed=0;
  CHECK(tinydng_write_memory(ctx,&img,&opts,&bad,&bad_size,&err)!=TINYDNG_OK,"JXL needs precompressed input");
  opts.precompressed=1; opts.header_first=1;
  CHECK(tinydng_write_memory(ctx,&img,&opts,&bad,&bad_size,&err)!=TINYDNG_OK,"JXL header-first rejected");
  CHECK(tinydng_context_memory_used(ctx)==0,"compressed allocations released");
  tinydng_context_destroy(ctx);
}
typedef struct { size_t remaining; } failing_allocator;
static void *fail_alloc(void *user,size_t size) {
  failing_allocator *a=(failing_allocator*)user;
  if(!a->remaining) return NULL;
  a->remaining--; return malloc(size);
}
static void fail_free(void *user,void *p) { (void)user; free(p); }
static void allocation_failures(void) {
  uint8_t pixels[4]={1,2,3,4}, text[]="metadata payload";
  tinydng_field field={270,2,sizeof(text),text,sizeof(text)};
  tinydng_field exif={36867,2,sizeof(text),text,sizeof(text)};
  tinydng_field gps={29,2,sizeof(text),text,sizeof(text)};
  tinydng_write_image img={0}; img.width=2; img.height=2; img.bits_per_sample=8;
  img.data=pixels; img.data_size=4; img.fields=&field; img.field_count=1;
  img.exif_fields=&exif; img.exif_field_count=1; img.gps_fields=&gps; img.gps_field_count=1;
  for(int head=0;head<2;head++) {
    int completed=0;
    for(size_t budget=0;budget<64;budget++) {
      failing_allocator a={100}; tinydng_config cfg={0};
      cfg.allocator.user_data=&a; cfg.allocator.alloc=fail_alloc; cfg.allocator.free=fail_free;
      tinydng_context *ctx=tinydng_context_create(&cfg,NULL);
      CHECK(ctx!=NULL,"create OOM test context"); if(!ctx) break;
      a.remaining=budget;
      tinydng_write_options opts={0}; opts.header_first=(uint8_t)head;
      uint8_t *data=NULL; size_t size=0; tinydng_error err;
      tinydng_status status=tinydng_write_memory(ctx,&img,&opts,&data,&size,&err);
      if(status==TINYDNG_OK) { completed=1; tinydng_buffer_free(ctx,data); }
      CHECK(tinydng_context_memory_used(ctx)==0,"OOM cleanup at allocation %zu head=%d",budget,head);
      tinydng_context_destroy(ctx);
      if(completed) break;
    }
    CHECK(completed,"allocation sweep reaches successful write");
  }
}
int main(void) {
  for(int be=0;be<2;be++) for(int head=0;head<2;head++) metadata(be,head);
  compressed(); allocation_failures(); return g_fail ? 1 : 0;
}
