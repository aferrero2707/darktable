/*
    This file is part of darktable,
    copyright (c) 2009--2010 johannes hanika.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/
#include "develop/imageop.h"
#include "common/opencl.h"
#include "bauhaus/bauhaus.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "common/darktable.h"
#include "common/interpolation.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/tiling.h"
#include <memory.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>

// we assume people have -msee support.
#include <xmmintrin.h>

#define BLOCKSIZE  2048		/* maximum blocksize. must be a power of 2 and will be automatically reduced if needed */

DT_MODULE_INTROSPECTION(3, dt_iop_demosaic_params_t)

typedef struct dt_iop_demosaic_params_t
{
  uint32_t green_eq;
  float median_thrs;
  uint32_t color_smoothing;
  uint32_t demosaicing_method;
  uint32_t yet_unused_data_specific_to_demosaicing_method;
}
dt_iop_demosaic_params_t;

typedef struct dt_iop_demosaic_gui_data_t
{
  GtkWidget *scale1;
  GtkWidget *greeneq;
  GtkWidget *color_smoothing;
  GtkWidget *demosaic_method_bayer;
  GtkWidget *demosaic_method_xtrans;
}
dt_iop_demosaic_gui_data_t;

typedef struct dt_iop_demosaic_global_data_t
{
  // demosaic pattern
  int kernel_green_eq;
  int kernel_pre_median;
  int kernel_ppg_green;
  int kernel_ppg_green_median;
  int kernel_ppg_redblue;
  int kernel_zoom_half_size;
  int kernel_downsample;
  int kernel_border_interpolate;
  int kernel_color_smoothing;
}
dt_iop_demosaic_global_data_t;

typedef struct dt_iop_demosaic_data_t
{
  // demosaic pattern
  uint32_t filters;
  uint32_t green_eq;
  uint32_t color_smoothing;
  uint32_t demosaicing_method;
  uint32_t yet_unused_data_specific_to_demosaicing_method;
  float median_thrs;
}
dt_iop_demosaic_data_t;

#define DEMOSAIC_XTRANS 1024    // maks for non-Bayer demosaic ops

typedef enum dt_iop_demosaic_method_t
{
  // methods for Bayer images
  DT_IOP_DEMOSAIC_PPG = 0,
  DT_IOP_DEMOSAIC_AMAZE = 1,
  // methods for x-trans images
  DT_IOP_DEMOSAIC_LINEAR = DEMOSAIC_XTRANS | 0,
  DT_IOP_DEMOSAIC_VNG = DEMOSAIC_XTRANS | 1,
  DT_IOP_DEMOSAIC_MARKESTEIJN = DEMOSAIC_XTRANS | 2,
  DT_IOP_DEMOSAIC_MARKESTEIJN_3 = DEMOSAIC_XTRANS | 3
}
dt_iop_demosaic_method_t;

typedef enum dt_iop_demosaic_greeneq_t
{
  DT_IOP_GREEN_EQ_NO = 0,
  DT_IOP_GREEN_EQ_LOCAL = 1,
  DT_IOP_GREEN_EQ_FULL = 2,
  DT_IOP_GREEN_EQ_BOTH = 3
}
dt_iop_demosaic_greeneq_t;

static void
amaze_demosaic_RT(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const float *const in, float *out, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out, const int filters);

const char *
name()
{
  return _("demosaic");
}

int
groups ()
{
  return IOP_GROUP_BASIC;
}

int
flags ()
{
  return IOP_FLAGS_ALLOW_TILING | IOP_FLAGS_ONE_INSTANCE;
}

void init_key_accels(dt_iop_module_so_t *self)
{
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "edge threshold"));
}

void connect_key_accels(dt_iop_module_t *self)
{
  dt_accel_connect_slider_iop(self, "edge threshold",
                              GTK_WIDGET(((dt_iop_demosaic_gui_data_t*)
                                          self->gui_data)->scale1));
}

int
legacy_params (dt_iop_module_t *self, const void *const old_params, const int old_version, void *new_params, const int new_version)
{
  if(old_version == 2 && new_version == 3)
  {
    dt_iop_demosaic_params_t *o = (dt_iop_demosaic_params_t *)old_params;
    dt_iop_demosaic_params_t *n = (dt_iop_demosaic_params_t *)new_params;
    n->green_eq = o->green_eq;
    n->median_thrs = o->median_thrs;
    n->color_smoothing = 0;
    n->demosaicing_method = 0;
    n->yet_unused_data_specific_to_demosaicing_method = 0;
    return 0;
  }
  return 1;
}

static int
FC(const int row, const int col, const unsigned int filters)
{
  return filters >> (((row << 1 & 14) + (col & 1)) << 1) & 3;
}

#define SWAP(a, b) {const float tmp = (b); (b) = (a); (a) = tmp;}

static void
pre_median_b(float *out, const float *const in, const dt_iop_roi_t *const roi, const int filters, const int num_passes, const float threshold)
{
#if 1
  memcpy(out, in, (size_t)roi->width*roi->height*sizeof(float));
#else
  // colors:
  const float thrsc = 2*threshold;
  for (int pass=0; pass < num_passes; pass++)
  {
    for (int c=0; c < 3; c+=2)
    {
      int rows = 3;
      if(FC(rows,3,filters) != c && FC(rows,4,filters) != c) rows++;
#ifdef _OPENMP
      #pragma omp parallel for default(none) shared(rows,c,out) schedule(static)
#endif
      for (int row=rows; row<roi->height-3; row+=2)
      {
        float med[9];
        int col = 3;
        if(FC(row,col,filters) != c) col++;
        float *pixo = out + (size_t)roi->width * row + col;
        const float *pixi = in + (size_t)roi->width * row + col;
        for(; col<roi->width-3; col+=2)
        {
          int cnt = 0;
          for (int k=0, i = -2*roi->width; i <= 2*roi->width; i += 2*roi->width)
          {
            for (int j = i-2; j <= i+2; j+=2)
            {
              if(fabsf(pixi[j] - pixi[0]) < thrsc)
              {
                med[k++] = pixi[j];
                cnt ++;
              }
              else med[k++] = 64.0f + pixi[j];
            }
          }
          for (int i=0; i<8; i++) for(int ii=i+1; ii<9; ii++) if(med[i] > med[ii]) SWAP(med[i], med[ii]);
#if 0
          // cnt == 1 and no small edge in greens.
          if(fabsf(pixi[-roi->width] - pixi[+roi->width]) + fabsf(pixi[-1] - pixi[+1])
              + fabsf(pixi[-roi->width] - pixi[+1]) + fabsf(pixi[-1] - pixi[+roi->width])
              + fabsf(pixi[+roi->width] - pixi[+1]) + fabsf(pixi[-1] - pixi[-roi->width])
              > 0.06)
            pixo[0] = med[(cnt-1)/2];
          else
#endif
            pixo[0] = (cnt == 1 ? med[4] - 64.0f : med[(cnt-1)/2]);
          pixo += 2;
          pixi += 2;
        }
      }
    }
  }
#endif

  // now green:
  const int lim[5] = {0, 1, 2, 1, 0};
  for (int pass=0; pass < num_passes; pass++)
  {
#ifdef _OPENMP
    #pragma omp parallel for default(none) shared(out) schedule(static)
#endif
    for (int row=3; row<roi->height-3; row++)
    {
      float med[9];
      int col = 3;
      if(FC(row,col,filters) != 1 && FC(row,col,filters) != 3) col++;
      float *pixo = out + (size_t)roi->width * row + col;
      const float *pixi = in + (size_t)roi->width * row + col;
      for(; col<roi->width-3; col+=2)
      {
        int cnt = 0;
        for (int k=0, i = 0; i < 5; i ++)
        {
          for (int j = -lim[i]; j <= lim[i]; j+=2)
          {
            if(fabsf(pixi[roi->width*(i-2) + j] - pixi[0]) < threshold)
            {
              med[k++] = pixi[roi->width*(i-2) + j];
              cnt++;
            }
            else med[k++] = 64.0f+pixi[roi->width*(i-2)+j];
          }
        }
        for (int i=0; i<8; i++) for(int ii=i+1; ii<9; ii++) if(med[i] > med[ii]) SWAP(med[i], med[ii]);
        pixo[0] = (cnt == 1 ? med[4] - 64.0f : med[(cnt-1)/2]);
        // pixo[0] = med[(cnt-1)/2];
        pixo += 2;
        pixi += 2;
      }
    }
  }
}

static void
pre_median(float *out, const float *const in, const dt_iop_roi_t *const roi, const int filters, const int num_passes, const float threshold)
{
  pre_median_b(out, in, roi, filters, num_passes, threshold);
}

#define SWAPmed(I,J) if (med[I] > med[J]) SWAP(med[I], med[J])

static void
color_smoothing(float *out, const dt_iop_roi_t *const roi_out, const int num_passes)
{
  const int width4 = 4 * roi_out->width;

  for (int pass=0; pass < num_passes; pass++)
  {
    for (int c=0; c < 3; c+=2)
    {
      float *outp = out;
      for (int j=0; j<roi_out->height; j++) for(int i=0; i<roi_out->width; i++,outp+=4)
          outp[3] = outp[c];
#ifdef _OPENMP
      #pragma omp parallel for schedule(static) default(none) shared(out,c)
#endif
      for (int j=1; j<roi_out->height-1; j++)
      {
        float *outp = out + (size_t)4*j*roi_out->width + 4;
        for(int i=1; i<roi_out->width-1; i++,outp+=4)
        {
          float med[9] =
          {
            outp[-width4-4+3] - outp[-width4-4+1],
            outp[-width4+0+3] - outp[-width4+0+1],
            outp[-width4+4+3] - outp[-width4+4+1],
            outp[-4+3] - outp[-4+1],
            outp[+0+3] - outp[+0+1],
            outp[+4+3] - outp[+4+1],
            outp[+width4-4+3] - outp[+width4-4+1],
            outp[+width4+0+3] - outp[+width4+0+1],
            outp[+width4+4+3] - outp[+width4+4+1],
          };
          /* optimal 9-element median search */
          SWAPmed(1,2);
          SWAPmed(4,5);
          SWAPmed(7,8);
          SWAPmed(0,1);
          SWAPmed(3,4);
          SWAPmed(6,7);
          SWAPmed(1,2);
          SWAPmed(4,5);
          SWAPmed(7,8);
          SWAPmed(0,3);
          SWAPmed(5,8);
          SWAPmed(4,7);
          SWAPmed(3,6);
          SWAPmed(1,4);
          SWAPmed(2,5);
          SWAPmed(4,7);
          SWAPmed(4,2);
          SWAPmed(6,4);
          SWAPmed(4,2);
          outp[c] = fmaxf(med[4] + outp[1], 0.0f);
        }
      }
    }
  }
}
#undef SWAP

static void
green_equilibration_lavg(float *out, const float *const in, const int width, const int height, const uint32_t filters, const int x, const int y, const int in_place, const float thr)
{
  const float maximum = 1.0f;

  int oj = 2, oi = 2;
  if(FC(oj+y, oi+x, filters) != 1) oj++;
  if(FC(oj+y, oi+x, filters) != 1) oi++;
  if(FC(oj+y, oi+x, filters) != 1) oj--;

  if(!in_place)
    memcpy(out,in,height*width*sizeof(float));

#ifdef _OPENMP
  #pragma omp parallel for schedule(static) default(none) shared(out,oi,oj)
#endif
  for(size_t j=oj; j<height-2; j+=2)
  {
    for(size_t i=oi; i<width-2; i+=2)
    {
      const float o1_1 = in[(j-1)*width+i-1];
      const float o1_2 = in[(j-1)*width+i+1];
      const float o1_3 = in[(j+1)*width+i-1];
      const float o1_4 = in[(j+1)*width+i+1];
      const float o2_1 = in[(j-2)*width+i];
      const float o2_2 = in[(j+2)*width+i];
      const float o2_3 = in[j*width+i-2];
      const float o2_4 = in[j*width+i+2];

      const float m1 = (o1_1+o1_2+o1_3+o1_4)/4.0f;
      const float m2 = (o2_1+o2_2+o2_3+o2_4)/4.0f;

      // prevent divide by zero and ...
      // guard against m1/m2 becoming too large (due to m2 being too small) which results in hot pixels
      if (m2>0.0f && m1/m2<maximum*2.0f)
      {
        const float c1 = (fabsf(o1_1-o1_2)+fabsf(o1_1-o1_3)+fabsf(o1_1-o1_4)+fabsf(o1_2-o1_3)+fabsf(o1_3-o1_4)+fabsf(o1_2-o1_4))/6.0f;
        const float c2 = (fabsf(o2_1-o2_2)+fabsf(o2_1-o2_3)+fabsf(o2_1-o2_4)+fabsf(o2_2-o2_3)+fabsf(o2_3-o2_4)+fabsf(o2_2-o2_4))/6.0f;
        if((in[j*width+i]<maximum*0.95f)&&(c1<maximum*thr)&&(c2<maximum*thr))
        {
          out[j*width+i] = in[j*width+i]*m1/m2;
        }
      }
    }
  }
}

