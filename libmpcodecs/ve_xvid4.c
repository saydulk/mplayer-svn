/*****************************************************************************
 *
 *  - XviD 1.0 export module for mplayer/mencoder -
 *
 *  Copyright(C) 2003 Marco Belli <elcabesa@inwind.it>
 *               2003 Edouard Gomez <ed.gomez@free.fr>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA
 *
 ****************************************************************************/

/*****************************************************************************
 * Includes
 ****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <math.h>
#include <limits.h>
#include <time.h>

#include "../config.h"
#include "../mp_msg.h"

#ifdef HAVE_XVID4

#include "codec-cfg.h"
#include "stream.h"
#include "demuxer.h"
#include "stheader.h"

#include "muxer.h"

#include "img_format.h"
#include "mp_image.h"
#include "vf.h"

#include <xvid.h>
#include <stdio.h>
#include <stdarg.h>
#include <limits.h>
#include <assert.h>

#include "m_option.h"

#define XVID_FIRST_PASS_FILENAME "xvid-twopass.stats"
#define FINE (!0)
#define BAD (!FINE)

// Code taken from Libavcodec and ve_lavc.c to handle Aspect Ratio calculation

typedef struct XVIDRational{
    int num; 
    int den;
} XVIDRational;

#define MAX(a,b) ((a) > (b) ? (a) : (b))
#define ABS(a) ((a) >= 0 ? (a) : (-(a)))


static int64_t xvid_gcd(int64_t a, int64_t b){
    if(b) return xvid_gcd(b, a%b);
    else  return a;
}

static int xvid_reduce(int *dst_nom, int *dst_den, int64_t nom, int64_t den, int64_t max){
    int exact=1, sign=0;
    int64_t gcd;

    assert(den != 0);

    if(den < 0){
        den= -den;
        nom= -nom;
    }
    
    if(nom < 0){
        nom= -nom;
        sign= 1;
    }
    
    gcd = xvid_gcd(nom, den);
    nom /= gcd;
    den /= gcd;
    
    if(nom > max || den > max){
        XVIDRational a0={0,1}, a1={1,0};
        exact=0;

        for(;;){
            int64_t x= nom / den;
            int64_t a2n= x*a1.num + a0.num;
            int64_t a2d= x*a1.den + a0.den;

            if(a2n > max || a2d > max) break;

            nom %= den;
        
            a0= a1;
            a1= (XVIDRational){a2n, a2d};
            if(nom==0) break;
            x= nom; nom=den; den=x;
        }
        nom= a1.num;
        den= a1.den;
    }
    
    assert(xvid_gcd(nom, den) == 1);
    
    if(sign) nom= -nom;
    
    *dst_nom = nom;
    *dst_den = den;
    
    return exact;
}


static XVIDRational xvid_d2q(double d, int max){
    XVIDRational a;
    int exponent= MAX( (int)(log(ABS(d) + 1e-20)/log(2)), 0);
    int64_t den= 1LL << (61 - exponent);
    xvid_reduce(&a.num, &a.den, (int64_t)(d * den + 0.5), den, max);

    return a;
}



/*****************************************************************************
 * Configuration options
 ****************************************************************************/

static int xvidenc_bitrate = 0;
static int xvidenc_pass = 0;
static float xvidenc_quantizer = 0;

static int xvidenc_packed = 0;
static int xvidenc_closed_gop = 1;
static int xvidenc_interlaced = 0;
static int xvidenc_quarterpel = 0;
static int xvidenc_gmc = 0;
static int xvidenc_trellis = 0;
static int xvidenc_cartoon = 0;
static int xvidenc_hqacpred = 1;
static int xvidenc_chromame = 0;
static int xvidenc_chroma_opt = 0;
static int xvidenc_vhq = 0;
static int xvidenc_motion = 6;
static int xvidenc_turbo = 0;
static int xvidenc_stats = 0;
static int xvidenc_max_key_interval = 0; /* Let xvidcore set a 10s interval by default */
static int xvidenc_frame_drop_ratio = 0;
static int xvidenc_greyscale = 0;
static int xvidenc_debug = 0;
static int xvidenc_psnr = 0;

static int xvidenc_max_bframes = 2;
static int xvidenc_bquant_ratio = 150;
static int xvidenc_bquant_offset = 100;
static int xvidenc_bframe_threshold = 0;

static int xvidenc_min_quant[3] = {2, 2, 2};
static int xvidenc_max_quant[3] = {31, 31, 31};
static char *xvidenc_intra_matrix_file = NULL;
static char *xvidenc_inter_matrix_file = NULL;
static char *xvidenc_quant_method = NULL;

static int xvidenc_cbr_reaction_delay_factor = 0;
static int xvidenc_cbr_averaging_period = 0;
static int xvidenc_cbr_buffer = 0;

static int xvidenc_vbr_keyframe_boost = 0;
static int xvidenc_vbr_overflow_control_strength = 5;
static int xvidenc_vbr_curve_compression_high = 0;
static int xvidenc_vbr_curve_compression_low = 0;
static int xvidenc_vbr_max_overflow_improvement = 5;
static int xvidenc_vbr_max_overflow_degradation = 5;
static int xvidenc_vbr_kfreduction = 0;
static int xvidenc_vbr_kfthreshold = 0;
static int xvidenc_vbr_container_frame_overhead = 24; /* mencoder uses AVI container */

static char *xvidenc_par = NULL;
static int xvidenc_par_width = 0;
static int xvidenc_par_height = 0;
static float xvidenc_dar_aspect = 0.0f;
static int xvidenc_autoaspect = 0;

