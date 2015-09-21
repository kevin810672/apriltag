#include "box.h"
#include "image_u32.h"
#include <alloca.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
#include <pthread.h>

#ifdef NDEBUG
#undef NDEBUG
#endif

#include <assert.h>

image_u8_t* image_u8_aligned64(int width, int height) {

  int stride = width;

  if (stride & 0x3f) {
    stride += 64 - (stride & 0x3f);
  }

  assert(stride % 64 == 0);

  void* vptr = 0;

  size_t size = height * stride * sizeof(uint8_t);
  int status = posix_memalign(&vptr, 64, size);
  
  if (status != 0 || !vptr) {
    return NULL;
  }

  image_u8_t tmp = {
    .width = width,
    .height = height,
    .stride = stride,
    .buf = vptr
  };

  image_u8_t* img = (image_u8_t*)malloc(sizeof(image_u8_t));

  *img = tmp;

  return img;
  
}

image_u32_t* image_u32_aligned64(int width, int height) {

  int stride = width;

  if (stride & 0xf) {
    stride += 16 - (stride & 0xf);
  }

  assert((stride * sizeof(uint32_t)) % 64 == 0);
  
  void* vptr = 0;

  size_t size = height * stride * sizeof(uint32_t);
  int status = posix_memalign(&vptr, 64, size);
  
  if (status != 0 || !vptr) {
    return NULL;
  }

  image_u32_t* img = (image_u32_t*)malloc(sizeof(image_u32_t));
  
  img->width = width;
  img->height = height;
  img->stride = stride;
  img->buf = vptr;

  return img;
  
}

enum {
  INTEGRATE_BLOCK_SIZE = 64,
  INTEGRATE_ALLOW_CENTRAL = 0,
  INTEGRATE_MIN_BLOCKS_PER_THREAD = 10,
  
};

static inline void integrate_block_center(const uint8_t* src, int sstride,
                                          uint32_t* dst, int dstride) {

  for (int y=0; y<INTEGRATE_BLOCK_SIZE; ++y) {
    const uint8_t* s = src;
    uint32_t* d = dst;
    for (int x=0; x<INTEGRATE_BLOCK_SIZE; ++x) {
      d[dstride+1] = *s++ + d[dstride] + d[1] - d[0];
      ++d;
    }
    src += sstride;
    dst += dstride;
  }

}

static inline void integrate_block(const uint8_t* src, int sstep, int sstride,
                                   uint32_t* dst, int dstride,
                                   int nx, int ny) {


  for (int y=0; y<ny; ++y) {
    const uint8_t* s = src;
    uint32_t* d = dst;
    for (int x=0; x<nx; ++x) {
      d[dstride+1] = *s + d[dstride] + d[1] - d[0];
      ++d;
      s += sstep;
    }
    src += sstride;
    dst += dstride;
  }

}
                                  
                                  
                                  

image_u32_t* integrate_border_replicate(const image_u8_t* img, int l) {

  return integrate_border_replicate_mt(img, l, NULL);

  image_u32_t* iimg = image_u32_aligned64(img->width + 2*l + 1,
                                          img->height + 2*l + 1);

  // zero out first line
  memset(iimg->buf, 0, sizeof(uint32_t)*iimg->width);
  uint32_t* dl = iimg->buf + iimg->stride;
  
  // zero out first row of each line
  for (int y=1; y<iimg->height; ++y) {
    *dl = 0 ;
    dl += iimg->stride;
  }
  
  const uint8_t* src = img->buf;

  uint32_t* dst = iimg->buf;
  int ds = iimg->stride;
  int ss = img->stride;

  int W = img->width, w = W-1, H = img->height, h = H-1;
    
  //////////////////////////////////////////////////////////////////////
  // TOP PADDING

  integrate_block(src, 0, 0, dst, ds, l, l);

  dst += l;

  integrate_block(src, 1, 0, dst, ds, W, l);

  dst += W;

  integrate_block(src + w, 0, 0, dst, ds, l, l);

  //////////////////////////////////////////////////////////////////////
  // MIDDLE ROWS

  dst = iimg->buf + ds*l;

  integrate_block(src, 0, ss, dst, ds,l, H);

  dst += l;

  integrate_block(src, 1, ss, dst, ds, W, H);

  dst += W;

  integrate_block(src + w, 0, ss, dst, ds, l, H);
  
  //////////////////////////////////////////////////////////////////////
  // BOTTOM PADDING

  dst = iimg->buf + ds*(l + H);

  integrate_block(src + h*ss, 0, 0, dst, ds, l, l);

  dst += l;

  integrate_block(src + h*ss, 1, 0, dst, ds, W, l);

  dst += W;

  integrate_block(src + h*ss + w, 0, 0, dst, ds, l, l);
  
  return iimg;

}