static void
green_equilibration_favg(float *out, const float *const in, const int width, const int height, const uint32_t filters, const int x, const int y)
{
  int oj = 0, oi = 0;
  //const float ratio_max = 1.1f;
  double sum1 = 0.0, sum2 = 0.0, gr_ratio;

  if( (FC(oj+y, oi+x, filters) & 1) != 1) oi++;
  int g2_offset = oi ? -1:1;
  memcpy(out,in,(size_t)height*width*sizeof(float));
#ifdef _OPENMP
  #pragma omp parallel for schedule(static) default(none) reduction(+: sum1, sum2) shared(oi, oj, g2_offset)
#endif
  for(size_t j=oj; j<(height-1); j+=2)
  {
    for(size_t i=oi; i<(width-1-g2_offset); i+=2)
    {
      sum1 += in[j*width+i];
      sum2 += in[(j+1) * width + i + g2_offset];
    }
  }

  if (sum1 > 0.0 && sum2 > 0.0)
    gr_ratio = sum1/sum2;
  else
    return;

#ifdef _OPENMP
  #pragma omp parallel for schedule(static) default(none) shared(out, oi, oj, gr_ratio, g2_offset)
#endif
  for(int j=oj; j<(height-1); j+=2)
  {
    for(int i=oi; i<(width-1-g2_offset); i+=2)
    {
      out[(size_t)j*width+i] = in[(size_t)j*width+i] / gr_ratio;
    }
  }
}


//
// x-trans specific demosaicing algorithms
//

#define fcol(row,col) xtrans[((row)+6)%6][((col)+6)%6]

// xtrans_interpolate adapted from dcraw 9.20

#define CLIPF(x) CLAMPS(x,0.0f,1.0f)
#define SQR(x) ((x)*(x))
#define TS 256          /* Tile Size */

/*
   Frank Markesteijn's algorithm for Fuji X-Trans sensors
 */
static void
xtrans_markesteijn_interpolate(
  float *const out, const float *const in,
  const dt_iop_roi_t *const roi_out,
  const dt_iop_roi_t *const roi_in,
  const dt_image_t *img,
  const uint8_t (*const xtrans)[6],
  const int passes)
{
  static const short orth[12] = { 1,0,0,1,-1,0,0,-1,1,0,0,1 },
        patt[2][16] = { { 0,1,0,-1,2,0,-1,0,1,1,1,-1,0,0,0,0 },
                        { 0,1,0,-2,1,0,-2,0,1,1,-2,-2,1,-1,-1,1 } },
        dir[4] = { 1,TS,TS+1,TS-1 };

  short allhex[3][3][2][8];
  // initialize as gcc thinks these could be uninitialized below
  unsigned short sgrow=0, sgcol=0;

  const int width = roi_out->width+12;
  const int height = roi_out->height+12;
  const int xoff = roi_in->x;
  const int yoff = roi_in->y;
  const int ndir = 4 << (passes > 1);

  const size_t image_size = width*height*4*(size_t)sizeof(float);
  const size_t buffer_size = TS*TS*(ndir*((size_t)sizeof(char)+7*(size_t)sizeof(float)));
  char *const all_buffers = (char *) dt_alloc_align(16, image_size+dt_get_num_threads()*buffer_size);
  if (!all_buffers)
  {
    printf("[demosaic] not able to allocate Markesteijn buffers\n");
    return;
  }

  float (*const image)[4] = (float (*)[4]) all_buffers;
  // Work in a 4-color temp buffer with 6-pixel borders filled with
  // mirrored/interpolated edge data. The extra border helps the
  // algorithm avoid discontinuities at image edges.
#define TRANSLATE(n,size) ((n<6)?(6-n):((n>=size-6)?(2*size-n-20):(n-6)))
#ifdef _OPENMP
  #pragma omp parallel for default(none) schedule(static)
#endif
  for (int row=0; row < height; row++)
    for (int col=0; col < width; col++)
      if (col>=6 && row >= 6 && col < width-6 && row < height-6)
      {
        const uint8_t f = fcol(row-6+yoff,col-6+xoff);
        for (int c=0; c<3; c++)
          image[row*width+col][c] = (c == f) ? in[roi_in->width*(row-6) + (col-6)] : 0;
      }
      else
      {
        float sum[3] = {0.0f};
        uint8_t count[3] = {0};
        for (int y=row-1; y <= row+1; y++)
          for (int x=col-1; x <= col+1; x++)
          {
            const int xx=TRANSLATE(x,width), yy=TRANSLATE(y,height);
            const uint8_t f = fcol(yy+yoff,xx+xoff);
            sum[f] += in[roi_in->width*yy + xx];
            count[f]++;
          }
        const int cx=TRANSLATE(col,width), cy=TRANSLATE(row,height);
        const uint8_t f = fcol(cy+yoff,cx+xoff);
        for (int c=0; c<3; c++)
          if (c != f && count[c] != 0)
            image[row*width+col][c] = sum[c] / count[c];
          else
            image[row*width+col][c] = in[roi_in->width*cy + cx];
      }

  /* Map a green hexagon around each non-green pixel and vice versa:    */
  for (int row=0; row < 3; row++)
    for (int col=0; col < 3; col++)
      for (int ng=0, d=0; d < 10; d+=2)
      {
        int g = fcol(row,col) == 1;
        if (fcol(row+orth[d],col+orth[d+2]) == 1) ng=0; else ng++;
        if (ng == 4)
        {
          sgrow = row;
          sgcol = col;
        }
        if (ng == g+1)
          for (int c=0; c<8; c++)
          {
            int v = orth[d  ]*patt[g][c*2] + orth[d+1]*patt[g][c*2+1];
            int h = orth[d+2]*patt[g][c*2] + orth[d+3]*patt[g][c*2+1];
            allhex[row][col][0][c^(g*2 & d)] = h + v*width;
            allhex[row][col][1][c^(g*2 & d)] = h + v*TS;
          }
      }

  /* Set green1 and green3 to the minimum and maximum allowed values:   */
#ifdef _OPENMP
  #pragma omp parallel for default(none) shared(allhex, sgrow) schedule(static)
#endif
  for (int row=2; row < height-2; row++)
  {
    float min=FLT_MAX, max=0;
    for (int col=2; col < width-2; col++)
    {
      if (fcol(yoff+row,xoff+col) == 1)
      {
        min=FLT_MAX;
        max=0;
        continue;
      }
      float (*pix)[4] = image + row*width + col;
      short *hex = allhex[row%3][col%3][0];
      if (max==0)
        for (int c=0; c<6; c++)
        {
          float val = pix[hex[c]][1];
          min = fminf(min,val);
          max = fmaxf(max,val);
        }
      pix[0][1] = min;
      pix[0][3] = max;
      switch ((row-sgrow) % 3)
      {
        case 1:
          if (row < height-3)
          {
            row++;
            col--;
          }
          break;
        case 2:
          min=FLT_MAX;
          max=0;
          if ((col+=2) < width-3 && row > 2) row--;
      }
    }
  }

#ifdef _OPENMP
  #pragma omp parallel for default(none) shared(sgrow, sgcol, allhex) schedule(static)
#endif
  for (int top=3; top < height-19; top += TS-16)
  {
    char *const buffer = all_buffers + image_size + dt_get_thread_num() * buffer_size;
    float       (*rgb)[TS][TS][3]  = (float(*)[TS][TS][3]) buffer;
    float (*const yuv)    [TS][3]  = (float(*)    [TS][3])(buffer + TS*TS*(ndir*3*sizeof(float)));
    float (*const drv)[TS][TS]     = (float(*)[TS][TS])   (buffer + TS*TS*(ndir*6*sizeof(float)));
    char (*const homo)[TS][TS]     = (char (*)[TS][TS])   (buffer + TS*TS*(ndir*7*sizeof(float)));

    for (int left=3; left < width-19; left += TS-16)
    {
      int mrow = MIN (top+TS, height-3);
      int mcol = MIN (left+TS, width-3);
      for (int row=top; row < mrow; row++)
        for (int col=left; col < mcol; col++)
          memcpy (rgb[0][row-top][col-left], image[row*width+col], 3*sizeof(float));
      for (int c=0; c<3; c++) memcpy (rgb[c+1], rgb[0], sizeof *rgb);

      /* Interpolate green horizontally, vertically, and along both diagonals: */
      for (int row=top; row < mrow; row++)
        for (int col=left; col < mcol; col++)
        {
          float color[8];
          int f = fcol(row+yoff,col+xoff);
          if (f == 1) continue;
          float (*pix)[4] = image + row*width + col;
          short *hex = allhex[row%3][col%3][0];
          color[0] = 0.68 * (pix[  hex[1]][1] + pix[  hex[0]][1]) -
                     0.18 * (pix[2*hex[1]][1] + pix[2*hex[0]][1]);
          color[1] = 0.87 *  pix[  hex[3]][1] + pix[  hex[2]][1] * 0.13 +
                     0.36 * (pix[      0 ][f] - pix[ -hex[2]][f]);
          for (int c=0; c<2; c++) color[2+c] =
                0.64 * pix[hex[4+c]][1] + 0.36 * pix[-2*hex[4+c]][1] + 0.13 *
                (2*pix[0][f] - pix[3*hex[4+c]][f] - pix[-3*hex[4+c]][f]);
          for (int c=0; c<4; c++) rgb[c^!((row-sgrow) % 3)][row-top][col-left][1] =
                CLAMPS(color[c],pix[0][1],pix[0][3]);
        }

      for (int pass=0; pass < passes; pass++)
      {
        if (pass == 1)
          memcpy (rgb+=4, buffer, 4*sizeof *rgb);

        /* Recalculate green from interpolated values of closer pixels: */
        if (pass)
        {
          for (int row=top+2; row < mrow-2; row++)
            for (int col=left+2; col < mcol-2; col++)
            {
              int f = fcol(row+yoff,col+xoff);
              if (f == 1) continue;
              float (*pix)[4] = image + row*width + col;
              short *hex = allhex[row%3][col%3][1];
              for (int d=3; d < 6; d++)
              {
                float (*rfx)[3] = &rgb[(d-2)^!((row-sgrow) % 3)][row-top][col-left];
                float val = rfx[-2*hex[d]][1] + 2*rfx[hex[d]][1]
                    - rfx[-2*hex[d]][f] - 2*rfx[hex[d]][f] + 3*rfx[0][f];
                rfx[0][1] = CLAMPS(val/3,pix[0][1],pix[0][3]);
              }
            }
        }

        /* Interpolate red and blue values for solitary green pixels:   */
        for (int row=(top-sgrow+4)/3*3+sgrow; row < mrow-2; row+=3)
          for (int col=(left-sgcol+4)/3*3+sgcol; col < mcol-2; col+=3)
          {
            float (*rfx)[3] = &rgb[0][row-top][col-left];
            int h = fcol(row+yoff,col+xoff+1);
            float diff[6] = {0.0f};
            float color[3][8];
            for (int i=1, d=0; d < 6; d++, i^=TS^1, h^=2)
            {
              for (int c=0; c < 2; c++, h^=2)
              {
                float g = 2*rfx[0][1] - rfx[i<<c][1] - rfx[-i<<c][1];
                color[h][d] = g + rfx[i<<c][h] + rfx[-i<<c][h];
                if (d > 1)
                  diff[d] += SQR (rfx[i<<c][1] - rfx[-i<<c][1]
                                - rfx[i<<c][h] + rfx[-i<<c][h]) + SQR(g);
              }
              if (d > 1 && (d & 1))
                if (diff[d-1] < diff[d])
                  for (int c=0; c<2; c++) color[c*2][d] = color[c*2][d-1];
              if (d < 2 || (d & 1))
              {
                for (int c=0; c<2; c++) rfx[0][c*2] = CLIPF(color[c*2][d]/2);
                rfx += TS*TS;
              }
            }
          }

        /* Interpolate red for blue pixels and vice versa:              */
        for (int row=top+1; row < mrow-1; row++)
          for (int col=left+1; col < mcol-1; col++)
          {
            int f = 2-fcol(row+yoff,col+xoff);
            if (f == 1) continue;
            float (*rfx)[3] = &rgb[0][row-top][col-left];
            int i = (row-sgrow) % 3 ? TS:1;
            for (int d=0; d < 4; d++, rfx += TS*TS)
              rfx[0][f] = CLIPF((rfx[i][f] + rfx[-i][f] +
                  2*rfx[0][1] - rfx[i][1] - rfx[-i][1])/2);
          }

        /* Fill in red and blue for 2x2 blocks of green:                */
        for (int row=top+2; row < mrow-2; row++)
          if ((row-sgrow) % 3)
            for (int col=left+2; col < mcol-2; col++)
              if ((col-sgcol) % 3)
              {
                float (*rfx)[3] = &rgb[0][row-top][col-left];
                short *hex = allhex[row%3][col%3][1];
                for (int d=0; d < ndir; d+=2, rfx += TS*TS)
                  if (hex[d] + hex[d+1])
                  {
                    float g = 3*rfx[0][1] - 2*rfx[hex[d]][1] - rfx[hex[d+1]][1];
                    for (int c=0; c < 4; c+=2)
                      rfx[0][c] = CLIPF((g + 2*rfx[hex[d]][c] + rfx[hex[d+1]][c])/3);
                  }
                  else
                  {
                    float g = 2*rfx[0][1] - rfx[hex[d]][1] - rfx[hex[d+1]][1];
                    for (int c=0; c < 4; c+=2)
                      rfx[0][c] = CLIPF((g + rfx[hex[d]][c] + rfx[hex[d+1]][c])/2);
                  }
              }
      }
      rgb = (float(*)[TS][TS][3]) buffer;
      mrow -= top;
      mcol -= left;

      /* Convert to perceptual colorspace and differentiate in all directions:  */
      // Original dcraw algorithm uses CIELab as perceptual space
      // (presumably coming from original AHD) and converts taking
      // camera matrix into account. Now use YPbPr which requires much
      // less code and is nearly indistinguishable. It assumes the
      // camera RGB is roughly linear.
      for (int d=0; d < ndir; d++)
      {
        for (int row=2; row < mrow-2; row++)
          for (int col=2; col < mcol-2; col++)
          {
            float *rx = rgb[d][row][col];
            // use ITU-R BT.2020 YPbPr, which is great, but could use
            // a better/simpler choice? note that imageop.h provides
            // dt_iop_RGB_to_YCbCr which uses Rec. 601 conversion,
            // which appears less good with specular highlights
            float y = 0.2627 * rx[0] + 0.6780 * rx[1] + 0.0593 * rx[2];
            yuv[row][col][0] = y;
            yuv[row][col][1] = (rx[2]-y)*0.56433;
            yuv[row][col][2] = (rx[0]-y)*0.67815;
          }
        int f=dir[d & 3];
        for (int row=3; row < mrow-3; row++)
          for (int col=3; col < mcol-3; col++)
          {
            float (*yfx)[3] = &yuv[row][col];
            float g = 2*yfx[0][0] - yfx[f][0] - yfx[-f][0];
            drv[d][row][col] = SQR(g)
              + SQR(2*yfx[0][1] - yfx[f][1] - yfx[-f][1])
              + SQR(2*yfx[0][2] - yfx[f][2] - yfx[-f][2]);
          }
      }

      /* Build homogeneity maps from the derivatives:                   */
      memset(homo, 0, ndir*TS*TS);
      for (int row=4; row < mrow-4; row++)
        for (int col=4; col < mcol-4; col++)
        {
          float tr=FLT_MAX;
          for (int d=0; d < ndir; d++)
            if (tr > drv[d][row][col])
                tr = drv[d][row][col];
          tr *= 8;
          for (int d=0; d < ndir; d++)
            for (int v=-1; v <= 1; v++)
              for (int h=-1; h <= 1; h++)
                if (drv[d][row+v][col+h] <= tr)
                  homo[d][row][col]++;
        }

      /* Average the most homogenous pixels for the final result:       */
      if (height-top < TS+4) mrow = height-top+2;
      if (width-left < TS+4) mcol = width-left+2;
      for (int row = MIN(top,8); row < mrow-8; row++)
        for (int col = MIN(left,8); col < mcol-8; col++)
        {
          int hm[8] = { 0 };
          for (int d=0; d < ndir; d++)
          {
            for (int v=-2; v <= 2; v++)
              for (int h=-2; h <= 2; h++)
                hm[d] += homo[d][row+v][col+h];
          }
          for (int d=0; d < ndir-4; d++)
            if (hm[d] < hm[d+4]) hm[d  ] = 0; else
            if (hm[d] > hm[d+4]) hm[d+4] = 0;
          unsigned short max=hm[0];
          for (int d=1; d < ndir; d++)
            if (max < hm[d]) max = hm[d];
          max -= max >> 3;
          float avg[4] = {0.0f};
          for (int d=0; d < ndir; d++)
            if (hm[d] >= max)
            {
              for (int c=0; c<3; c++) avg[c] += rgb[d][row][col][c];
              avg[3]+=1;
            }
          for (int c=0; c<3; c++) image[(row+top)*width+col+left][c] = avg[c]/avg[3];
        }
    }
  }

#ifdef _OPENMP
  #pragma omp parallel for default(none) schedule(static)
#endif
  for (int row=0; row < roi_out->height; row++)
    for (int col=0; col < roi_out->width; col++)
      for (int c=0; c<3; c++)
        out[4*(roi_out->width*row + col) + c] = image[(row+6)*width+(col+6)][c];

  free(all_buffers);
}