m_option_t xvidencopts_conf[] =
{
	/* Standard things mencoder should be able to treat directly */
	{"bitrate", &xvidenc_bitrate, CONF_TYPE_INT, 0, 0, 0, NULL},
	{"pass", &xvidenc_pass, CONF_TYPE_INT, CONF_RANGE, 1, 2, NULL},
	{"fixed_quant", &xvidenc_quantizer, CONF_TYPE_FLOAT, CONF_RANGE, 1, 31, NULL},

	/* Features */
	{"quant_type", &xvidenc_quant_method, CONF_TYPE_STRING, 0, 0, 0, NULL},
	{"me_quality", &xvidenc_motion, CONF_TYPE_INT, CONF_RANGE, 0, 6, NULL},
	{"chroma_me", &xvidenc_chromame, CONF_TYPE_FLAG, 0, 0, 1, NULL},
	{"chroma_opt", &xvidenc_chroma_opt, CONF_TYPE_FLAG, 0, 0, 1, NULL},
	{"vhq", &xvidenc_vhq, CONF_TYPE_INT, CONF_RANGE, 0, 4, NULL},
	{"max_bframes", &xvidenc_max_bframes, CONF_TYPE_INT, CONF_RANGE, 0, 20, NULL},
	{"bquant_ratio", &xvidenc_bquant_ratio, CONF_TYPE_INT, CONF_RANGE, 0, 200, NULL},
	{"bquant_offset", &xvidenc_bquant_offset, CONF_TYPE_INT, CONF_RANGE, 0, 200, NULL},
	{"bf_threshold", &xvidenc_bframe_threshold, CONF_TYPE_INT, CONF_RANGE, -255, 255, NULL},
	{"qpel", &xvidenc_quarterpel, CONF_TYPE_FLAG, 0, 0, 1, NULL},
	{"gmc", &xvidenc_gmc, CONF_TYPE_FLAG, 0, 0, 1, NULL},
	{"trellis", &xvidenc_trellis, CONF_TYPE_FLAG, 0, 0, 1, NULL},
	{"packed", &xvidenc_packed, CONF_TYPE_FLAG, 0, 0, 1, NULL},
	{"closed_gop", &xvidenc_closed_gop, CONF_TYPE_FLAG, 0, 0, 1, NULL},
	{"interlacing", &xvidenc_interlaced, CONF_TYPE_FLAG, 0, 0, 1, NULL},
	{"cartoon", &xvidenc_cartoon, CONF_TYPE_FLAG, 0, 0, 1, NULL},
	{"hq_ac", &xvidenc_hqacpred, CONF_TYPE_FLAG, 0, 0, 1, NULL},
	{"frame_drop_ratio", &xvidenc_frame_drop_ratio, CONF_TYPE_INT, CONF_RANGE, 0, 100, NULL},
	{"max_key_interval", &xvidenc_max_key_interval, CONF_TYPE_INT, CONF_MIN, 0, 0, NULL},
	{"greyscale", &xvidenc_greyscale, CONF_TYPE_FLAG, 0, 0, 1, NULL},
	{"turbo", &xvidenc_turbo, CONF_TYPE_FLAG, 0, 0, 1, NULL},
	{"debug", &xvidenc_debug, CONF_TYPE_INT , 0 ,0,-1,NULL},
	{"stats", &xvidenc_stats, CONF_TYPE_FLAG, 0, 0, 1, NULL},
	{"psnr",  &xvidenc_psnr , CONF_TYPE_FLAG, 0, 0, 1, NULL},


	/* section [quantizer] */
	{"min_iquant", &xvidenc_min_quant[0], CONF_TYPE_INT, CONF_RANGE, 1, 31, NULL},
	{"max_iquant", &xvidenc_max_quant[0], CONF_TYPE_INT, CONF_RANGE, 1, 31, NULL},
	{"min_pquant", &xvidenc_min_quant[1], CONF_TYPE_INT, CONF_RANGE, 1, 31, NULL},
	{"max_pquant", &xvidenc_max_quant[1], CONF_TYPE_INT, CONF_RANGE, 1, 31, NULL},
	{"min_bquant", &xvidenc_min_quant[2], CONF_TYPE_INT, CONF_RANGE, 1, 31, NULL},
	{"max_bquant", &xvidenc_max_quant[2], CONF_TYPE_INT, CONF_RANGE, 1, 31, NULL},
	{"quant_intra_matrix", &xvidenc_intra_matrix_file, CONF_TYPE_STRING, 0, 0, 100, NULL},
	{"quant_inter_matrix", &xvidenc_inter_matrix_file, CONF_TYPE_STRING, 0, 0, 100, NULL},

	/* section [cbr] */
	{"rc_reaction_delay_factor", &xvidenc_cbr_reaction_delay_factor, CONF_TYPE_INT, CONF_RANGE, 0, 100, NULL},
	{"rc_averaging_period", &xvidenc_cbr_averaging_period, CONF_TYPE_INT, CONF_MIN, 0, 0, NULL},
	{"rc_buffer", &xvidenc_cbr_buffer, CONF_TYPE_INT, CONF_MIN, 0, 0, NULL},

	/* section [vbr] */
	{"keyframe_boost", &xvidenc_vbr_keyframe_boost, CONF_TYPE_INT, CONF_RANGE, 0, 100, NULL},
	{"curve_compression_high", &xvidenc_vbr_curve_compression_high, CONF_TYPE_INT, CONF_RANGE, 0, 100, NULL},
	{"curve_compression_low", &xvidenc_vbr_curve_compression_low, CONF_TYPE_INT, CONF_RANGE, 0, 100, NULL},
	{"overflow_control_strength", &xvidenc_vbr_overflow_control_strength, CONF_TYPE_INT, CONF_RANGE, 0, 100, NULL},
	{"max_overflow_improvement", &xvidenc_vbr_max_overflow_improvement, CONF_TYPE_INT, CONF_RANGE, 0, 100, NULL},
	{"max_overflow_degradation", &xvidenc_vbr_max_overflow_degradation, CONF_TYPE_INT, CONF_RANGE, 0, 100, NULL},
	{"kfreduction", &xvidenc_vbr_kfreduction, CONF_TYPE_INT, CONF_RANGE, 0, 100, NULL},
	{"kfthreshold", &xvidenc_vbr_kfthreshold, CONF_TYPE_INT, CONF_MIN, 0, 0, NULL},
	{"container_frame_overhead", &xvidenc_vbr_container_frame_overhead, CONF_TYPE_INT, CONF_MIN, 0, 0, NULL},

	/* Section Aspect Ratio */
	{"par", &xvidenc_par, CONF_TYPE_STRING, 0, 0, 0, NULL},
	{"par_width", &xvidenc_par_width, CONF_TYPE_INT, CONF_RANGE, 0, 255, NULL},
	{"par_height", &xvidenc_par_height, CONF_TYPE_INT, CONF_RANGE, 0, 255, NULL},
	{"aspect", &xvidenc_dar_aspect, CONF_TYPE_FLOAT, CONF_RANGE, 0.1, 9.99, NULL},
	{"autoaspect", &xvidenc_autoaspect, CONF_TYPE_FLAG, 0, 0, 1, NULL},

	/* End of the config array */
	{NULL, 0, 0, 0, 0, 0, NULL}
};