static inline void box_filter_rows(uint8_t* dst_row, int dst_stride, 
                                   const uint32_t* sum_row, int sum_stride,
                                   int nx, int ny, int sz) {

  int s2 = sz*sz;
  int s22 = s2/2; // for rounding?
  
  int ob = sum_stride*sz;
  int od = ob + sz;

  for (int y=0; y<ny; ++y) {
    uint8_t* dst = dst_row;
    const uint32_t* sum = sum_row;
    for (int x=0; x<nx; ++x) {
      *dst++ = (sum[od] - sum[ob] - sum[sz] + sum[0] + s22)/s2;
      ++sum;
    }
    dst_row += dst_stride;
    sum_row += sum_stride;
  }


}

image_u8_t* box_filter_border_replicate(const image_u8_t* src_img, 
                                        int sz) {

  image_u8_t* dst_img = image_u8_aligned64(src_img->width, src_img->height);

  int l = sz/2;
  sz = 2*l+1;

  image_u32_t* sum_img = integrate_border_replicate(src_img, l);

  box_filter_rows(dst_img->buf, dst_img->stride,
                  sum_img->buf, sum_img->stride,
                  src_img->width, src_img->height, sz);

  image_u32_destroy(sum_img);

  return dst_img;

}

typedef struct box_filter_info {
  uint8_t* dst;
  const uint32_t* sum;
  int dst_stride;
  int sum_stride;
  int nx;
  int ny;
  int sz;
} box_filter_info_t;


static void box_filter_task(void* p) {

  box_filter_info_t* info = (box_filter_info_t*)p;

  box_filter_rows(info->dst, info->dst_stride,
                  info->sum, info->sum_stride,
                  info->nx, info->ny, info->sz);

}


image_u8_t* box_filter_border_replicate_mt(const image_u8_t* src_img, 
                                           int sz, workerpool_t* wp) {

  image_u8_t* dst_img = image_u8_aligned64(src_img->width, src_img->height);

  int l = sz/2;
  sz = 2*l+1;
  
  image_u32_t* sum_img = integrate_border_replicate_mt(src_img, l, wp);

  int nt = wp ? workerpool_get_nthreads(wp) : 1;

  if (wp == NULL || nt <= 1) {

    box_filter_rows(dst_img->buf, dst_img->stride,
                    sum_img->buf, sum_img->stride,
                    src_img->width, src_img->height, sz);

  } else {

    int rows_per_block = src_img->height / nt;
    if (src_img->height % nt) { ++rows_per_block; }

    box_filter_info_t bfs[nt];

    int y0 = 0;
    uint8_t* dst_row = dst_img->buf;
    const uint32_t* sum_row = sum_img->buf;

    for (int i=0; i<nt; ++i) {
      int y1 = y0 + rows_per_block;
      if (y1 > src_img->height) { y1 = src_img->height; }
      bfs[i].dst = dst_row;
      bfs[i].sum = sum_row;
      bfs[i].dst_stride = dst_img->stride;
      bfs[i].sum_stride = sum_img->stride;
      bfs[i].nx = dst_img->width;
      bfs[i].ny = y1 - y0;
      bfs[i].sz = sz;
      dst_row += rows_per_block * dst_img->stride;
      sum_row += rows_per_block * sum_img->stride;
      workerpool_add_task(wp, box_filter_task, bfs+i);
    }

    workerpool_run(wp);

  }

  image_u32_destroy(sum_img);

  return dst_img;

}