#undef TS


/* taken from dcraw and demosaic_ppg below */

static void
xtrans_lin_interpolate(
  float *const out, const float *const in,
  const dt_iop_roi_t *const roi_out,
  const dt_iop_roi_t *const roi_in,
  const uint8_t (*const xtrans)[6])
{
  // border interpolate
#ifdef _OPENMP
  #pragma omp parallel for default(none) schedule(static)
#endif
  for (int row=0; row < roi_out->height; row++)
    for (int col=0; col < roi_out->width; col++)
    {
      float sum[3] = {0.0f};
      uint8_t count[3] = {0};
      if (col==1 && row >= 1 && row < roi_out->height-1)
        col = roi_out->width-1;
      // average all the adjoining pixels inside image by color
      for (int y=row-1; y != row+2; y++)
        for (int x=col-1; x != col+2; x++)
        {
          const int yy = y + roi_out->y, xx = x + roi_out->x;
          if (yy >= 0 && xx >= 0 && yy < roi_in->height && xx < roi_in->width)
          {
            const uint8_t f = fcol(y+roi_in->y,x+roi_in->x);
            sum[f] += in[y*roi_in->width + x];
            count[f]++;
          }
        }
      const uint8_t f = fcol(row+roi_in->y,col+roi_in->x);
      // for current cell, copy the current sensor's color data,
      // interpolate the other two colors from surrounding pixels of
      // their color
      for (int c=0; c<3; c++)
      {
        if (c != f && count[c] != 0)
          out[4*(row*roi_out->width+col)+c] = sum[c] / count[c];
        else
          out[4*(row*roi_out->width+col)+c] = in[(row+roi_out->y)*roi_in->width+col+roi_out->x];
      }
    }

  // build interpolation lookup table which for a given offset in the sensor
  // lists neighboring pixels from which to interpolate:
  // NUM_PIXELS                 # of neighboring pixels to read
  // for (1..NUM_PIXELS):
  //   OFFSET                   # in bytes from current pixel
  //   WEIGHT                   # how much weight to give this neighbor
  //   COLOR                    # sensor color
  // # weights of adjoining pixels not of this pixel's color
  // COLORA TOT_WEIGHT
  // COLORB TOT_WEIGHT
  // COLORPIX                   # color of center pixel

  // would be nice to precalculate this for each orientation, but offsets change
  // depending on roi_in->width
  int lookup[6][6][32];
  for (int row=0; row < 6; row++)
    for (int col=0; col < 6; col++)
    {
      int *ip = lookup[row][col]+1;
      int sum[3] = {0};
      const uint8_t f = fcol(row+roi_in->y,col+roi_in->x);
      // make list of adjoining pixel offsets by weight & color
      for (int y=-1; y <= 1; y++)
        for (int x=-1; x <= 1; x++)
        {
          int weight = 1 << ((y==0) + (x==0));
          const uint8_t color = fcol(row+y+roi_in->y,col+x+roi_in->x);
          if (color == f) continue;
          *ip++ = (roi_in->width*(y + roi_out->y) + roi_out->x + x);
          *ip++ = weight;
          *ip++ = color;
          sum[color] += weight;
        }
      lookup[row][col][0] = (ip - lookup[row][col]) / 3; /* # of neighboring pixels found */
      for (int c=0; c < 3; c++)
        if (c != f)
        {
          *ip++ = c;
          *ip++ = sum[c];
        }
      *ip = f;
    }

#ifdef _OPENMP
  #pragma omp parallel for default(none) shared(lookup) schedule(static)
#endif
  for (int row=1; row < roi_out->height-1; row++)
  {
    float *buf = out + 4*roi_out->width*row + 4;
    const float *buf_in = in + roi_in->width*(row + roi_out->y) + 1 + roi_out->x;
    for (int col=1; col < roi_out->width-1; col++)
    {
      float sum[3] = {0.0f};
      int *ip = lookup[row % 6][col % 6]; // interp. lookup for current pos in sensor
      // for each adjoining pixel not of this pixel's color, sum up its weighted values
      for (int i=*ip++; i--; ip+=3)
        sum[ip[2]] += buf_in[ip[0]] * ip[1];
      // for each interpolated color, load it into the pixel
      for (int i=3; --i; ip+=2)
        buf[*ip] = sum[ip[0]] / ip[1];
      buf[*ip] = *buf_in;
      buf += 4;
      buf_in ++;
    }
  }
}


// VNG interpolate adapted from dcraw 9.20

/*
   This algorithm is officially called:

   "Interpolation using a Threshold-based variable number of gradients"

   described in http://scien.stanford.edu/pages/labsite/1999/psych221/projects/99/tingchen/algodep/vargra.html

   I've extended the basic idea to work with non-Bayer filter arrays.
   Gradients are numbered clockwise from NW=0 to W=7.
 */