/*****************************************************************************
 * Module private data
 ****************************************************************************/

typedef struct _xvid_mplayer_module_t
{
	/* Instance related global vars */
	void *instance;
	xvid_gbl_init_t      init;
	xvid_enc_create_t    create;
	xvid_enc_frame_t     frame;
	xvid_plugin_single_t onepass;
	xvid_plugin_2pass1_t pass1;
	xvid_plugin_2pass2_t pass2;

	/* This data must survive local block scope, so here it is */
	xvid_enc_plugin_t    plugins[7];
	xvid_enc_zone_t      zones[1];

	/* MPEG4 stream buffer */
	muxer_stream_t *mux;

	/* Stats accumulators */
	int frames;
	long long sse_y;
	long long sse_u;
	long long sse_v;

	/* Min & Max PSNR */
	int min_sse_y;
	int min_sse_u;
	int min_sse_v;
	int min_framenum;
	int max_sse_y;
	int max_sse_u;
	int max_sse_v;
	int max_framenum;
	
	int pixels;
	int d_width, d_height;
} xvid_mplayer_module_t;

static void dispatch_settings(xvid_mplayer_module_t *mod);
static int set_create_struct(xvid_mplayer_module_t *mod);
static int set_frame_struct(xvid_mplayer_module_t *mod, mp_image_t *mpi);
static const char *errorstring(int err);

/*****************************************************************************
 * Video Filter API function definitions
 ****************************************************************************/

/*============================================================================
 * config
 *==========================================================================*/

static int
config(struct vf_instance_s* vf,
       int width, int height, int d_width, int d_height,
       unsigned int flags, unsigned int outfmt)
{
	int err;
	xvid_mplayer_module_t *mod = (xvid_mplayer_module_t *)vf->priv;
	
	/* Complete the muxer initialization */
	mod->mux->bih->biWidth = width;
	mod->mux->bih->biHeight = height;
	mod->mux->bih->biSizeImage = 
		mod->mux->bih->biWidth * mod->mux->bih->biHeight * 3;

	/* Message the FourCC type */
	mp_msg(MSGT_MENCODER, MSGL_INFO,
	       "videocodec: XviD (%dx%d fourcc=%x [%.4s])\n",
	       width, height, mod->mux->bih->biCompression,
	       (char *)&mod->mux->bih->biCompression);

	/*--------------------------------------------------------------------
	 * Dispatch all module settings to XviD structures
	 *------------------------------------------------------------------*/

	mod->d_width = d_width;
	mod->d_height = d_height;

	dispatch_settings(mod);

	/*--------------------------------------------------------------------
	 * Set remaining information in the xvid_enc_create_t structure
	 *------------------------------------------------------------------*/

	if(set_create_struct(mod) == BAD)
		return(BAD);

	/*--------------------------------------------------------------------
	 * Encoder instance creation
	 *------------------------------------------------------------------*/

	err = xvid_encore(NULL, XVID_ENC_CREATE, &mod->create, NULL);

	if(err<0) {
		mp_msg(MSGT_MENCODER, MSGL_ERR,
		       "xvid: xvidcore returned a '%s' error\n", errorstring(err));
		return(BAD);
	}
	
	/* Store the encoder instance into the private data */
	mod->instance = mod->create.handle;

	return(FINE);
}

/*============================================================================
 * uninit
 *==========================================================================*/

static void
uninit(struct vf_instance_s* vf)
{

	xvid_mplayer_module_t *mod = (xvid_mplayer_module_t *)vf->priv;

	/* Destroy xvid instance */
	xvid_encore(mod->instance, XVID_ENC_DESTROY, NULL, NULL);

	/* Display stats */
	if(mod->frames) {
		mod->sse_y /= mod->frames;
		mod->sse_u /= mod->frames;
		mod->sse_v /= mod->frames;

#define SSE2PSNR(sse, nbpixels) \
((!(sse)) ? 99.99f : 48.131f - 10*(double)log10((double)(sse)/(double)((nbpixels))))
		mp_msg(MSGT_MENCODER, MSGL_INFO,
		       "The value 99.99dB is a special value and represents "
		       "the upper range limit\n");
		mp_msg(MSGT_MENCODER, MSGL_INFO,
		       "xvid:     Min PSNR y : %.2f dB, u : %.2f dB, v : %.2f dB, in frame %d\n",
		       SSE2PSNR(mod->max_sse_y, mod->pixels),
		       SSE2PSNR(mod->max_sse_u, mod->pixels/4),
		       SSE2PSNR(mod->max_sse_v, mod->pixels/4),
		       mod->max_framenum);
		mp_msg(MSGT_MENCODER, MSGL_INFO,
		       "xvid: Average PSNR y : %.2f dB, u : %.2f dB, v : %.2f dB, for %d frames\n",
		       SSE2PSNR(mod->sse_y, mod->pixels),
		       SSE2PSNR(mod->sse_u, mod->pixels/4),
		       SSE2PSNR(mod->sse_v, mod->pixels/4),
		       mod->frames);
		mp_msg(MSGT_MENCODER, MSGL_INFO,
		       "xvid:     Max PSNR y : %.2f dB, u : %.2f dB, v : %.2f dB, in frame %d\n",
		       SSE2PSNR(mod->min_sse_y, mod->pixels),
		       SSE2PSNR(mod->min_sse_u, mod->pixels/4),
		       SSE2PSNR(mod->min_sse_v, mod->pixels/4),
		       mod->min_framenum);
	}

	/* ToDo: free matrices, and some string settings (quant method, matrix
	 * filenames...) */

	return;
}