image_u8_t* box_threshold(const image_u8_t* src_img, 
                          int max_value, 
                          int invert, 
                          int sz, 
                          int tau) {

  
  image_u8_t* dst_img = image_u8_aligned64(src_img->width,
                                           src_img->height);
                                           
  int gt = invert ? 0 : max_value;
  int lt = max_value - gt;
  
  int l = sz/2;
  sz = 2*l+1;

  int s2 = sz*sz;
  int s22 = s2/2; // for rounding?

  image_u32_t* tmp_img = integrate_border_replicate(src_img, l);

  uint8_t* dst = dst_img->buf;

  const uint32_t* tmp = tmp_img->buf;
  const uint8_t* src = src_img->buf;

  int ob = tmp_img->stride*sz;
  int od = ob + sz;

  int tmprem = tmp_img->stride - dst_img->width;
  int dstrem = dst_img->stride - dst_img->width;
  int srcrem = src_img->stride - dst_img->width;

  for (int y=0; y<dst_img->height; ++y) {
    for (int x=0; x<dst_img->width; ++x) {
      int t = (tmp[od] - tmp[ob] - tmp[sz] + *tmp + s22)/s2 - tau;
      int s = *src++;
      *dst++ = s > t ? gt : lt;
      ++tmp;
    }
    src += srcrem;
    dst += dstrem;
    tmp += tmprem;
  }

  image_u32_destroy(tmp_img);

  return dst_img;

}


typedef struct block_info {

  int bx; 
  int by;

  const uint8_t* src;
  
  int sstep;
  int sstride;
  
  uint32_t* dst;
  
  int nx;
  int ny;

  int is_central;
  int status; // 0 = not yet enqueued, 1 = pending, 2 = finished
  
} block_info_t;

typedef struct integrate_info {

  pthread_mutex_t mutex;
  pthread_cond_t  cond;

  int dstride;
  
  int num_blocks_x;
  int num_blocks_y;
  int total_blocks;

  block_info_t* blocks;
  
  int* queue;

  int queue_start;
  int queue_end;
  int finished;
  
} integrate_info_t;

typedef struct integrate_task_ {
  integrate_info_t* info;
  int id;
} integrate_task_t;


#define task_debug 0
#define debug_printf if (task_debug) printf
#define debug_fflush if (task_debug) fflush

void integrate_task(void* p) {

  integrate_task_t* task = (integrate_task_t*)p;

  integrate_info_t* info = task->info;
  int task_id = task->id;

  debug_printf("starting integrate_task for task %d\n", task_id);

  pthread_mutex_lock(&info->mutex);

  debug_printf("task %d has the lock\n", task_id);
  
  // invariant: we hold the lock at the top of the loop here
  while (!info->finished) { 
    
    debug_printf("task %d at top of loop\n", task_id);
    
    if (info->queue_start == info->queue_end) {

      debug_printf("task %d waiting because queue empty!\n", task_id);
      debug_fflush(stdout);
      
      // if the queue is empty, wait for work to be placed into it (or
      // for all work to be finished)
      pthread_cond_wait(&info->cond, &info->mutex);
      // note we regain the lock after this returns
 
      debug_printf("task %d woke up after signal!\n", task_id);
      
    } else {


      int idx = info->queue[info->queue_start];
      block_info_t* block = info->blocks + idx;
      assert(block->status == 1);
      ++info->queue_start;

      debug_printf("task %d popped off idx %d, queue size is %d. "
              "unlocking and integrating...\n",
              task_id, idx, info->queue_end - info->queue_start);
      
      pthread_mutex_unlock(&info->mutex);

      if (INTEGRATE_ALLOW_CENTRAL && block->is_central) {
        integrate_block_center(block->src, block->sstride,
                               block->dst, info->dstride);
      } else {
        integrate_block(block->src, block->sstep, block->sstride,
                        block->dst, info->dstride,
                        block->nx, block->ny);
      }

      debug_fflush(stdout);

      //sched_yield();

      pthread_mutex_lock(&info->mutex);

      debug_printf("task %d done integrating block %d and re-locked\n",
                   task_id, idx);
      
      block->status = 2;
      int do_broadcast = 0;

      if (idx != (info->total_blocks-1)) { // not all done

        if (block->bx + 1 < info->num_blocks_x) { // are blocks right ?
          int ridx = idx + 1;
          debug_printf("task %d checking to see whether should enqueue %d...\n", task_id, ridx);
          if (info->blocks[ridx].status == 0) {
            int aridx = ridx - info->num_blocks_x;
            if ( !block->by || info->blocks[aridx].status == 2 ) {
              info->queue[info->queue_end++] = ridx;
              info->blocks[ridx].status = 1;
              do_broadcast = 1;
              debug_printf("task %d enqueued block %d (will broadcast)\n",
                           task_id, ridx);
            } else {
              debug_printf("task %d found block %d not ready yet.\n", task_id, ridx);
            }
          } else {
            debug_printf("task %d found that block %d is already enqueued/done.\n", task_id, ridx);
          }
        }

        if (block->by + 1 < info->num_blocks_y) { // are blocks below?
          int bidx = idx + info->num_blocks_x;
          debug_printf("task %d checking to see whether should enqueue %d...\n", task_id, bidx);
          if (info->blocks[bidx].status == 0) {
            int blidx = bidx - 1;
            if ( !block->bx || info->blocks[blidx].status == 2 ) {
              info->queue[info->queue_end++] = bidx;
              info->blocks[bidx].status = 1;
              do_broadcast = 1; 
              debug_printf("task %d enqueued block %d (will broadcast)\n",
                           task_id, bidx);
            } else {
              debug_printf("task %d found block %d not ready yet.\n", task_id, bidx);
            }
          } else {
            debug_printf("task %d found that block %d is already enqueued/done.\n", task_id, bidx);
          }
        }

      } else { // all done

        do_broadcast = 1;
        info->finished = 1;
 
        debug_printf("task %d is the finisher! (will broadcast)\n",
                     task_id);
       
      }

      if (1 || do_broadcast) {
        pthread_cond_broadcast(&info->cond);
      }
      
    }
    
  }

  pthread_mutex_unlock(&info->mutex);
  debug_printf("task %d all done!\n", task_id);
  debug_fflush(stdout);
  
}