static void
xtrans_vng_interpolate(
  float *const out, const float *const in,
  const dt_iop_roi_t *const roi_out, const dt_iop_roi_t *const roi_in,
  const uint8_t (*const xtrans)[6])
{
  static const signed char terms[] =
    {
      -2,-2,+0,-1,1,0x01, -2,-2,+0,+0,2,0x01, -2,-1,-1,+0,1,0x01,
      -2,-1,+0,-1,1,0x02, -2,-1,+0,+0,1,0x03, -2,-1,+0,+1,2,0x01,
      -2,+0,+0,-1,1,0x06, -2,+0,+0,+0,2,0x02, -2,+0,+0,+1,1,0x03,
      -2,+1,-1,+0,1,0x04, -2,+1,+0,-1,2,0x04, -2,+1,+0,+0,1,0x06,
      -2,+1,+0,+1,1,0x02, -2,+2,+0,+0,2,0x04, -2,+2,+0,+1,1,0x04,
      -1,-2,-1,+0,1,0x80, -1,-2,+0,-1,1,0x01, -1,-2,+1,-1,1,0x01,
      -1,-2,+1,+0,2,0x01, -1,-1,-1,+1,1,0x88, -1,-1,+1,-2,1,0x40,
      -1,-1,+1,-1,1,0x22, -1,-1,+1,+0,1,0x33, -1,-1,+1,+1,2,0x11,
      -1,+0,-1,+2,1,0x08, -1,+0,+0,-1,1,0x44, -1,+0,+0,+1,1,0x11,
      -1,+0,+1,-2,2,0x40, -1,+0,+1,-1,1,0x66, -1,+0,+1,+0,2,0x22,
      -1,+0,+1,+1,1,0x33, -1,+0,+1,+2,2,0x10, -1,+1,+1,-1,2,0x44,
      -1,+1,+1,+0,1,0x66, -1,+1,+1,+1,1,0x22, -1,+1,+1,+2,1,0x10,
      -1,+2,+0,+1,1,0x04, -1,+2,+1,+0,2,0x04, -1,+2,+1,+1,1,0x04,
      +0,-2,+0,+0,2,0x80, +0,-1,+0,+1,2,0x88, +0,-1,+1,-2,1,0x40,
      +0,-1,+1,+0,1,0x11, +0,-1,+2,-2,1,0x40, +0,-1,+2,-1,1,0x20,
      +0,-1,+2,+0,1,0x30, +0,-1,+2,+1,2,0x10, +0,+0,+0,+2,2,0x08,
      +0,+0,+2,-2,2,0x40, +0,+0,+2,-1,1,0x60, +0,+0,+2,+0,2,0x20,
      +0,+0,+2,+1,1,0x30, +0,+0,+2,+2,2,0x10, +0,+1,+1,+0,1,0x44,
      +0,+1,+1,+2,1,0x10, +0,+1,+2,-1,2,0x40, +0,+1,+2,+0,1,0x60,
      +0,+1,+2,+1,1,0x20, +0,+1,+2,+2,1,0x10, +1,-2,+1,+0,1,0x80,
      +1,-1,+1,+1,1,0x88, +1,+0,+1,+2,1,0x08, +1,+0,+2,-1,1,0x40,
      +1,+0,+2,+1,1,0x10
    },
    chood[] = { -1,-1, -1,0, -1,+1, 0,+1, +1,+1, +1,0, +1,-1, 0,-1 };
  int *ip, *code[6][6];
  float (*brow[5])[4];
  const int width = roi_out->width, height = roi_out->height;

  xtrans_lin_interpolate(out, in, roi_out, roi_in, xtrans);

  ip = (int *) dt_alloc_align(16, (size_t)sizeof(*ip) * 6*6 * 320);
  brow[4] = (float (*)[4]) dt_alloc_align(16, (size_t)sizeof(**brow)*width*3);
  if (!ip || !brow[4])
  {
    fprintf(stderr, "[demosaic] not able to allocate VNG buffers\n");
    free(ip);
    free(brow[4]);
    return;
  }

  for (int row=0; row < 6; row++)               /* Precalculate for VNG */
    for (int col=0; col < 6; col++)
    {
      code[row][col] = ip;
      const signed char *cp = terms;
      for (int t=0; t < 64; t++)
      {
        int y1 = *cp++, x1 = *cp++;
        int y2 = *cp++, x2 = *cp++;
        int weight = *cp++;
        int grads = *cp++;
        int color = fcol(row+y1,col+x1);
        if (fcol(row+y2,col+x2) != color) continue;
        int diag = (fcol(row,col+1) == color && fcol(row+1,col) == color) ? 2:1;
        if (abs(y1-y2) == diag && abs(x1-x2) == diag) continue;
        *ip++ = (y1*width + x1)*4 + color;
        *ip++ = (y2*width + x2)*4 + color;
        *ip++ = weight;
        for (int g=0; g < 8; g++)
          if (grads & 1<<g) *ip++ = g;
        *ip++ = -1;
      }
      *ip++ = INT_MAX;
      cp=chood;
      for (int g=0; g < 8; g++)
      {
        int y = *cp++, x = *cp++;
        *ip++ = (y*width + x) * 4;
        int color = fcol(row,col);
        if (fcol(row+y,col+x) != color && fcol(row+y*2,col+x*2) == color)
          *ip++ = (y*width + x) * 8 + color;
        else
          *ip++ = 0;
      }
    }

  for (int row=0; row < 3; row++)
    brow[row] = brow[4] + row*width;
  for (int row=2; row < height-2; row++)        /* Do VNG interpolation */
  {
#ifdef _OPENMP
    #pragma omp parallel for default(none) shared(row, code, brow) private(ip) schedule(static)
#endif
    for (int col=2; col < width-2; col++)
    {
      int g;
      float gval[8] = { 0.0f };
      float *pix = out + 4*(row*width+col);
      ip = code[row%6][col%6];
      while ((g = ip[0]) != INT_MAX)            /* Calculate gradients */
      {
        float diff = fabsf(pix[g] - pix[ip[1]]) * ip[2];
        gval[ip[3]] += diff;
        ip += 5;
        if ((g = ip[-1]) == -1) continue;
        gval[g] += diff;
        while ((g = *ip++) != -1)
          gval[g] += diff;
      }
      ip++;
      float gmin = gval[0], gmax = gval[0];     /* Choose a threshold */
      for (g=1; g < 8; g++)
      {
        if (gmin > gval[g]) gmin = gval[g];
        if (gmax < gval[g]) gmax = gval[g];
      }
      if (gmax == 0)
      {
        memcpy (brow[2][col], pix, 4 * sizeof *out);
        continue;
      }
      float thold = gmin + (gmax * 0.5f);
      float sum[3] = { 0.0f };
      int color = fcol(row,col);
      int num=0;
      for (g=0; g < 8; g++,ip+=2)               /* Average the neighbors */
      {
        if (gval[g] <= thold)
        {
          for (int c=0; c < 3; c++)
            if (c == color && ip[1])
              sum[c] += (pix[c] + pix[ip[1]]) * 0.5f;
            else
              sum[c] += pix[ip[0] + c];
          num++;
        }
      }
      for (int c=0; c < 3; c++)                 /* Save to buffer */
      {
        float tot = pix[color];
        if (c != color)
          tot += (sum[c] - sum[color]) / num;
        brow[2][col][c] = CLIPF(tot);
      }
    }
    if (row > 3)                                /* Write buffer to image */
      memcpy (out + 4*((row-2)*width+2), brow[0]+2, (width-4)*4*sizeof *out);
    for (int g=0; g < 4; g++)
      brow[(g-1) & 3] = brow[g];
  }
  memcpy (out + (4*((height-4)*width+2)), brow[0]+2, (width-4)*4*sizeof *out);
  memcpy (out + (4*((height-3)*width+2)), brow[1]+2, (width-4)*4*sizeof *out);
  free (brow[4]);
  free (code[0][0]);
}


/** 1:1 demosaic from in to out, in is full buf, out is translated/cropped (scale == 1.0!) */
static void
demosaic_ppg(float *out, const float *in, dt_iop_roi_t *roi_out, const dt_iop_roi_t *roi_in, const int filters, const float thrs)
{
  // snap to start of mosaic block:
  roi_out->x = 0;//MAX(0, roi_out->x & ~1);
  roi_out->y = 0;//MAX(0, roi_out->y & ~1);
  // offsets only where the buffer ends:
  const int offx = 3; //MAX(0, 3 - roi_out->x);
  const int offy = 3; //MAX(0, 3 - roi_out->y);
  const int offX = 3; //MAX(0, 3 - (roi_in->width  - (roi_out->x + roi_out->width)));
  const int offY = 3; //MAX(0, 3 - (roi_in->height - (roi_out->y + roi_out->height)));

  // border interpolate
  float sum[8];
  for (int j=0; j < roi_out->height; j++) for (int i=0; i < roi_out->width; i++)
    {
      if (i == offx && j >= offy && j < roi_out->height-offY)
        i = roi_out->width-offX;
      if(i == roi_out->width) break;
      memset (sum, 0, sizeof(float)*8);
      for (int y=j-1; y != j+2; y++) for (int x=i-1; x != i+2; x++)
        {
          const int yy = y + roi_out->y, xx = x + roi_out->x;
          if (yy >= 0 && xx >= 0 && yy < roi_in->height && xx < roi_in->width)
          {
            int f = FC(y,x,filters);
            sum[f] += in[(size_t)yy*roi_in->width+xx];
            sum[f+4]++;
          }
        }
      int f = FC(j,i,filters);
      for(int c=0; c<3; c++)
      {
        if (c != f && sum[c+4] > 0.0f)
          out[4*((size_t)j*roi_out->width+i)+c] = sum[c] / sum[c+4];
        else
          out[4*((size_t)j*roi_out->width+i)+c] = in[((size_t)j+roi_out->y)*roi_in->width+i+roi_out->x];
      }
    }
  const int median = thrs > 0.0f;
  // if(median) fbdd_green(out, in, roi_out, roi_in, filters);
  if(median)
  {
    float *med_in = (float *)dt_alloc_align(16, (size_t)roi_in->height*roi_in->width*sizeof(float));
    pre_median(med_in, in, roi_in, filters, 1, thrs);
    in = med_in;
  }
  // for all pixels: interpolate green into float array, or copy color.
#ifdef _OPENMP
  #pragma omp parallel for default(none) shared(roi_in, roi_out, in, out) schedule(static)
#endif
  for (int j=offy; j < roi_out->height-offY; j++)
  {
    float *buf = out + (size_t)4*roi_out->width*j + 4*offx;
    const float *buf_in = in + (size_t)roi_in->width*(j + roi_out->y) + offx + roi_out->x;
    for (int i=offx; i < roi_out->width-offX; i++)
    {
      const int c = FC(j,i,filters);
      // prefetch what we need soon (load to cpu caches)
      _mm_prefetch((char *)buf_in + 256, _MM_HINT_NTA); // TODO: try HINT_T0-3
      _mm_prefetch((char *)buf_in +   roi_in->width + 256, _MM_HINT_NTA);
      _mm_prefetch((char *)buf_in + 2*roi_in->width + 256, _MM_HINT_NTA);
      _mm_prefetch((char *)buf_in + 3*roi_in->width + 256, _MM_HINT_NTA);
      _mm_prefetch((char *)buf_in -   roi_in->width + 256, _MM_HINT_NTA);
      _mm_prefetch((char *)buf_in - 2*roi_in->width + 256, _MM_HINT_NTA);
      _mm_prefetch((char *)buf_in - 3*roi_in->width + 256, _MM_HINT_NTA);
      __m128 col = _mm_load_ps(buf);
      float *color = (float*)&col;
      const float pc = buf_in[0];
      // if(__builtin_expect(c == 0 || c == 2, 1))
      if(c == 0 || c == 2)
      {
        color[c] = pc;
        // get stuff (hopefully from cache)
        const float pym  = buf_in[ - roi_in->width*1];
        const float pym2 = buf_in[ - roi_in->width*2];
        const float pym3 = buf_in[ - roi_in->width*3];
        const float pyM  = buf_in[ + roi_in->width*1];
        const float pyM2 = buf_in[ + roi_in->width*2];
        const float pyM3 = buf_in[ + roi_in->width*3];
        const float pxm  = buf_in[ - 1];
        const float pxm2 = buf_in[ - 2];
        const float pxm3 = buf_in[ - 3];
        const float pxM  = buf_in[ + 1];
        const float pxM2 = buf_in[ + 2];
        const float pxM3 = buf_in[ + 3];

        const float guessx = (pxm + pc + pxM) * 2.0f - pxM2 - pxm2;
        const float diffx  = (fabsf(pxm2 - pc) +
                              fabsf(pxM2 - pc) +
                              fabsf(pxm  - pxM)) * 3.0f +
                             (fabsf(pxM3 - pxM) + fabsf(pxm3 - pxm)) * 2.0f;
        const float guessy = (pym + pc + pyM) * 2.0f - pyM2 - pym2;
        const float diffy  = (fabsf(pym2 - pc) +
                              fabsf(pyM2 - pc) +
                              fabsf(pym  - pyM)) * 3.0f +
                             (fabsf(pyM3 - pyM) + fabsf(pym3 - pym)) * 2.0f;
        if(diffx > diffy)
        {
          // use guessy
          const float m = fminf(pym, pyM);
          const float M = fmaxf(pym, pyM);
          color[1] = fmaxf(fminf(guessy*.25f, M), m);
        }
        else
        {
          const float m = fminf(pxm, pxM);
          const float M = fmaxf(pxm, pxM);
          color[1] = fmaxf(fminf(guessx*.25f, M), m);
        }
      }
      else color[1] = pc;

      // write using MOVNTPS (write combine omitting caches)
      // _mm_stream_ps(buf, col);
      memcpy(buf, color, 4*sizeof(float));
      buf += 4;
      buf_in ++;
    }
  }
  // SFENCE (make sure stuff is stored now)
  // _mm_sfence();

  // for all pixels: interpolate colors into float array
#ifdef _OPENMP
  #pragma omp parallel for default(none) shared(roi_in, roi_out, out) schedule(static)
#endif
  for (int j=1; j < roi_out->height-1; j++)
  {
    float *buf = out + (size_t)4*roi_out->width*j + 4;
    for (int i=1; i < roi_out->width-1; i++)
    {
      // also prefetch direct nbs top/bottom
      _mm_prefetch((char *)buf + 256, _MM_HINT_NTA);
      _mm_prefetch((char *)buf - roi_out->width*4*sizeof(float) + 256, _MM_HINT_NTA);
      _mm_prefetch((char *)buf + roi_out->width*4*sizeof(float) + 256, _MM_HINT_NTA);

      const int c = FC(j, i, filters);
      __m128 col = _mm_load_ps(buf);
      float *color = (float *)&col;
      // fill all four pixels with correctly interpolated stuff: r/b for green1/2
      // b for r and r for b
      if(__builtin_expect(c & 1, 1)) // c == 1 || c == 3)
      {
        // calculate red and blue for green pixels:
        // need 4-nbhood:
        const float* nt = buf - 4*roi_out->width;
        const float* nb = buf + 4*roi_out->width;
        const float* nl = buf - 4;
        const float* nr = buf + 4;
        if(FC(j, i+1, filters) == 0) // red nb in same row
        {
          color[2] = (nt[2] + nb[2] + 2.0f*color[1] - nt[1] - nb[1])*.5f;
          color[0] = (nl[0] + nr[0] + 2.0f*color[1] - nl[1] - nr[1])*.5f;
        }
        else
        {
          // blue nb
          color[0] = (nt[0] + nb[0] + 2.0f*color[1] - nt[1] - nb[1])*.5f;
          color[2] = (nl[2] + nr[2] + 2.0f*color[1] - nl[1] - nr[1])*.5f;
        }
      }
      else
      {
        // get 4-star-nbhood:
        const float* ntl = buf - 4 - 4*roi_out->width;
        const float* ntr = buf + 4 - 4*roi_out->width;
        const float* nbl = buf - 4 + 4*roi_out->width;
        const float* nbr = buf + 4 + 4*roi_out->width;

        if(c == 0)
        {
          // red pixel, fill blue:
          const float diff1  = fabsf(ntl[2] - nbr[2]) + fabsf(ntl[1] - color[1]) + fabsf(nbr[1] - color[1]);
          const float guess1 = ntl[2] + nbr[2] + 2.0f*color[1] - ntl[1] - nbr[1];
          const float diff2  = fabsf(ntr[2] - nbl[2]) + fabsf(ntr[1] - color[1]) + fabsf(nbl[1] - color[1]);
          const float guess2 = ntr[2] + nbl[2] + 2.0f*color[1] - ntr[1] - nbl[1];
          if     (diff1 > diff2) color[2] = guess2 * .5f;
          else if(diff1 < diff2) color[2] = guess1 * .5f;
          else color[2] = (guess1 + guess2)*.25f;
        }
        else // c == 2, blue pixel, fill red:
        {
          const float diff1  = fabsf(ntl[0] - nbr[0]) + fabsf(ntl[1] - color[1]) + fabsf(nbr[1] - color[1]);
          const float guess1 = ntl[0] + nbr[0] + 2.0f*color[1] - ntl[1] - nbr[1];
          const float diff2  = fabsf(ntr[0] - nbl[0]) + fabsf(ntr[1] - color[1]) + fabsf(nbl[1] - color[1]);
          const float guess2 = ntr[0] + nbl[0] + 2.0f*color[1] - ntr[1] - nbl[1];
          if     (diff1 > diff2) color[0] = guess2 * .5f;
          else if(diff1 < diff2) color[0] = guess1 * .5f;
          else color[0] = (guess1 + guess2)*.25f;
        }
      }
      // _mm_stream_ps(buf, col);
      memcpy(buf, color, 4*sizeof(float));
      buf += 4;
    }
  }
  // _mm_sfence();
  if (median)
    dt_free_align((float*)in);
}