/*============================================================================
 * control
 *==========================================================================*/

static int
control(struct vf_instance_s* vf, int request, void* data)
{
	return(CONTROL_UNKNOWN);
}

/*============================================================================
 * query_format
 *==========================================================================*/

static int
query_format(struct vf_instance_s* vf, unsigned int fmt)
{
	switch(fmt){
	case IMGFMT_YV12:
	case IMGFMT_IYUV:
	case IMGFMT_I420:
		return(VFCAP_CSP_SUPPORTED | VFCAP_CSP_SUPPORTED_BY_HW);
	case IMGFMT_YUY2:
	case IMGFMT_UYVY:
		return(VFCAP_CSP_SUPPORTED);
	}
	return(BAD);
}

/*============================================================================
 * put_image
 *==========================================================================*/

static int
put_image(struct vf_instance_s* vf, mp_image_t *mpi)
{
	int size;
	xvid_enc_stats_t stats; 
	xvid_mplayer_module_t *mod = (xvid_mplayer_module_t *)vf->priv;

	/* Prepare the stats */
	memset(&stats,0,sizeof( xvid_enc_stats_t));
	stats.version = XVID_VERSION;

	/* -------------------------------------------------------------------
	 * Set remaining information in the xvid_enc_frame_t structure
	 * NB: all the other struct members were initialized by
	 *     dispatch_settings
	 * -----------------------------------------------------------------*/

	if(set_frame_struct(mod, mpi) == BAD)
		return(BAD);

	/* -------------------------------------------------------------------
	 * Encode the frame
	 * ---------------------------------------------------------------- */

	size = xvid_encore(mod->instance, XVID_ENC_ENCODE, &mod->frame, &stats);

	/* Analyse the returned value */
	if(size<0) {
		mp_msg(MSGT_MENCODER, MSGL_ERR,
		       "xvid: xvidcore returned a '%s' error\n", errorstring(size));
		return(BAD);
	}

	/* If size is == 0, we're done with that frame */
	if(size == 0) return(FINE);

	/* Did xvidcore returned stats about an encoded frame ? (asynchronous) */
	if(xvidenc_stats && stats.type > 0) {
		mod->sse_y += stats.sse_y;
		mod->sse_u += stats.sse_u;
		mod->sse_v += stats.sse_v;

		if(mod->min_sse_y > stats.sse_y) {
			mod->min_sse_y = stats.sse_y;
			mod->min_sse_u = stats.sse_u;
			mod->min_sse_v = stats.sse_v;
			mod->min_framenum = mod->frames;
		}

		if(mod->max_sse_y < stats.sse_y) {
			mod->max_sse_y = stats.sse_y;
			mod->max_sse_u = stats.sse_u;
			mod->max_sse_v = stats.sse_v;
			mod->max_framenum = mod->frames;
		}
		if (xvidenc_psnr) {
                    static FILE *fvstats = NULL;
                    char filename[20];

                    if (!fvstats) {
                        time_t today2;
                        struct tm *today;
                        today2 = time (NULL);
                        today = localtime (&today2);
                        sprintf (filename, "psnr_%02d%02d%02d.log", today->tm_hour, today->tm_min, today->tm_sec);
                        fvstats = fopen (filename,"w");
                        if (!fvstats) {
                            perror ("fopen");
                            xvidenc_psnr = 0; // disable block
                        }
                    }
                    fprintf (fvstats, "%6d, %2d, %6d, %2.2f, %2.2f, %2.2f, %2.2f %c\n",
                             mod->frames,
                             stats.quant,
                             stats.length,
                             SSE2PSNR (stats.sse_y, mod->pixels),
                             SSE2PSNR (stats.sse_u, mod->pixels / 4),
                             SSE2PSNR (stats.sse_v, mod->pixels / 4),
                             SSE2PSNR (stats.sse_y + stats.sse_u + stats.sse_v,(double)mod->pixels * 1.5),
                             stats.type==1?'I':stats.type==2?'P':stats.type==3?'B':stats.type?'S':'?'
                             );
		}
		mod->frames++;

	}
#undef SSE2PSNR

	/* xvidcore outputed bitstream -- mux it */
	muxer_write_chunk(mod->mux,
			  size,
			  (mod->frame.out_flags & XVID_KEYFRAME)?0x10:0);

	return(FINE);
}

/*============================================================================
 * vf_open
 *==========================================================================*/