image_u32_t* integrate_border_replicate_mt(const image_u8_t* img, int l,
                                           workerpool_t* wp) {

  image_u32_t* iimg = image_u32_aligned64(img->width + 2*l + 1,
                                          img->height + 2*l + 1);

  // zero out first line
  memset(iimg->buf, 0, sizeof(uint32_t)*iimg->width);
  uint32_t* dl = iimg->buf + iimg->stride;
  
  // zero out first row of each line
  for (int y=1; y<iimg->height; ++y) {
    *dl = 0 ;
    dl += iimg->stride;
  }

  //memset(iimg->buf, 0, sizeof(uint32_t)*iimg->height*iimg->stride);

  integrate_info_t info;
  pthread_mutex_init(&info.mutex, NULL);
  pthread_cond_init(&info.cond, NULL);
  info.dstride = iimg->stride;

  int nbl = l ? (l / INTEGRATE_BLOCK_SIZE) + (l % INTEGRATE_BLOCK_SIZE ? 1 : 0) : 0;
  int nbx = (img->width / INTEGRATE_BLOCK_SIZE) + (img->width % INTEGRATE_BLOCK_SIZE ? 1 : 0);
  int nby = (img->height / INTEGRATE_BLOCK_SIZE) + (img->height % INTEGRATE_BLOCK_SIZE ? 1 : 0);

  info.num_blocks_x = nbx + 2*nbl;
  info.num_blocks_y = nby + 2*nbl;
  info.total_blocks = info.num_blocks_x * info.num_blocks_y;

  info.blocks = malloc(sizeof(block_info_t)*info.total_blocks);
  info.queue = malloc(sizeof(int)*info.total_blocks);

  block_info_t* cur_block = info.blocks;

  int r1 = nbl, r2 = nbl + nby;
  int c1 = nbl, c2 = nbl + nbx;

  for (int by=0; by<info.num_blocks_y; ++by) {
    for (int bx=0; bx<info.num_blocks_x; ++bx) {

      cur_block->bx = bx;
      cur_block->by = by;
      
      cur_block->src = img->buf;

      int xstart, xend, rx;

      if (bx < c1) { // LEFT
        cur_block->sstep = 0;
        xstart = 0;
        xend = l;
        rx = bx;
      } else if (bx < c2) { // MIDDLE
        cur_block->sstep = 1;
        xstart = l;
        xend = img->width + l;
        rx = bx-c1;
      } else { // RIGHT
        cur_block->src += img->width-1;
        cur_block->sstep = 0;
        xstart = img->width + l;
        xend = xstart + l;
        rx = bx-c2;
      }

      int ystart, yend, ry;

      if (by < r1) { // TOP
        cur_block->sstride = 0;
        ystart = 0;
        yend = l;
        ry = by;
      } else if (by < r2) { // MIDDLE
        cur_block->sstride = img->stride;
        ystart = l;
        yend = img->height + l;
        ry = by-r1;
      } else { // BOTTOM
        cur_block->src += (img->height-1)*img->stride;
        cur_block->sstride = 0;
        ystart = img->height + l;
        yend = ystart + l;
        ry = by-r2;
      }

      int x0 = xstart + rx * INTEGRATE_BLOCK_SIZE;
      int y0 = ystart + ry * INTEGRATE_BLOCK_SIZE;

      int x1 = x0 + INTEGRATE_BLOCK_SIZE;
      int y1 = y0 + INTEGRATE_BLOCK_SIZE;

      if (x1 > xend) { x1 = xend; }
      if (y1 > yend) { y1 = yend; }

      cur_block->src +=  (ry * cur_block->sstride + 
                          rx * cur_block->sstep) * INTEGRATE_BLOCK_SIZE;
      
      cur_block->dst = iimg->buf + x0 + y0*iimg->stride;

      cur_block->nx = x1-x0;
      cur_block->ny = y1-y0;

      cur_block->status = 0;

      cur_block->is_central =
        (INTEGRATE_ALLOW_CENTRAL &&
         cur_block->sstride && cur_block->sstep &&
         cur_block->nx == INTEGRATE_BLOCK_SIZE &&
         cur_block->ny == INTEGRATE_BLOCK_SIZE);

      if (0) {

        int sy = (cur_block->src - img->buf) / img->stride;
        int sx = (cur_block->src - img->buf) % img->stride;

        int dy = (cur_block->dst - iimg->buf) / iimg->stride;
        int dx = (cur_block->dst - iimg->buf) % iimg->stride;

        printf("adding block #%d at (%d, %d)\n", (int)(cur_block-info.blocks),
               cur_block->bx, cur_block->by);
        printf("  src is %p, offset into img at (%d, %d)\n",
               cur_block->src, sx, sy);
        printf("  sstep=%d\n", cur_block->sstep);
        printf("  sstride=%d\n", cur_block->sstride);
        printf("  dst is %p, offset into iimg at (%d, %d)\n",
               cur_block->dst, dx, dy);
        printf("  block size is %dx%d\n", cur_block->nx, cur_block->ny);
        printf("  is_central=%d\n", cur_block->is_central);
        printf("\n");
        
      }
      
        
      ++cur_block;
      
    }

  }
  
  info.queue[0] = 0;
  info.queue_start = 0;
  info.queue_end = 1;
  info.blocks[0].status = 1;
  info.finished = 0;

  memset(iimg->buf, 0, 4*iimg->stride*iimg->height);

  int nt = wp ? workerpool_get_nthreads(wp) : 1;

  int blocks_per_thread = info.total_blocks / nt;

  if (wp == NULL || nt <= 1 ||
      blocks_per_thread < INTEGRATE_MIN_BLOCKS_PER_THREAD) {

    for (int i=0; i<info.total_blocks; ++i) {

      cur_block = info.blocks + i;

      if (cur_block->bx) {
        assert(info.blocks[i-1].status == 2);
      }
      if (cur_block->by) {
        assert(info.blocks[i-info.num_blocks_x].status == 2);
      }

      if (INTEGRATE_ALLOW_CENTRAL && cur_block->is_central) {
        integrate_block_center(cur_block->src, cur_block->sstride,
                               cur_block->dst, info.dstride);
      } else {
        integrate_block(cur_block->src, cur_block->sstep, cur_block->sstride,
                        cur_block->dst, info.dstride, cur_block->nx, cur_block->ny);
      }

      cur_block->status = 2;
    
    }

  } else {


    //printf("adding tasks...\n");

    integrate_task_t tasks[nt];

    for (int i=0; i<nt; ++i) {
      integrate_task_t* task = tasks + i;
      task->id = i;
      task->info = &info;
      workerpool_add_task(wp, integrate_task, task);
    }
    
    //printf("added!\n");
    //printf("running tasks...\n");

    workerpool_run(wp);
    //workerpool_run_single(wp);

    //printf("done!\n");
    
  }

  return iimg;

}