// which roi input is needed to process to this output?
// roi_out is unchanged, full buffer in is full buffer out.
void
modify_roi_in (struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, const dt_iop_roi_t *roi_out, dt_iop_roi_t *roi_in)
{
  dt_iop_demosaic_data_t *data = (dt_iop_demosaic_data_t *)piece->data;
  // this op is disabled for preview pipe/filters == 0

  *roi_in = *roi_out;
  // need 1:1, demosaic and then sub-sample. or directly sample half-size
  roi_in->x /= roi_out->scale;
  roi_in->y /= roi_out->scale;
  roi_in->width /= roi_out->scale;
  roi_in->height /= roi_out->scale;
  roi_in->scale = 1.0f;
  // clamp to even x/y, to make demosaic pattern still hold..
  if (data->filters != 9)
  {
    roi_in->x = MAX(0, roi_in->x & ~1);
    roi_in->y = MAX(0, roi_in->y & ~1);
  }
  else
  {
    // bilinear can handle any offset, but Markesteijn needs factors of 3
    roi_in->x = MAX(0, roi_in->x - (roi_in->x%3));
    roi_in->y = MAX(0, roi_in->y - (roi_in->y%3));
  }

  // clamp numeric inaccuracies to full buffer, to avoid scaling/copying in pixelpipe:
  if(abs(piece->pipe->image.width - roi_in->width) < MAX(ceilf(1.0f/roi_out->scale), 10))
    roi_in->width  = piece->pipe->image.width;

  if(abs(piece->pipe->image.height - roi_in->height) < MAX(ceilf(1.0f/roi_out->scale), 10))
    roi_in->height = piece->pipe->image.height;
}

static int get_quality()
{
  int qual = 1;
  gchar *quality = dt_conf_get_string("plugins/darkroom/demosaic/quality");
  if(quality)
  {
    if(!strcmp(quality, "always bilinear (fast)"))
      qual = 0;
    else if(!strcmp(quality, "full (possibly slow)"))
      qual = 2;
    g_free(quality);
  }
  return qual;
}

void
process (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *i, void *o, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  const dt_image_t *img = &self->dev->image_storage;
  const float threshold = 0.0001f * img->exif_iso;

  dt_iop_roi_t roi, roo;
  roi = *roi_in;
  roo = *roi_out;
  roo.x = roo.y = 0;
  // roi_out->scale = global scale: (iscale == 1.0, always when demosaic is on)

  dt_iop_demosaic_data_t *data = (dt_iop_demosaic_data_t *)piece->data;

  const int qual = get_quality();
  int demosaicing_method = data->demosaicing_method;
  if(piece->pipe->type == DT_DEV_PIXELPIPE_FULL && qual < 2 && roi_out->scale<=.99999f) // only overwrite setting if quality << requested and in dr mode
    demosaicing_method = (img->filters != 9) ? DT_IOP_DEMOSAIC_PPG : MIN(demosaicing_method, DT_IOP_DEMOSAIC_LINEAR + qual);

  const float *const pixels = (float *)i;
  if(roi_out->scale > .99999f && roi_out->scale < 1.00001f)
  {
    // output 1:1
    if(img->filters==9)
    {
      if (demosaicing_method == DT_IOP_DEMOSAIC_LINEAR)
        xtrans_lin_interpolate((float *)o, pixels, &roo, &roi, img->xtrans);
      else if (demosaicing_method < DT_IOP_DEMOSAIC_MARKESTEIJN)
        xtrans_vng_interpolate((float *)o, pixels, &roo, &roi, img->xtrans);
      else
        xtrans_markesteijn_interpolate(
          (float *)o, pixels, &roo, &roi, img, img->xtrans, 1+(demosaicing_method-DT_IOP_DEMOSAIC_MARKESTEIJN)*2);
    }
    // green eq:
    else if(data->green_eq != DT_IOP_GREEN_EQ_NO)
    {
      float *in = (float *)dt_alloc_align(16, (size_t)roi_in->height*roi_in->width*sizeof(float));
      switch(data->green_eq)
      {
        case DT_IOP_GREEN_EQ_FULL:
          green_equilibration_favg(in, pixels, roi_in->width, roi_in->height,
                                   data->filters,  roi_in->x, roi_in->y);
          break;
        case DT_IOP_GREEN_EQ_LOCAL:
          green_equilibration_lavg(in, pixels, roi_in->width, roi_in->height,
                                   data->filters, roi_in->x, roi_in->y, 0, threshold);
          break;
        case DT_IOP_GREEN_EQ_BOTH:
          green_equilibration_favg(in, pixels, roi_in->width, roi_in->height,
                                   data->filters,  roi_in->x, roi_in->y);
          green_equilibration_lavg(in, in, roi_in->width, roi_in->height,
                                   data->filters, roi_in->x, roi_in->y, 1, threshold);
          break;
      }
      if (demosaicing_method != DT_IOP_DEMOSAIC_AMAZE)
        demosaic_ppg((float *)o, in, &roo, &roi, data->filters, data->median_thrs);
      else
        amaze_demosaic_RT(self, piece, in, (float *)o, &roi, &roo, data->filters);
      dt_free_align(in);
    }
    else
    {
      if (demosaicing_method != DT_IOP_DEMOSAIC_AMAZE)
        demosaic_ppg((float *)o, pixels, &roo, &roi, data->filters, data->median_thrs);
      else
        amaze_demosaic_RT(self, piece, pixels, (float *)o, &roi, &roo, data->filters);
    }
  }
  else if(roi_out->scale > (img->filters == 9 ? 0.333f : .5f) ||       // also covers roi_out->scale >1
          (piece->pipe->type == DT_DEV_PIXELPIPE_FULL && qual > 0) ||  // or in darkroom mode and quality requested by user settings
          (piece->pipe->type == DT_DEV_PIXELPIPE_EXPORT))              // we assume you always want that for exports.
  {
    // demosaic and then clip and zoom
    // roo.x = roi_out->x / global_scale;
    // roo.y = roi_out->y / global_scale;
    roo.width  = roi_out->width / roi_out->scale;
    roo.height = roi_out->height / roi_out->scale;
    roo.scale = 1.0f;

    float *tmp = (float *)dt_alloc_align(16, (size_t)roo.width*roo.height*4*sizeof(float));
    if(img->filters==9)
    {
      if (demosaicing_method == DT_IOP_DEMOSAIC_LINEAR)
        xtrans_lin_interpolate(tmp, pixels, &roo, &roi, img->xtrans);
      else if (demosaicing_method < DT_IOP_DEMOSAIC_MARKESTEIJN)
        xtrans_vng_interpolate(tmp, pixels, &roo, &roi, img->xtrans);
      else
        xtrans_markesteijn_interpolate(
          tmp, pixels, &roo, &roi, img, img->xtrans, 1+(demosaicing_method-DT_IOP_DEMOSAIC_MARKESTEIJN)*2);
    }
    else if(data->green_eq != DT_IOP_GREEN_EQ_NO)
    {
      float *in = (float *)dt_alloc_align(16, (size_t)roi_in->height*roi_in->width*sizeof(float));
      switch(data->green_eq)
      {
        case DT_IOP_GREEN_EQ_FULL:
          green_equilibration_favg(in, pixels, roi_in->width, roi_in->height,
                                   data->filters,  roi_in->x, roi_in->y);
          break;
        case DT_IOP_GREEN_EQ_LOCAL:
          green_equilibration_lavg(in, pixels, roi_in->width, roi_in->height,
                                   data->filters, roi_in->x, roi_in->y, 0, threshold);
          break;
        case DT_IOP_GREEN_EQ_BOTH:
          green_equilibration_favg(in, pixels, roi_in->width, roi_in->height,
                                   data->filters,  roi_in->x, roi_in->y);
          green_equilibration_lavg(in, in, roi_in->width, roi_in->height,
                                   data->filters, roi_in->x, roi_in->y, 1, threshold);
          break;
      }
      // wanted ppg or zoomed out a lot and quality is limited to 1
      if(demosaicing_method != DT_IOP_DEMOSAIC_AMAZE)
        demosaic_ppg(tmp, in, &roo, &roi, data->filters, data->median_thrs);
      else
        amaze_demosaic_RT(self, piece, in, tmp, &roi, &roo, data->filters);
      dt_free_align(in);
    }
    else
    {
      if(demosaicing_method != DT_IOP_DEMOSAIC_AMAZE)
        demosaic_ppg(tmp, pixels, &roo, &roi, data->filters, data->median_thrs);
      else
        amaze_demosaic_RT(self, piece, pixels, tmp, &roi, &roo, data->filters);
    }
    roi = *roi_out;
    roi.x = roi.y = 0;
    roi.scale = roi_out->scale;
    dt_iop_clip_and_zoom((float *)o, tmp, &roi, &roo, roi.width, roo.width);
    dt_free_align(tmp);
  }
  else
  {
    // sample half (or 1/3)-size raw
    const float clip = fminf(piece->pipe->processed_maximum[0], fminf(piece->pipe->processed_maximum[1], piece->pipe->processed_maximum[2]));
    if(img->filters==9)
      dt_iop_clip_and_zoom_demosaic_third_size_xtrans_f((float *)o, pixels, &roo, &roi, roo.width, roi.width, img->xtrans);
    else if(piece->pipe->type == DT_DEV_PIXELPIPE_EXPORT && data->median_thrs > 0.0f)
    {
      float *tmp = (float *)dt_alloc_align(16, (size_t)sizeof(float)*roi_in->width*roi_in->height);
      pre_median_b(tmp, pixels, roi_in, data->filters, 1, data->median_thrs);
      dt_iop_clip_and_zoom_demosaic_half_size_f((float *)o, tmp, &roo, &roi, roo.width, roi.width, data->filters, clip);
      dt_free_align(tmp);
    }
    else
      dt_iop_clip_and_zoom_demosaic_half_size_f((float *)o, pixels, &roo, &roi, roo.width, roi.width, data->filters, clip);
  }
  if(data->color_smoothing) color_smoothing(o, roi_out, data->color_smoothing);
}