static int
vf_open(vf_instance_t *vf, char* args)
{
	xvid_mplayer_module_t *mod;
	xvid_gbl_init_t xvid_gbl_init;
	xvid_gbl_info_t xvid_gbl_info;

	/* Setting libmpcodec module API pointers */
	vf->config       = config;
	vf->control      = control;
	vf->uninit       = uninit;
	vf->query_format = query_format;
	vf->put_image    = put_image;

	/* Allocate the private part of the codec module */
	vf->priv = malloc(sizeof(xvid_mplayer_module_t));
	mod = (xvid_mplayer_module_t*)vf->priv;

	if(mod == NULL) {
		mp_msg(MSGT_MENCODER,MSGL_ERR,
		       "xvid: memory allocation failure (private data)\n");
		return(BAD);
	}

	/* Initialize the module to zeros */
	memset(mod, 0, sizeof(xvid_mplayer_module_t));
	mod->min_sse_y = mod->min_sse_u = mod->min_sse_v = INT_MAX;
	mod->max_sse_y = mod->max_sse_u = mod->max_sse_v = INT_MIN;

	/* Bind the Muxer */
	mod->mux = (muxer_stream_t*)args;

	/* Initialize muxer BITMAP header */
	mod->mux->bih = malloc(sizeof(BITMAPINFOHEADER));

	if(mod->mux->bih  == NULL) {
		mp_msg(MSGT_MENCODER,MSGL_ERR,
		       "xvid: memory allocation failure (BITMAP header)\n");
		return(BAD);
	}

	mod->mux->bih->biSize = sizeof(BITMAPINFOHEADER);
	mod->mux->bih->biWidth = 0;
	mod->mux->bih->biHeight = 0;
	mod->mux->bih->biPlanes = 1;
	mod->mux->bih->biBitCount = 24;
	mod->mux->bih->biCompression = mmioFOURCC('X','V','I','D');

	/* Retrieve information about the host XviD library */
	memset(&xvid_gbl_info, 0, sizeof(xvid_gbl_info_t));
	xvid_gbl_info.version = XVID_VERSION;

	if (xvid_global(NULL, XVID_GBL_INFO, &xvid_gbl_info, NULL) < 0) {
		mp_msg(MSGT_MENCODER,MSGL_INFO, "xvid: could not get information about the library\n");
	} else {
		mp_msg(MSGT_MENCODER,MSGL_INFO, "xvid: using library version %d.%d.%d (build %s)\n",
		       XVID_VERSION_MAJOR(xvid_gbl_info.actual_version),
		       XVID_VERSION_MINOR(xvid_gbl_info.actual_version),
		       XVID_VERSION_PATCH(xvid_gbl_info.actual_version),
		       xvid_gbl_info.build);
	}
		
	/* Initialize the xvid_gbl_init structure */
	memset(&xvid_gbl_init, 0, sizeof(xvid_gbl_init_t));
	xvid_gbl_init.version = XVID_VERSION;
	xvid_gbl_init.debug = xvidenc_debug;

	/* Initialize the xvidcore library */
	if (xvid_global(NULL, XVID_GBL_INIT, &xvid_gbl_init, NULL) < 0) {
		mp_msg(MSGT_MENCODER,MSGL_ERR, "xvid: initialisation failure\n");
		return(BAD);
	}

	return(FINE);
}

/*****************************************************************************
 * Helper functions
 ****************************************************************************/

static void *read_matrix(unsigned char *filename);

