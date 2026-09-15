"""Compile the real helpers and compare every authored byte and submission fence."""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]

class BoundedCoordinateTests(unittest.TestCase):
    def test_cached_commands_match_uncached_including_tails_and_batches(self):
        source = (ROOT / 'drivers/video/fbdev/gcn-gx.c').read_text()
        def function(name):
            match = re.search(r'^static (?:inline )?\w+ '+name+r'\(', source, re.M)
            self.assertIsNotNone(match)
            start = source.index('{', match.start())
            depth = 1
            end = start+1
            while depth:
                depth += (source[end]=='{') - (source[end]=='}')
                end += 1
            return source[match.start():end]
        preamble = r'''
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64; typedef int32_t s32;
#define min_t(t,a,b) ((t)(a)<(t)(b)?(t)(a):(t)(b))
#define DIV_ROUND_UP(a,b) (((a)+(b)-1)/(b))
#define BUILD_BUG_ON(x) _Static_assert(!(x), "build bug")
#define F32_ZERO 0U
#define F32_NEG(b) ((b)^0x80000000U)
#define do_div(n,d) ((n)/=(d))
/* Consume arguments to retain compiler checks without emitting diagnostic text. */
#define pr_info(...) ((void)snprintf(logbuf,sizeof(logbuf),__VA_ARGS__))
#define GX_FIFO_SIZE 65536
static char logbuf[512];
static bool gx_scale_bounded_coord_cache;
static bool gx_scale_bounded_log=true;
static u32 gx_scale_bounded_batch_quads=600, fifo_pos, used;
static unsigned char stream[2000000], reference[2000000];
static void gx_wr8(u32 v) { if(used>=sizeof(stream)) abort(); stream[used++]=v; fifo_pos++; }
static void gx_wr16be(u32 v) { gx_wr8(v>>8); gx_wr8(v); }
static void gx_wr32be(u32 v) { gx_wr16be(v>>16); gx_wr16be(v); }
static void wg_f32_bits(u32 v) { gx_wr32be(v); }
static void gx_load_bp_reg(u32 v) { gx_wr8(0x61); gx_wr32be(v); }
static int gx_submit_and_wait_finish(const char *phase) {
 if(fifo_pos>GX_FIFO_SIZE-256) abort();
 /* Include boundaries and phase identity in the comparison stream. */
 gx_wr32be(fifo_pos); while(*phase) gx_wr8(*phase++); gx_wr8(0);
 return 0;
}
'''
        functions = '\n'.join(function(name) for name in (
            'f32_from_u16','f32_div_u32','gx_semantic_texcoord_bits_phase',
            'gx_nearest_source_index','gx_nearest_run_count','gx_emit_textured_rect',
            'gx_draw_bounded_vertical_runs','gx_draw_bounded_horizontal_runs'))
        main = r'''
static unsigned state=1;
static unsigned next(void) { state=state*1664525U+1013904223U; return state; }
int main(void) {
 for(unsigned n=0;n<512;n++) {
  u16 width=1+next()%640, sh=1+next()%528, dh=1+next()%528;
  u16 sw=1+next()%640, dw=1+next()%640;
  if(n==0) { width=640; sh=240; dh=480; sw=320; dw=640; }
  if(n==1) { width=256; sh=255; dh=127; sw=255; dw=256; }
  if(n==2) { width=1; sh=1; dh=1; sw=1; dw=1; }
  if(n==3) { width=640; sh=528; dh=528; sw=640; dw=640; }
  gx_scale_bounded_batch_quads=n%2?400:600;
  for(unsigned direction=0;direction<2;direction++) {
   unsigned length=0; int previous=0;
   for(unsigned cache=0;cache<4;cache++) {
    gx_scale_bounded_coord_cache=cache&1; gx_scale_bounded_log=!(cache&2); used=0; fifo_pos=0;
    for(unsigned i=0;i<966;i++) gx_wr8(i);
    int ret=direction ? gx_draw_bounded_horizontal_runs(sw,1024,sh,1024,dw) :
     gx_draw_bounded_vertical_runs(3,7,width,1024,sh,1024,dh);
    gx_load_bp_reg(0x45000002); gx_submit_and_wait_finish("caller-last");
    if(!cache) { length=used; previous=ret; memcpy(reference,stream,used); }
    else if(ret!=previous || length!=used || memcmp(reference,stream,used)) return 1;
   }
  }
 }
 puts("1024 geometry/direction pairs match, including all bytes and fences");
 return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)
            (path/'test.c').write_text(preamble+functions+main)
            subprocess.run(['cc','-O2','-Wall','-Wextra','-Werror',str(path/'test.c'),'-o',str(path/'test')],check=True)
            result=subprocess.run([str(path/'test')],check=True,capture_output=True,text=True)
            self.assertIn('1024 geometry/direction pairs match',result.stdout)