#ifdef HAVE_OPENCL
int
process_cl (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
            const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  dt_iop_demosaic_data_t *data = (dt_iop_demosaic_data_t *)piece->data;
  dt_iop_demosaic_global_data_t *gd = (dt_iop_demosaic_global_data_t *)self->data;
  const dt_image_t *img = &self->dev->image_storage;
  const float threshold = 0.0001f * img->exif_iso;

  if(roi_out->scale >= 1.00001f)
  {
    dt_print(DT_DEBUG_OPENCL, "[opencl_demosaic] demosaic with upscaling not yet supported by opencl code\n");
    return FALSE;
  }

  const int devid = piece->pipe->devid;
  const int qual = get_quality();

  cl_mem dev_tmp = NULL;
  cl_mem dev_green_eq = NULL;
  cl_int err = -999;

  if(roi_out->scale > .99999f)
  {
    const int width = roi_out->width;
    const int height = roi_out->height;
    size_t sizes[2] = { ROUNDUPWD(width), ROUNDUPHT(height) };
    // 1:1 demosaic
    dev_green_eq = NULL;
    if(data->green_eq != DT_IOP_GREEN_EQ_NO)
    {
      // green equilibration
      dev_green_eq = dt_opencl_alloc_device(devid, roi_in->width, roi_in->height, sizeof(float));
      if (dev_green_eq == NULL) goto error;
      dt_opencl_set_kernel_arg(devid, gd->kernel_green_eq, 0, sizeof(cl_mem), &dev_in);
      dt_opencl_set_kernel_arg(devid, gd->kernel_green_eq, 1, sizeof(cl_mem), &dev_green_eq);
      dt_opencl_set_kernel_arg(devid, gd->kernel_green_eq, 2, sizeof(int), &width);
      dt_opencl_set_kernel_arg(devid, gd->kernel_green_eq, 3, sizeof(int), &height);
      dt_opencl_set_kernel_arg(devid, gd->kernel_green_eq, 4, sizeof(uint32_t), (void*)&data->filters);
      dt_opencl_set_kernel_arg(devid, gd->kernel_green_eq, 5, sizeof(float), (void*)&threshold);
      err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_green_eq, sizes);
      if(err != CL_SUCCESS) goto error;
      dev_in = dev_green_eq;
    }

    if(data->median_thrs > 0.0f)
    {
      const int one = 1;
      dt_opencl_set_kernel_arg(devid, gd->kernel_pre_median, 0, sizeof(cl_mem), &dev_in);
      dt_opencl_set_kernel_arg(devid, gd->kernel_pre_median, 1, sizeof(cl_mem), &dev_out);
      dt_opencl_set_kernel_arg(devid, gd->kernel_pre_median, 2, sizeof(int), &width);
      dt_opencl_set_kernel_arg(devid, gd->kernel_pre_median, 3, sizeof(int), &height);
      dt_opencl_set_kernel_arg(devid, gd->kernel_pre_median, 4, sizeof(uint32_t), (void*)&data->filters);
      dt_opencl_set_kernel_arg(devid, gd->kernel_pre_median, 5, sizeof(float), (void*)&data->median_thrs);
      dt_opencl_set_kernel_arg(devid, gd->kernel_pre_median, 6, sizeof(int), (void*)&one);
      err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_pre_median, sizes);
      if(err != CL_SUCCESS) goto error;

      dt_opencl_set_kernel_arg(devid, gd->kernel_ppg_green_median, 0, sizeof(cl_mem), &dev_out);
      dt_opencl_set_kernel_arg(devid, gd->kernel_ppg_green_median, 1, sizeof(cl_mem), &dev_out);
      dt_opencl_set_kernel_arg(devid, gd->kernel_ppg_green_median, 2, sizeof(int), &width);
      dt_opencl_set_kernel_arg(devid, gd->kernel_ppg_green_median, 3, sizeof(int), &height);
      dt_opencl_set_kernel_arg(devid, gd->kernel_ppg_green_median, 4, sizeof(uint32_t), (void*)&data->filters);
      err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_ppg_green_median, sizes);
      if(err != CL_SUCCESS) goto error;
    }
    else
    {
      dt_opencl_set_kernel_arg(devid, gd->kernel_ppg_green, 0, sizeof(cl_mem), &dev_in);
      dt_opencl_set_kernel_arg(devid, gd->kernel_ppg_green, 1, sizeof(cl_mem), &dev_out);
      dt_opencl_set_kernel_arg(devid, gd->kernel_ppg_green, 2, sizeof(int), &width);
      dt_opencl_set_kernel_arg(devid, gd->kernel_ppg_green, 3, sizeof(int), &height);
      dt_opencl_set_kernel_arg(devid, gd->kernel_ppg_green, 4, sizeof(uint32_t), (void*)&data->filters);
      err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_ppg_green, sizes);
      if(err != CL_SUCCESS) goto error;
    }

    dt_opencl_set_kernel_arg(devid, gd->kernel_ppg_redblue, 0, sizeof(cl_mem), &dev_out);
    dt_opencl_set_kernel_arg(devid, gd->kernel_ppg_redblue, 1, sizeof(cl_mem), &dev_out);
    dt_opencl_set_kernel_arg(devid, gd->kernel_ppg_redblue, 2, sizeof(int), &width);
    dt_opencl_set_kernel_arg(devid, gd->kernel_ppg_redblue, 3, sizeof(int), &height);
    dt_opencl_set_kernel_arg(devid, gd->kernel_ppg_redblue, 4, sizeof(uint32_t), (void*)&data->filters);
    err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_ppg_redblue, sizes);
    if(err != CL_SUCCESS) goto error;

    // manage borders
    dt_opencl_set_kernel_arg(devid, gd->kernel_border_interpolate, 0, sizeof(cl_mem), &dev_in);
    dt_opencl_set_kernel_arg(devid, gd->kernel_border_interpolate, 1, sizeof(cl_mem), &dev_out);
    dt_opencl_set_kernel_arg(devid, gd->kernel_border_interpolate, 2, sizeof(int), (void*)&width);
    dt_opencl_set_kernel_arg(devid, gd->kernel_border_interpolate, 3, sizeof(int), (void*)&height);
    dt_opencl_set_kernel_arg(devid, gd->kernel_border_interpolate, 4, sizeof(uint32_t), (void*)&data->filters);
    err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_border_interpolate, sizes);
    if(err != CL_SUCCESS) goto error;

  }
  else if(roi_out->scale > .5f ||  // full needed because zoomed in enough
          (piece->pipe->type == DT_DEV_PIXELPIPE_FULL && qual > 0) ||  // or in darkroom mode and quality requested by user settings
          (piece->pipe->type == DT_DEV_PIXELPIPE_EXPORT))              // we assume you always want that for exports.
  {
    // need to scale to right res
    dev_tmp = dt_opencl_alloc_device(devid, roi_in->width, roi_in->height, 4*sizeof(float));
    if (dev_tmp == NULL) goto error;
    const int width = roi_in->width;
    const int height = roi_in->height;
    size_t sizes[2] = { ROUNDUPWD(width), ROUNDUPHT(height) };
    if(data->green_eq != DT_IOP_GREEN_EQ_NO)
    {
      // green equilibration
      dev_green_eq = dt_opencl_alloc_device(devid, roi_in->width, roi_in->height, sizeof(float));
      if (dev_green_eq == NULL) goto error;
      dt_opencl_set_kernel_arg(devid, gd->kernel_green_eq, 0, sizeof(cl_mem), &dev_in);
      dt_opencl_set_kernel_arg(devid, gd->kernel_green_eq, 1, sizeof(cl_mem), &dev_green_eq);
      dt_opencl_set_kernel_arg(devid, gd->kernel_green_eq, 2, sizeof(int), &width);
      dt_opencl_set_kernel_arg(devid, gd->kernel_green_eq, 3, sizeof(int), &height);
      dt_opencl_set_kernel_arg(devid, gd->kernel_green_eq, 4, sizeof(uint32_t), (void*)&data->filters);
      dt_opencl_set_kernel_arg(devid, gd->kernel_green_eq, 5, sizeof(float), (void*)&threshold);
      err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_green_eq, sizes);
      if(err != CL_SUCCESS) goto error;
      dev_in = dev_green_eq;
    }

    if(data->median_thrs > 0.0f)
    {
      const int one = 1;
      dt_opencl_set_kernel_arg(devid, gd->kernel_pre_median, 0, sizeof(cl_mem), &dev_in);
      dt_opencl_set_kernel_arg(devid, gd->kernel_pre_median, 1, sizeof(cl_mem), &dev_tmp);
      dt_opencl_set_kernel_arg(devid, gd->kernel_pre_median, 2, sizeof(int), &width);
      dt_opencl_set_kernel_arg(devid, gd->kernel_pre_median, 3, sizeof(int), &height);
      dt_opencl_set_kernel_arg(devid, gd->kernel_pre_median, 4, sizeof(uint32_t), (void*)&data->filters);
      dt_opencl_set_kernel_arg(devid, gd->kernel_pre_median, 5, sizeof(float), (void*)&data->median_thrs);
      dt_opencl_set_kernel_arg(devid, gd->kernel_pre_median, 6, sizeof(int), (void*)&one);
      err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_pre_median, sizes);
      if(err != CL_SUCCESS) goto error;

      dt_opencl_set_kernel_arg(devid, gd->kernel_ppg_green_median, 0, sizeof(cl_mem), &dev_tmp);
      dt_opencl_set_kernel_arg(devid, gd->kernel_ppg_green_median, 1, sizeof(cl_mem), &dev_tmp);
      dt_opencl_set_kernel_arg(devid, gd->kernel_ppg_green_median, 2, sizeof(int), &width);
      dt_opencl_set_kernel_arg(devid, gd->kernel_ppg_green_median, 3, sizeof(int), &height);
      dt_opencl_set_kernel_arg(devid, gd->kernel_ppg_green_median, 4, sizeof(uint32_t), (void*)&data->filters);
      err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_ppg_green_median, sizes);
      if(err != CL_SUCCESS) goto error;
    }
    else
    {
      dt_opencl_set_kernel_arg(devid, gd->kernel_ppg_green, 0, sizeof(cl_mem), &dev_in);
      dt_opencl_set_kernel_arg(devid, gd->kernel_ppg_green, 1, sizeof(cl_mem), &dev_tmp);
      dt_opencl_set_kernel_arg(devid, gd->kernel_ppg_green, 2, sizeof(int), &width);
      dt_opencl_set_kernel_arg(devid, gd->kernel_ppg_green, 3, sizeof(int), &height);
      dt_opencl_set_kernel_arg(devid, gd->kernel_ppg_green, 4, sizeof(uint32_t), (void*)&data->filters);
      err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_ppg_green, sizes);
      if(err != CL_SUCCESS) goto error;
    }

    dt_opencl_set_kernel_arg(devid, gd->kernel_ppg_redblue, 0, sizeof(cl_mem), &dev_tmp);
    dt_opencl_set_kernel_arg(devid, gd->kernel_ppg_redblue, 1, sizeof(cl_mem), &dev_tmp);
    dt_opencl_set_kernel_arg(devid, gd->kernel_ppg_redblue, 2, sizeof(int), &width);
    dt_opencl_set_kernel_arg(devid, gd->kernel_ppg_redblue, 3, sizeof(int), &height);
    dt_opencl_set_kernel_arg(devid, gd->kernel_ppg_redblue, 4, sizeof(uint32_t), (void*)&data->filters);
    err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_ppg_redblue, sizes);
    if(err != CL_SUCCESS) goto error;

    // manage borders
    dt_opencl_set_kernel_arg(devid, gd->kernel_border_interpolate, 0, sizeof(cl_mem), &dev_in);
    dt_opencl_set_kernel_arg(devid, gd->kernel_border_interpolate, 1, sizeof(cl_mem), &dev_tmp);
    dt_opencl_set_kernel_arg(devid, gd->kernel_border_interpolate, 2, sizeof(int), (void*)&width);
    dt_opencl_set_kernel_arg(devid, gd->kernel_border_interpolate, 3, sizeof(int), (void*)&height);
    dt_opencl_set_kernel_arg(devid, gd->kernel_border_interpolate, 4, sizeof(uint32_t), (void*)&data->filters);
    err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_border_interpolate, sizes);
    if(err != CL_SUCCESS) goto error;

    // scale temp buffer to output buffer
    dt_iop_roi_t roi, roo;
    roi = *roi_in;
    roo = *roi_out;
    roo.x = roo.y = roi.x = roi.y = 0;
    err = dt_iop_clip_and_zoom_cl(devid, dev_out, dev_tmp, &roo, &roi);
    if(err != CL_SUCCESS) goto error;
  }
  else
  {
    // sample half-size image:
    const int zero = 0;
    cl_mem dev_pix = dev_in;
    if(piece->pipe->type == DT_DEV_PIXELPIPE_EXPORT && data->median_thrs > 0.0f)
    {
      dev_tmp = dt_opencl_alloc_device(devid, roi_in->width, roi_in->height, sizeof(float));
      if (dev_tmp == NULL) goto error;
      const int width = roi_in->width;
      const int height = roi_in->height;
      size_t sizes[2] = { ROUNDUPWD(width), ROUNDUPHT(height) };

      dt_opencl_set_kernel_arg(devid, gd->kernel_pre_median, 0, sizeof(cl_mem), &dev_in);
      dt_opencl_set_kernel_arg(devid, gd->kernel_pre_median, 1, sizeof(cl_mem), &dev_tmp);
      dt_opencl_set_kernel_arg(devid, gd->kernel_pre_median, 2, sizeof(int), &width);
      dt_opencl_set_kernel_arg(devid, gd->kernel_pre_median, 3, sizeof(int), &height);
      dt_opencl_set_kernel_arg(devid, gd->kernel_pre_median, 4, sizeof(uint32_t), (void*)&data->filters);
      dt_opencl_set_kernel_arg(devid, gd->kernel_pre_median, 5, sizeof(float), (void*)&data->median_thrs);
      dt_opencl_set_kernel_arg(devid, gd->kernel_pre_median, 6, sizeof(int), (void*)&zero);
      err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_pre_median, sizes);
      if(err != CL_SUCCESS) goto error;
      dev_pix = dev_tmp;
    }

    const int width = roi_out->width;
    const int height = roi_out->height;
    size_t sizes[2] = { ROUNDUPWD(width), ROUNDUPHT(height) };

    dt_opencl_set_kernel_arg(devid, gd->kernel_zoom_half_size, 0, sizeof(cl_mem), &dev_pix);
    dt_opencl_set_kernel_arg(devid, gd->kernel_zoom_half_size, 1, sizeof(cl_mem), &dev_out);
    dt_opencl_set_kernel_arg(devid, gd->kernel_zoom_half_size, 2, sizeof(int), &width);
    dt_opencl_set_kernel_arg(devid, gd->kernel_zoom_half_size, 3, sizeof(int), &height);
    dt_opencl_set_kernel_arg(devid, gd->kernel_zoom_half_size, 4, sizeof(int), (void*)&zero);
    dt_opencl_set_kernel_arg(devid, gd->kernel_zoom_half_size, 5, sizeof(int), (void*)&zero);
    dt_opencl_set_kernel_arg(devid, gd->kernel_zoom_half_size, 6, sizeof(int), (void*)&roi_in->width);
    dt_opencl_set_kernel_arg(devid, gd->kernel_zoom_half_size, 7, sizeof(int), (void*)&roi_in->height);
    dt_opencl_set_kernel_arg(devid, gd->kernel_zoom_half_size, 8, sizeof(float), (void*)&roi_out->scale);
    dt_opencl_set_kernel_arg(devid, gd->kernel_zoom_half_size, 9, sizeof(uint32_t), (void*)&data->filters);
    err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_zoom_half_size, sizes);
    if(err != CL_SUCCESS) goto error;
  }

  if(dev_tmp != NULL) dt_opencl_release_mem_object(dev_tmp);
  if(dev_green_eq != NULL) dt_opencl_release_mem_object(dev_green_eq);
  dev_tmp = dev_green_eq = NULL;

  // color smoothing
  if(data->color_smoothing)
  {
    dev_tmp = dt_opencl_alloc_device(devid, roi_out->width, roi_out->height, 4*sizeof(float));
    if (dev_tmp == NULL) goto error;

    const int width = roi_out->width;
    const int height = roi_out->height;

    // prepare local work group
    size_t maxsizes[3] = { 0 };        // the maximum dimensions for a work group
    size_t workgroupsize = 0;          // the maximum number of items in a work group
    unsigned long localmemsize = 0;    // the maximum amount of local memory we can use
    size_t kernelworkgroupsize = 0;    // the maximum amount of items in work group for this kernel

    // Make sure blocksize is not too large. As our kernel is very register hungry we
    // need to take maximum work group size into account
    int blocksize = BLOCKSIZE;
    int blockwd;
    int blockht;
    if(dt_opencl_get_work_group_limits(devid, maxsizes, &workgroupsize, &localmemsize) == CL_SUCCESS &&
        dt_opencl_get_kernel_work_group_size(devid, gd->kernel_color_smoothing, &kernelworkgroupsize) == CL_SUCCESS)
    {

      while(blocksize > maxsizes[0] || blocksize > maxsizes[1] || blocksize*blocksize > workgroupsize ||
            (blocksize+2)*(blocksize+2)*4*sizeof(float) > localmemsize)
      {
        if(blocksize == 1) break;
        blocksize >>= 1;
      }

      blockwd = blockht = blocksize;

      if(blockwd * blockht > kernelworkgroupsize)
        blockht = kernelworkgroupsize / blockwd;

      // speed optimized limits for my NVIDIA GTS450
      // TODO: find out if this is good for other systems as well
      blockwd = blockwd > 16 ? 16 : blockwd;
      blockht = blockht > 8 ? 8 : blockht;
    }
    else
    {
      blockwd = blockht = 1;   // slow but safe
    }

    size_t sizes[] = { ROUNDUP(width, blockwd), ROUNDUP(height, blockht), 1 };
    size_t local[] = { blockwd, blockht, 1 };
    size_t origin[] = { 0, 0, 0 };
    size_t region[] = { width, height, 1 };
    // two buffer references for our ping-pong
    cl_mem dev_t1 = dev_out;
    cl_mem dev_t2 = dev_tmp;

    for(uint32_t pass = 0; pass < data->color_smoothing; pass++)
    {

      dt_opencl_set_kernel_arg(devid, gd->kernel_color_smoothing, 0, sizeof(cl_mem), &dev_t1);
      dt_opencl_set_kernel_arg(devid, gd->kernel_color_smoothing, 1, sizeof(cl_mem), &dev_t2);
      dt_opencl_set_kernel_arg(devid, gd->kernel_color_smoothing, 2, sizeof(int), &width);
      dt_opencl_set_kernel_arg(devid, gd->kernel_color_smoothing, 3, sizeof(int), &height);
      dt_opencl_set_kernel_arg(devid, gd->kernel_color_smoothing, 4, (blockwd+2)*(blockht+2)*4*sizeof(float), NULL);
      err = dt_opencl_enqueue_kernel_2d_with_local(devid, gd->kernel_color_smoothing, sizes, local);
      if(err != CL_SUCCESS) goto error;

      // swap dev_t1 and dev_t2
      cl_mem t = dev_t1;
      dev_t1 = dev_t2;
      dev_t2 = t;
    }

    // after last step we find final output in dev_t1.
    // let's see if this is in dev_tmp and needs to be copied to dev_out
    if(dev_t1 == dev_tmp)
    {
      // copy data from dev_tmp -> dev_out
      err = dt_opencl_enqueue_copy_image(devid, dev_tmp, dev_out, origin, origin, region);
      if(err != CL_SUCCESS) goto error;
    }
  }

  if(dev_tmp != NULL) dt_opencl_release_mem_object(dev_tmp);
  return TRUE;