static void dispatch_settings(xvid_mplayer_module_t *mod)
{
	xvid_enc_create_t *create     = &mod->create;
	xvid_enc_frame_t  *frame      = &mod->frame;
	xvid_plugin_single_t *onepass = &mod->onepass;
	xvid_plugin_2pass2_t *pass2   = &mod->pass2;
	XVIDRational ar;

	const int motion_presets[7] =
		{
			0,
			0,
			0,
			0,
			XVID_ME_HALFPELREFINE16,
			XVID_ME_HALFPELREFINE16 | XVID_ME_ADVANCEDDIAMOND16,
			XVID_ME_HALFPELREFINE16 | XVID_ME_EXTSEARCH16 |
			XVID_ME_HALFPELREFINE8  | XVID_ME_USESQUARES16
		};

	
	/* -------------------------------------------------------------------
	 * Dispatch all settings having an impact on the "create" structure
	 * This includes plugins as they are passed to encore through the
	 * create structure
	 * -----------------------------------------------------------------*/

	/* -------------------------------------------------------------------
	 * The create structure
	 * ---------------------------------------------------------------- */

	create->global = 0;

	if(xvidenc_packed)
		create->global |= XVID_GLOBAL_PACKED;

	if(xvidenc_closed_gop)
		create->global |= XVID_GLOBAL_CLOSED_GOP;

        if(xvidenc_psnr)
	    xvidenc_stats = 1;

	if(xvidenc_stats)
		create->global |= XVID_GLOBAL_EXTRASTATS_ENABLE;

	create->num_zones = 0;
	create->zones = NULL;
	create->num_plugins = 0;
	create->plugins = NULL;
	create->num_threads = 0;
	create->max_bframes = xvidenc_max_bframes;
	create->bquant_ratio = xvidenc_bquant_ratio;
	create->bquant_offset = xvidenc_bquant_offset;
	create->max_key_interval = xvidenc_max_key_interval;
	create->frame_drop_ratio = xvidenc_frame_drop_ratio;
	create->min_quant[0] = xvidenc_min_quant[0];
	create->min_quant[1] = xvidenc_min_quant[1];
	create->min_quant[2] = xvidenc_min_quant[2];
	create->max_quant[0] = xvidenc_max_quant[0];
	create->max_quant[1] = xvidenc_max_quant[1];
	create->max_quant[2] = xvidenc_max_quant[2];


	/* -------------------------------------------------------------------
	 * The single pass plugin
	 * ---------------------------------------------------------------- */

	onepass->bitrate = xvidenc_bitrate;
	onepass->reaction_delay_factor = xvidenc_cbr_reaction_delay_factor;
	onepass->averaging_period = xvidenc_cbr_averaging_period;
	onepass->buffer = xvidenc_cbr_buffer;

	/* -------------------------------------------------------------------
	 * The pass2 plugin
	 * ---------------------------------------------------------------- */

	pass2->keyframe_boost = xvidenc_vbr_keyframe_boost;
	pass2->overflow_control_strength = xvidenc_vbr_overflow_control_strength;
	pass2->curve_compression_high = xvidenc_vbr_curve_compression_high;
	pass2->curve_compression_low = xvidenc_vbr_curve_compression_low;
	pass2->max_overflow_improvement = xvidenc_vbr_max_overflow_improvement;
	pass2->max_overflow_degradation = xvidenc_vbr_max_overflow_degradation;
	pass2->kfreduction = xvidenc_vbr_kfreduction;
	pass2->kfthreshold = xvidenc_vbr_kfthreshold;
	pass2->container_frame_overhead = xvidenc_vbr_container_frame_overhead;

	/* -------------------------------------------------------------------
	 * The frame structure
	 * ---------------------------------------------------------------- */
	frame->vol_flags = 0;
	frame->vop_flags = 0;
	frame->motion    = 0;

	frame->vop_flags |= XVID_VOP_HALFPEL;
	frame->motion    |= motion_presets[xvidenc_motion];

	if(xvidenc_stats)
		frame->vol_flags |= XVID_VOL_EXTRASTATS;

	if(xvidenc_greyscale)
		frame->vop_flags |= XVID_VOP_GREYSCALE;

	if(xvidenc_cartoon) {
		frame->vop_flags |= XVID_VOP_CARTOON;
		frame->motion |= XVID_ME_DETECT_STATIC_MOTION;
	}

	if(xvidenc_intra_matrix_file != NULL) {
		frame->quant_intra_matrix = (unsigned char*)read_matrix(xvidenc_intra_matrix_file);
		if(frame->quant_intra_matrix != NULL) {
			fprintf(stderr, "xvid: Loaded Intra matrix (switching to mpeg quantization type)\n");
			if(xvidenc_quant_method) free(xvidenc_quant_method);
			xvidenc_quant_method = strdup("mpeg");
		}
	}
	if(xvidenc_inter_matrix_file != NULL) {
		frame->quant_inter_matrix = read_matrix(xvidenc_inter_matrix_file);
		if(frame->quant_inter_matrix) {
			fprintf(stderr, "\nxvid: Loaded Inter matrix (switching to mpeg quantization type)\n");
			if(xvidenc_quant_method) free(xvidenc_quant_method);
			xvidenc_quant_method = strdup("mpeg");
		}
	}
	if(xvidenc_quant_method != NULL && !strcasecmp(xvidenc_quant_method, "mpeg")) {
		frame->vol_flags |= XVID_VOL_MPEGQUANT;
	}
	if(xvidenc_quarterpel) {
		frame->vol_flags |= XVID_VOL_QUARTERPEL;
		frame->motion    |= XVID_ME_QUARTERPELREFINE16;
		frame->motion    |= XVID_ME_QUARTERPELREFINE8;
	}
	if(xvidenc_gmc) {
		frame->vol_flags |= XVID_VOL_GMC;
		frame->motion    |= XVID_ME_GME_REFINE;
	}
	if(xvidenc_interlaced) {
		frame->vol_flags |= XVID_VOL_INTERLACING;
	}
	if(xvidenc_trellis) {
		frame->vop_flags |= XVID_VOP_TRELLISQUANT;
	}
	if(xvidenc_hqacpred) {
		frame->vop_flags |= XVID_VOP_HQACPRED;
	}
	if(xvidenc_chroma_opt) {
		frame->vop_flags |= XVID_VOP_CHROMAOPT;
	}
	if(xvidenc_motion > 4) {
		frame->vop_flags |= XVID_VOP_INTER4V;
	}
	if(xvidenc_chromame) {
		frame->motion |= XVID_ME_CHROMA_PVOP;
		frame->motion |= XVID_ME_CHROMA_BVOP;
	}
	if(xvidenc_vhq >= 1) {
		frame->vop_flags |= XVID_VOP_MODEDECISION_RD;
	}
	if(xvidenc_vhq >= 2) {
		frame->motion |= XVID_ME_HALFPELREFINE16_RD;
		frame->motion |= XVID_ME_QUARTERPELREFINE16_RD;
	}
	if(xvidenc_vhq >= 3) {
		frame->motion |= XVID_ME_HALFPELREFINE8_RD;
		frame->motion |= XVID_ME_QUARTERPELREFINE8_RD;
		frame->motion |= XVID_ME_CHECKPREDICTION_RD;
	}
	if(xvidenc_vhq >= 4) {
		frame->motion |= XVID_ME_EXTSEARCH_RD;
	}
	if(xvidenc_turbo) {
		frame->motion |= XVID_ME_FASTREFINE16;
		frame->motion |= XVID_ME_FASTREFINE8;
		frame->motion |= XVID_ME_SKIP_DELTASEARCH;
		frame->motion |= XVID_ME_FAST_MODEINTERPOLATE;
		frame->motion |= XVID_ME_BFRAME_EARLYSTOP;
	}

	/* motion level == 0 means no motion search which is equivalent to
	 * intra coding only */
	if(xvidenc_motion == 0) {
		frame->type = XVID_TYPE_IVOP;
	} else {
		frame->type = XVID_TYPE_AUTO;
	}

	frame->bframe_threshold = xvidenc_bframe_threshold;

	/* PAR related initialization */
	frame->par = XVID_PAR_11_VGA; /* Default */

	if(xvidenc_dar_aspect > 0) 
	    ar = xvid_d2q(xvidenc_dar_aspect * mod->mux->bih->biHeight / mod->mux->bih->biWidth, 255);
	else if(xvidenc_autoaspect)
	    ar = xvid_d2q((float)mod->d_width / mod->d_height * mod->mux->bih->biHeight / mod->mux->bih->biWidth, 255);
	else ar.num = ar.den = 0;
	
	if(ar.den != 0) {
		if(ar.num == 12 && ar.den == 11)
		    frame->par = XVID_PAR_43_PAL;
		else if(ar.num == 10 && ar.den == 11)
		    frame->par = XVID_PAR_43_NTSC;
		else if(ar.num == 16 && ar.den == 11)
		    frame->par = XVID_PAR_169_PAL;
		else if(ar.num == 40 && ar.den == 33)
		    frame->par = XVID_PAR_169_NTSC;
		else
		{    
		    frame->par = XVID_PAR_EXT;
		    frame->par_width = ar.num;
		    frame->par_height= ar.den;
		}
			
		mp_msg(MSGT_MENCODER, MSGL_INFO, "XVID4_DAR: %d/%d code %d, Display frame: (%d, %d), original frame: (%d, %d)\n", 
	    	    ar.num, ar.den, frame->par,
		    mod->d_width, mod->d_height, mod->mux->bih->biWidth, mod->mux->bih->biHeight);
	} else if(xvidenc_par != NULL) {
		if(strcasecmp(xvidenc_par, "pal43") == 0)
			frame->par = XVID_PAR_43_PAL;
		else if(strcasecmp(xvidenc_par, "pal169") == 0)
			frame->par = XVID_PAR_169_PAL;
		else if(strcasecmp(xvidenc_par, "ntsc43") == 0)
			frame->par = XVID_PAR_43_NTSC;
		else if(strcasecmp(xvidenc_par, "ntsc169") == 0)
			frame->par = XVID_PAR_169_NTSC;
		else if(strcasecmp(xvidenc_par, "ext") == 0)
			frame->par = XVID_PAR_EXT;

	if(frame->par == XVID_PAR_EXT) {
		if(xvidenc_par_width)
			frame->par_width = xvidenc_par_width;
		else
			frame->par_width = 1;

		if(xvidenc_par_height)
			frame->par_height = xvidenc_par_height;
		else
			frame->par_height = 1;
	}
	}
	return;
}