error:
  if (dev_tmp != NULL) dt_opencl_release_mem_object(dev_tmp);
  if (dev_green_eq != NULL) dt_opencl_release_mem_object(dev_green_eq);
  dt_print(DT_DEBUG_OPENCL, "[opencl_demosaic] couldn't enqueue kernel! %d\n", err);
  return FALSE;
}
#endif

void tiling_callback  (struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out, struct dt_develop_tiling_t *tiling)
{
  dt_iop_demosaic_data_t *data = (dt_iop_demosaic_data_t *)piece->data;

  const int qual = get_quality();
  const float ioratio = (float)roi_out->width*roi_out->height/((float)roi_in->width*roi_in->height);
  const float smooth = data->color_smoothing ? ioratio : 0.0f;

  tiling->factor = 1.0f + ioratio;

  if(roi_out->scale > 0.99999f && roi_out->scale < 1.00001f)
    tiling->factor += fmax(0.25f, smooth);
  else if(roi_out->scale > 0.5f ||
          (piece->pipe->type == DT_DEV_PIXELPIPE_FULL && qual > 0) || (piece->pipe->type == DT_DEV_PIXELPIPE_EXPORT))
    tiling->factor += fmax(1.25f, smooth);
  else
    tiling->factor += fmax(0.25f, smooth);

  tiling->maxbuf = 1.0f;
  tiling->overhead = 0;
  tiling->overlap = 5; // take care of border handling
  tiling->xalign = 2; // Bayer pattern
  tiling->yalign = 2; // Bayer pattern
  return;
}


void init(dt_iop_module_t *module)
{
  module->params = malloc(sizeof(dt_iop_demosaic_params_t));
  module->default_params = malloc(sizeof(dt_iop_demosaic_params_t));
  module->default_enabled = 1;
  module->priority = 122; // module order created by iop_dependencies.py, do not edit!
  module->hide_enable_button = 1;
  module->params_size = sizeof(dt_iop_demosaic_params_t);
  module->gui_data = NULL;
}

void init_global(dt_iop_module_so_t *module)
{
  const int program = 0; // from programs.conf
  dt_iop_demosaic_global_data_t *gd = (dt_iop_demosaic_global_data_t *)malloc(sizeof(dt_iop_demosaic_global_data_t));
  module->data = gd;
  gd->kernel_zoom_half_size     = dt_opencl_create_kernel(program, "clip_and_zoom_demosaic_half_size");
  gd->kernel_ppg_green          = dt_opencl_create_kernel(program, "ppg_demosaic_green");
  gd->kernel_green_eq           = dt_opencl_create_kernel(program, "green_equilibration");
  gd->kernel_pre_median         = dt_opencl_create_kernel(program, "pre_median");
  gd->kernel_ppg_green_median   = dt_opencl_create_kernel(program, "ppg_demosaic_green_median");
  gd->kernel_ppg_redblue        = dt_opencl_create_kernel(program, "ppg_demosaic_redblue");
  gd->kernel_downsample         = dt_opencl_create_kernel(program, "clip_and_zoom");
  gd->kernel_border_interpolate = dt_opencl_create_kernel(program, "border_interpolate");
  gd->kernel_color_smoothing    = dt_opencl_create_kernel(program, "color_smoothing");
}

void cleanup(dt_iop_module_t *module)
{
  free(module->gui_data);
  module->gui_data = NULL;
  free(module->params);
  module->params = NULL;
}

void cleanup_global(dt_iop_module_so_t *module)
{
  dt_iop_demosaic_global_data_t *gd = (dt_iop_demosaic_global_data_t *)module->data;
  dt_opencl_free_kernel(gd->kernel_zoom_half_size);
  dt_opencl_free_kernel(gd->kernel_ppg_green);
  dt_opencl_free_kernel(gd->kernel_pre_median);
  dt_opencl_free_kernel(gd->kernel_green_eq);
  dt_opencl_free_kernel(gd->kernel_ppg_green_median);
  dt_opencl_free_kernel(gd->kernel_ppg_redblue);
  dt_opencl_free_kernel(gd->kernel_downsample);
  dt_opencl_free_kernel(gd->kernel_border_interpolate);
  dt_opencl_free_kernel(gd->kernel_color_smoothing);
  free(module->data);
  module->data = NULL;
}

void commit_params (struct dt_iop_module_t *self, dt_iop_params_t *params, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_demosaic_params_t *p = (dt_iop_demosaic_params_t *)params;
  dt_iop_demosaic_data_t *d = (dt_iop_demosaic_data_t *)piece->data;
  d->filters = dt_image_filter(&pipe->image);
  if(!(pipe->image.flags & DT_IMAGE_RAW) || dt_dev_pixelpipe_uses_downsampled_input(pipe)) piece->enabled = 0;
  d->green_eq = p->green_eq;
  d->color_smoothing = p->color_smoothing;
  d->median_thrs = p->median_thrs;
  d->demosaicing_method = p->demosaicing_method;

  piece->process_cl_ready = 1;

  // x-trans images not implemented in OpenCL yet
  if(d->filters == 9)
    piece->process_cl_ready = 0;

  // Demosaic mode AMAZE not implemented in OpenCL yet.
  if(d->demosaicing_method == DT_IOP_DEMOSAIC_AMAZE)
    piece->process_cl_ready = 0;

  // OpenCL can not (yet) green-equilibrate over full image.
  if(d->green_eq == DT_IOP_GREEN_EQ_FULL || d->green_eq == DT_IOP_GREEN_EQ_BOTH)
    piece->process_cl_ready = 0;
}

void init_pipe     (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = malloc(sizeof(dt_iop_demosaic_data_t));
  self->commit_params(self, self->default_params, pipe, piece);
}

void cleanup_pipe  (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
  piece->data = NULL;
}

void gui_update   (struct dt_iop_module_t *self)
{
  dt_iop_demosaic_gui_data_t *g = (dt_iop_demosaic_gui_data_t *)self->gui_data;
  dt_iop_demosaic_params_t *p = (dt_iop_demosaic_params_t *)self->params;

  if(self->dev->image_storage.filters != 9)
  {
    gtk_widget_show(g->demosaic_method_bayer);
    gtk_widget_hide(g->demosaic_method_xtrans);
    gtk_widget_show(g->scale1);
    gtk_widget_show(g->greeneq);
    dt_bauhaus_combobox_set(g->demosaic_method_bayer, p->demosaicing_method);
  }
  else
  {
    gtk_widget_show(g->demosaic_method_xtrans);
    gtk_widget_hide(g->demosaic_method_bayer);
    gtk_widget_hide(g->scale1);
    gtk_widget_hide(g->greeneq);
    dt_bauhaus_combobox_set(g->demosaic_method_xtrans, p->demosaicing_method & ~DEMOSAIC_XTRANS);
  }

  dt_bauhaus_slider_set(g->scale1, p->median_thrs);
  dt_bauhaus_combobox_set(g->color_smoothing, p->color_smoothing);
  dt_bauhaus_combobox_set(g->greeneq, p->green_eq);
}

void reload_defaults(dt_iop_module_t *module)
{
  dt_iop_demosaic_params_t tmp = (dt_iop_demosaic_params_t)
  {
    0, 0.0f,0,0,0
  };
  if (module->dev->image_storage.filters == 9)
    tmp.demosaicing_method = DT_IOP_DEMOSAIC_VNG;
  memcpy(module->params, &tmp, sizeof(dt_iop_demosaic_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_demosaic_params_t));
}

static void
median_thrs_callback (GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(darktable.gui->reset) return;
  dt_iop_demosaic_params_t *p = (dt_iop_demosaic_params_t *)self->params;
  p->median_thrs = dt_bauhaus_slider_get(slider);
  if(p->median_thrs < 0.001f) p->median_thrs = 0.0f;
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void
color_smoothing_callback (GtkWidget *button, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(darktable.gui->reset) return;
  dt_iop_demosaic_params_t *p = (dt_iop_demosaic_params_t *)self->params;
  p->color_smoothing = dt_bauhaus_combobox_get(button);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void
greeneq_callback (GtkWidget *combo, dt_iop_module_t *self)
{
  dt_iop_demosaic_params_t *p = (dt_iop_demosaic_params_t *)self->params;
  int active = dt_bauhaus_combobox_get(combo);
  switch(active)
  {
    case DT_IOP_GREEN_EQ_FULL:
      p->green_eq = DT_IOP_GREEN_EQ_FULL;
      break;
    case DT_IOP_GREEN_EQ_LOCAL:
      p->green_eq = DT_IOP_GREEN_EQ_LOCAL;
      break;
    case DT_IOP_GREEN_EQ_BOTH:
      p->green_eq = DT_IOP_GREEN_EQ_BOTH;
      break;
    default:
    case DT_IOP_GREEN_EQ_NO:
      p->green_eq = DT_IOP_GREEN_EQ_NO;
      break;
  }
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void
demosaic_method_bayer_callback(GtkWidget *combo, dt_iop_module_t *self)
{
  dt_iop_demosaic_params_t *p = (dt_iop_demosaic_params_t *)self->params;
  int active = dt_bauhaus_combobox_get(combo);

  switch(active)
  {
    case DT_IOP_DEMOSAIC_AMAZE:
      p->demosaicing_method = DT_IOP_DEMOSAIC_AMAZE;
      break;
    default:
    case DT_IOP_DEMOSAIC_PPG:
      p->demosaicing_method = DT_IOP_DEMOSAIC_PPG;
      break;
  }
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void
demosaic_method_xtrans_callback(GtkWidget *combo, dt_iop_module_t *self)
{
  dt_iop_demosaic_params_t *p = (dt_iop_demosaic_params_t *)self->params;
  p->demosaicing_method = dt_bauhaus_combobox_get(combo)|DEMOSAIC_XTRANS;
  if ((p->demosaicing_method > DT_IOP_DEMOSAIC_MARKESTEIJN_3) || (p->demosaicing_method < DT_IOP_DEMOSAIC_LINEAR))
    p->demosaicing_method = DT_IOP_DEMOSAIC_VNG;
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

void gui_init     (struct dt_iop_module_t *self)
{
  self->gui_data = malloc(sizeof(dt_iop_demosaic_gui_data_t));
  dt_iop_demosaic_gui_data_t *g = (dt_iop_demosaic_gui_data_t *)self->gui_data;
  dt_iop_demosaic_params_t *p = (dt_iop_demosaic_params_t *)self->params;

  self->widget = gtk_vbox_new(TRUE, DT_BAUHAUS_SPACE);

  g->demosaic_method_bayer = dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(g->demosaic_method_bayer, NULL, _("method"));
  gtk_box_pack_start(GTK_BOX(self->widget), g->demosaic_method_bayer, TRUE, TRUE, 0);
  dt_bauhaus_combobox_add(g->demosaic_method_bayer, _("PPG (fast)"));
  dt_bauhaus_combobox_add(g->demosaic_method_bayer, _("amaze (slow)"));
  g_object_set(G_OBJECT(g->demosaic_method_bayer), "tooltip-text", _("demosaicing raw data method"), (char *)NULL);

  g->demosaic_method_xtrans = dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(g->demosaic_method_xtrans, NULL, _("method"));
  gtk_box_pack_start(GTK_BOX(self->widget), g->demosaic_method_xtrans, TRUE, TRUE, 0);
  dt_bauhaus_combobox_add(g->demosaic_method_xtrans, _("linear (fast)"));
  dt_bauhaus_combobox_add(g->demosaic_method_xtrans, _("VNG"));
  dt_bauhaus_combobox_add(g->demosaic_method_xtrans, _("Markesteijn 1-pass (slow)"));
  dt_bauhaus_combobox_add(g->demosaic_method_xtrans, _("Markesteijn 3-pass (slower)"));
  g_object_set(G_OBJECT(g->demosaic_method_xtrans), "tooltip-text", _("demosaicing raw data method"), (char *)NULL);

  g->scale1 = dt_bauhaus_slider_new_with_range(self, 0.0, 1.0, 0.001, p->median_thrs, 3);
  g_object_set(G_OBJECT(g->scale1), "tooltip-text", _("threshold for edge-aware median.\nset to 0.0 to switch off.\nset to 1.0 to ignore edges."), (char *)NULL);
  dt_bauhaus_widget_set_label(g->scale1, NULL, _("edge threshold"));
  gtk_box_pack_start(GTK_BOX(self->widget), g->scale1, TRUE, TRUE, 0);

  g->color_smoothing = dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(g->color_smoothing, NULL, _("color smoothing"));
  gtk_box_pack_start(GTK_BOX(self->widget), g->color_smoothing, TRUE, TRUE, 0);
  dt_bauhaus_combobox_add(g->color_smoothing, _("off"));
  dt_bauhaus_combobox_add(g->color_smoothing, _("one time"));
  dt_bauhaus_combobox_add(g->color_smoothing, _("two times"));
  dt_bauhaus_combobox_add(g->color_smoothing, _("three times"));
  dt_bauhaus_combobox_add(g->color_smoothing, _("four times"));
  dt_bauhaus_combobox_add(g->color_smoothing, _("five times"));
  g_object_set(G_OBJECT(g->color_smoothing), "tooltip-text", _("how many color smoothing median steps after demosaicing"), (char *)NULL);

  g->greeneq = dt_bauhaus_combobox_new(self);
  gtk_box_pack_start(GTK_BOX(self->widget), g->greeneq, TRUE, TRUE, 0);
  dt_bauhaus_widget_set_label(g->greeneq, NULL, _("match greens"));
  dt_bauhaus_combobox_add(g->greeneq, _("disabled"));
  dt_bauhaus_combobox_add(g->greeneq, _("local average"));
  dt_bauhaus_combobox_add(g->greeneq, _("full average"));
  dt_bauhaus_combobox_add(g->greeneq, _("full and local average"));
  g_object_set(G_OBJECT(g->greeneq), "tooltip-text", _("green channels matching method"), (char *)NULL);

  g_signal_connect (G_OBJECT (g->scale1), "value-changed",
                    G_CALLBACK (median_thrs_callback), self);
  g_signal_connect (G_OBJECT (g->color_smoothing), "value-changed",
                    G_CALLBACK (color_smoothing_callback), self);
  g_signal_connect (G_OBJECT (g->greeneq), "value-changed",
                    G_CALLBACK (greeneq_callback), self);
  g_signal_connect (G_OBJECT (g->demosaic_method_bayer), "value-changed",
                    G_CALLBACK (demosaic_method_bayer_callback), self);
  g_signal_connect (G_OBJECT (g->demosaic_method_xtrans), "value-changed",
                    G_CALLBACK (demosaic_method_xtrans_callback), self);
}

void gui_cleanup  (struct dt_iop_module_t *self)
{
  free(self->gui_data);
  self->gui_data = NULL;
}

#include "iop/amaze_demosaic_RT.cc"
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