static int set_create_struct(xvid_mplayer_module_t *mod)
{
	int pass;
	xvid_enc_create_t *create    = &mod->create;

	/* Most of the structure is initialized by dispatch settings, only a
	 * few things are missing  */
	create->version = XVID_VERSION;

	/* Width and Height */
	create->width  = mod->mux->bih->biWidth;
	create->height = mod->mux->bih->biHeight;

	/* Pixels are needed for PSNR calculations */
	mod->pixels = create->width * create->height;

	/* FPS */
	create->fincr = mod->mux->h.dwScale;
	create->fbase = mod->mux->h.dwRate;

	/* Encodings zones */
	memset(mod->zones, 0, sizeof(mod->zones));
	create->zones     = mod->zones;
	create->num_zones = 0;

	/* Plugins */
	memset(mod->plugins, 0, sizeof(mod->plugins));
	create->plugins     = mod->plugins;
	create->num_plugins = 0;

	/* -------------------------------------------------------------------
	 * Initialize and bind the right rate controller plugin
	 * ---------------------------------------------------------------- */

	/* First we try to sort out configuration conflicts */
	if(xvidenc_quantizer != 0 && (xvidenc_bitrate || xvidenc_pass)) {
		mp_msg(MSGT_MENCODER, MSGL_ERR,
		       "xvid: you can't mix Fixed Quantizer Rate Control"
		       " with other Rate Control mechanisms\n");
		return(BAD);
	}

	if(xvidenc_bitrate != 0 && xvidenc_pass == 1) {
		mp_msg(MSGT_MENCODER, MSGL_ERR,
		       "xvid: bitrate setting is ignored during first pass\n");
	}

	/* Sort out which sort of pass we are supposed to do
	 * pass == 1<<0 CBR
	 * pass == 1<<1 Two pass first pass
	 * pass == 1<<2 Two pass second pass
	 * pass == 1<<3 Constant quantizer
	 */
#define MODE_CBR    (1<<0)
#define MODE_2PASS1 (1<<1)
#define MODE_2PASS2 (1<<2)
#define MODE_QUANT  (1<<3)

	pass = 0;

	if(xvidenc_bitrate != 0 && xvidenc_pass == 0)
		pass |= MODE_CBR;

	if(xvidenc_pass == 1)
		pass |= MODE_2PASS1;

	if(xvidenc_bitrate != 0 && xvidenc_pass == 2)
		pass |= MODE_2PASS2;

	if(xvidenc_quantizer != 0  && xvidenc_pass == 0)
		pass |= MODE_QUANT;

	/* We must be in at least one RC mode */
	if(pass == 0) {
		mp_msg(MSGT_MENCODER, MSGL_ERR,
		       "xvid: you must specify one or a valid combination of "
		       "'bitrate', 'pass', 'quantizer' settings\n");
		return(BAD);
	}

	/* Sanity checking */
	if(pass != MODE_CBR    && pass != MODE_QUANT &&
	   pass != MODE_2PASS1 && pass != MODE_2PASS2) {
		mp_msg(MSGT_MENCODER, MSGL_ERR,
		       "xvid: this code should not be reached - fill a bug "
		       "report\n");
		return(BAD);
	}

	/* This is a single pass encoding: either a CBR pass or a constant
	 * quantizer pass */
	if(pass == MODE_CBR  || pass == MODE_QUANT) {
		xvid_plugin_single_t *onepass = &mod->onepass;

		/* There is not much left to initialize after dispatch settings */
		onepass->version = XVID_VERSION;
		onepass->bitrate = xvidenc_bitrate*1000;

		/* Quantizer mode uses the same plugin, we have only to define
		 * a constant quantizer zone beginning at frame 0 */
		if(pass == MODE_QUANT) {
			int base, incr;

			base = 100;
			incr = (int)xvidenc_quantizer*base;

			create->zones[create->num_zones].mode      = XVID_ZONE_QUANT;
			create->zones[create->num_zones].frame     = 0;
			create->zones[create->num_zones].base      = base;
			create->zones[create->num_zones].increment = incr;
			create->num_zones++;

			mp_msg(MSGT_MENCODER, MSGL_INFO,
			       "xvid: Fixed Quant Rate Control -- quantizer=%d/%d=%2.2f\n",
			       incr,
			       base,
			       (float)(incr)/(float)(base));
			
		} else {
			mp_msg(MSGT_MENCODER, MSGL_INFO,
			       "xvid: CBR Rate Control -- bitrate=%dkbit/s\n",
			       xvidenc_bitrate);
		}

		create->plugins[create->num_plugins].func  = xvid_plugin_single;
		create->plugins[create->num_plugins].param = onepass;
		create->num_plugins++;
	}

	/* This is the first pass of a Two pass process */
	if(pass == MODE_2PASS1) {
		xvid_plugin_2pass1_t *pass1 = &mod->pass1;

		/* There is not much to initialize for this plugin */
		pass1->version  = XVID_VERSION;
		pass1->filename = XVID_FIRST_PASS_FILENAME;

		create->plugins[create->num_plugins].func  = xvid_plugin_2pass1;
		create->plugins[create->num_plugins].param = pass1;
		create->num_plugins++;

		mp_msg(MSGT_MENCODER, MSGL_INFO,
		       "xvid: 2Pass Rate Control -- 1st pass\n");
	}

	/* This is the second pass of a Two pass process */
	if(pass == MODE_2PASS2) {
		xvid_plugin_2pass2_t *pass2 = &mod->pass2;

		/* There is not much left to initialize after dispatch settings */
		pass2->version  = XVID_VERSION;
		pass2->filename =  XVID_FIRST_PASS_FILENAME;

		/* Positive bitrate values are bitrates as usual but if the
		 * value is negative it is considered as being a total size
		 * to reach (in kilobytes) */
		if(xvidenc_bitrate > 0) {
			pass2->bitrate  = xvidenc_bitrate*1000;
			mp_msg(MSGT_MENCODER, MSGL_INFO,
			       "xvid: 2Pass Rate Control -- 2nd pass -- bitrate=%dkbit/s\n",
			       xvidenc_bitrate);
		} else {
			pass2->bitrate  = xvidenc_bitrate;
			mp_msg(MSGT_MENCODER, MSGL_INFO,
			       "xvid: 2Pass Rate Control -- 2nd pass -- total size=%dkB\n",
			       -xvidenc_bitrate);
		}

		create->plugins[create->num_plugins].func  = xvid_plugin_2pass2;
		create->plugins[create->num_plugins].param = pass2;
		create->num_plugins++;
	}

	return(FINE);
}

static int set_frame_struct(xvid_mplayer_module_t *mod, mp_image_t *mpi)
{
	xvid_enc_frame_t *frame = &mod->frame;

	/* Most of the initialization is done during dispatch_settings */
	frame->version = XVID_VERSION;

	/* Bind output buffer */
	frame->bitstream = mod->mux->buffer;
	frame->length    = -1;

	/* Frame format */
	switch(mpi->imgfmt) {
	case IMGFMT_YV12:
	case IMGFMT_IYUV:
	case IMGFMT_I420:
		frame->input.csp = XVID_CSP_USER;
		break;
	case IMGFMT_YUY2:
		frame->input.csp = XVID_CSP_YUY2;
		break;
	case IMGFMT_UYVY:
		frame->input.csp = XVID_CSP_UYVY;
		break;
	default:
		mp_msg(MSGT_MENCODER, MSGL_ERR,
		       "xvid: unsupported picture format (%s)!\n",
		       vo_format_name(mpi->imgfmt));
		return(BAD);
	}

	/* Bind source frame */
	frame->input.plane[0]  = mpi->planes[0];
	frame->input.plane[1]  = mpi->planes[1];
	frame->input.plane[2]  = mpi->planes[2];
	frame->input.stride[0] = mpi->stride[0];
	frame->input.stride[1] = mpi->stride[1];
	frame->input.stride[2] = mpi->stride[2];

	/* Force the right quantizer -- It is internally managed by RC
	 * plugins */
	frame->quant = 0;

	return(FINE);
}

static void *read_matrix(unsigned char *filename)
{
	int i;
	unsigned char *matrix;
	FILE *input;
	
	/* Allocate matrix space */
	if((matrix = malloc(64*sizeof(unsigned char))) == NULL)
	   return(NULL);

	/* Open the matrix file */
	if((input = fopen(filename, "rb")) == NULL) {
		fprintf(stderr,
			"xvid: Error opening the matrix file %s\n",
			filename);
		free(matrix);
		return(NULL);
	}

	/* Read the matrix */
	for(i=0; i<64; i++) {

		int value;

		/* If fscanf fails then get out of the loop */
		if(fscanf(input, "%d", &value) != 1) {
			fprintf(stderr,
				"xvid: Error reading the matrix file %s\n",
				filename);
			free(matrix);
			fclose(input);
			return(NULL);
		}

		/* Clamp the value to safe range */
		value     = (value<  1)?1  :value;
		value     = (value>255)?255:value;
		matrix[i] = value;
	}

	/* Fills the rest with 1 */
	while(i<64) matrix[i++] = 1;

	/* We're done */
	fclose(input);

	return(matrix);
	
}

static const char *errorstring(int err)
{
	char *error;
	switch(err) {
	case XVID_ERR_FAIL:
		error = "General fault";
		break;
	case XVID_ERR_MEMORY:
		error =  "Memory allocation error";
		break;
	case XVID_ERR_FORMAT:
		error =  "File format error";
		break;
	case XVID_ERR_VERSION:
		error =  "Structure version not supported";
		break;
	case XVID_ERR_END:
		error =  "End of stream reached";
		break;
	default:
		error = "Unknown";
	}

	return((const char *)error);
}

/*****************************************************************************
 * Module structure definition
 ****************************************************************************/

vf_info_t ve_info_xvid = {
	"XviD 1.0 encoder",
	"xvid",
	"Marco Belli <elcabesa@inwind.it>, Edouard Gomez <ed.gomez@free.fr>",
	"No comment",
	vf_open
};


#endif /* HAVE_XVID4 */

/* Please do not change that tag comment.
 * arch-tag: 42ccc257-0548-4a3e-9617-2876c4e8ac88 mplayer xvid encoder module */
