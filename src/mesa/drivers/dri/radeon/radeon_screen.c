/**************************************************************************

Copyright 2000, 2001 ATI Technologies Inc., Ontario, Canada, and
                     VA Linux Systems Inc., Fremont, California.

All Rights Reserved.

Permission is hereby granted, free of charge, to any person obtaining
a copy of this software and associated documentation files (the
"Software"), to deal in the Software without restriction, including
without limitation the rights to use, copy, modify, merge, publish,
distribute, sublicense, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to
the following conditions:

The above copyright notice and this permission notice (including the
next paragraph) shall be included in all copies or substantial
portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
IN NO EVENT SHALL THE COPYRIGHT OWNER(S) AND/OR ITS SUPPLIERS BE
LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

**************************************************************************/

/**
 * \file radeon_screen.c
 * Screen initialization functions for the Radeon driver.
 *
 * \author Kevin E. Martin <martin@valinux.com>
 * \author  Gareth Hughes <gareth@valinux.com>
 */

#include "main/glheader.h"
#include "main/imports.h"
#include "main/mtypes.h"
#include "main/framebuffer.h"
#include "main/renderbuffer.h"

#define STANDALONE_MMIO
#include "radeon_chipset.h"
#include "radeon_macros.h"
#include "radeon_screen.h"
#if !RADEON_COMMON
#include "radeon_context.h"
#include "radeon_span.h"
#include "radeon_tex.h"
#elif RADEON_COMMON && defined(RADEON_COMMON_FOR_R200)
#include "r200_context.h"
#include "r200_ioctl.h"
#include "r200_span.h"
#include "r200_tex.h"
#elif RADEON_COMMON && defined(RADEON_COMMON_FOR_R300)
#include "r300_context.h"
#include "r300_fragprog.h"
#include "r300_tex.h"
#include "radeon_span.h"
#endif

#include "utils.h"
#include "vblank.h"
#include "drirenderbuffer.h"

#include "GL/internal/dri_interface.h"

/* Radeon configuration
 */
#include "xmlpool.h"

#if !RADEON_COMMON	/* R100 */
PUBLIC const char __driConfigOptions[] =
DRI_CONF_BEGIN
    DRI_CONF_SECTION_PERFORMANCE
        DRI_CONF_TCL_MODE(DRI_CONF_TCL_CODEGEN)
        DRI_CONF_FTHROTTLE_MODE(DRI_CONF_FTHROTTLE_IRQS)
        DRI_CONF_VBLANK_MODE(DRI_CONF_VBLANK_DEF_INTERVAL_0)
        DRI_CONF_MAX_TEXTURE_UNITS(3,2,3)
        DRI_CONF_HYPERZ(false)
    DRI_CONF_SECTION_END
    DRI_CONF_SECTION_QUALITY
        DRI_CONF_TEXTURE_DEPTH(DRI_CONF_TEXTURE_DEPTH_FB)
        DRI_CONF_DEF_MAX_ANISOTROPY(1.0,"1.0,2.0,4.0,8.0,16.0")
        DRI_CONF_NO_NEG_LOD_BIAS(false)
        DRI_CONF_FORCE_S3TC_ENABLE(false)
        DRI_CONF_COLOR_REDUCTION(DRI_CONF_COLOR_REDUCTION_DITHER)
        DRI_CONF_ROUND_MODE(DRI_CONF_ROUND_TRUNC)
        DRI_CONF_DITHER_MODE(DRI_CONF_DITHER_XERRORDIFF)
        DRI_CONF_ALLOW_LARGE_TEXTURES(2)
    DRI_CONF_SECTION_END
    DRI_CONF_SECTION_DEBUG
        DRI_CONF_NO_RAST(false)
    DRI_CONF_SECTION_END
DRI_CONF_END;
static const GLuint __driNConfigOptions = 14;

#elif RADEON_COMMON && defined(RADEON_COMMON_FOR_R200)

PUBLIC const char __driConfigOptions[] =
DRI_CONF_BEGIN
    DRI_CONF_SECTION_PERFORMANCE
        DRI_CONF_TCL_MODE(DRI_CONF_TCL_CODEGEN)
        DRI_CONF_FTHROTTLE_MODE(DRI_CONF_FTHROTTLE_IRQS)
        DRI_CONF_VBLANK_MODE(DRI_CONF_VBLANK_DEF_INTERVAL_0)
        DRI_CONF_MAX_TEXTURE_UNITS(6,2,6)
        DRI_CONF_HYPERZ(false)
    DRI_CONF_SECTION_END
    DRI_CONF_SECTION_QUALITY
        DRI_CONF_TEXTURE_DEPTH(DRI_CONF_TEXTURE_DEPTH_FB)
        DRI_CONF_DEF_MAX_ANISOTROPY(1.0,"1.0,2.0,4.0,8.0,16.0")
        DRI_CONF_NO_NEG_LOD_BIAS(false)
        DRI_CONF_FORCE_S3TC_ENABLE(false)
        DRI_CONF_COLOR_REDUCTION(DRI_CONF_COLOR_REDUCTION_DITHER)
        DRI_CONF_ROUND_MODE(DRI_CONF_ROUND_TRUNC)
        DRI_CONF_DITHER_MODE(DRI_CONF_DITHER_XERRORDIFF)
        DRI_CONF_ALLOW_LARGE_TEXTURES(2)
        DRI_CONF_TEXTURE_BLEND_QUALITY(1.0,"0.0:1.0")
    DRI_CONF_SECTION_END
    DRI_CONF_SECTION_DEBUG
        DRI_CONF_NO_RAST(false)
    DRI_CONF_SECTION_END
    DRI_CONF_SECTION_SOFTWARE
        DRI_CONF_NV_VERTEX_PROGRAM(false)
    DRI_CONF_SECTION_END
DRI_CONF_END;
static const GLuint __driNConfigOptions = 16;

extern const struct dri_extension blend_extensions[];
extern const struct dri_extension ARB_vp_extension[];
extern const struct dri_extension NV_vp_extension[];
extern const struct dri_extension ATI_fs_extension[];
extern const struct dri_extension point_extensions[];

#elif RADEON_COMMON && defined(RADEON_COMMON_FOR_R300)

/* TODO: integrate these into xmlpool.h! */
#define DRI_CONF_MAX_TEXTURE_IMAGE_UNITS(def,min,max) \
DRI_CONF_OPT_BEGIN_V(texture_image_units,int,def, # min ":" # max ) \
        DRI_CONF_DESC(en,"Number of texture image units") \
        DRI_CONF_DESC(de,"Anzahl der Textureinheiten") \
DRI_CONF_OPT_END

#define DRI_CONF_MAX_TEXTURE_COORD_UNITS(def,min,max) \
DRI_CONF_OPT_BEGIN_V(texture_coord_units,int,def, # min ":" # max ) \
        DRI_CONF_DESC(en,"Number of texture coordinate units") \
        DRI_CONF_DESC(de,"Anzahl der Texturkoordinateneinheiten") \
DRI_CONF_OPT_END

#define DRI_CONF_COMMAND_BUFFER_SIZE(def,min,max) \
DRI_CONF_OPT_BEGIN_V(command_buffer_size,int,def, # min ":" # max ) \
        DRI_CONF_DESC(en,"Size of command buffer (in KB)") \
        DRI_CONF_DESC(de,"Grösse des Befehlspuffers (in KB)") \
DRI_CONF_OPT_END

#define DRI_CONF_DISABLE_S3TC(def) \
DRI_CONF_OPT_BEGIN(disable_s3tc,bool,def) \
        DRI_CONF_DESC(en,"Disable S3TC compression") \
DRI_CONF_OPT_END

#define DRI_CONF_DISABLE_FALLBACK(def) \
DRI_CONF_OPT_BEGIN(disable_lowimpact_fallback,bool,def) \
        DRI_CONF_DESC(en,"Disable Low-impact fallback") \
DRI_CONF_OPT_END

#define DRI_CONF_DISABLE_DOUBLE_SIDE_STENCIL(def) \
DRI_CONF_OPT_BEGIN(disable_stencil_two_side,bool,def) \
        DRI_CONF_DESC(en,"Disable GL_EXT_stencil_two_side") \
DRI_CONF_OPT_END

#define DRI_CONF_FP_OPTIMIZATION(def) \
DRI_CONF_OPT_BEGIN_V(fp_optimization,enum,def,"0:1") \
	DRI_CONF_DESC_BEGIN(en,"Fragment Program optimization") \
                DRI_CONF_ENUM(0,"Optimize for Speed") \
                DRI_CONF_ENUM(1,"Optimize for Quality") \
        DRI_CONF_DESC_END \
DRI_CONF_OPT_END

PUBLIC const char __driConfigOptions[] =
DRI_CONF_BEGIN
	DRI_CONF_SECTION_PERFORMANCE
		DRI_CONF_TCL_MODE(DRI_CONF_TCL_CODEGEN)
		DRI_CONF_FTHROTTLE_MODE(DRI_CONF_FTHROTTLE_IRQS)
		DRI_CONF_VBLANK_MODE(DRI_CONF_VBLANK_DEF_INTERVAL_0)
		DRI_CONF_MAX_TEXTURE_IMAGE_UNITS(8, 2, 8)
		DRI_CONF_MAX_TEXTURE_COORD_UNITS(8, 2, 8)
		DRI_CONF_COMMAND_BUFFER_SIZE(8, 8, 32)
		DRI_CONF_DISABLE_FALLBACK(true)
		DRI_CONF_DISABLE_DOUBLE_SIDE_STENCIL(false)
	DRI_CONF_SECTION_END
	DRI_CONF_SECTION_QUALITY
		DRI_CONF_TEXTURE_DEPTH(DRI_CONF_TEXTURE_DEPTH_FB)
		DRI_CONF_DEF_MAX_ANISOTROPY(1.0, "1.0,2.0,4.0,8.0,16.0")
		DRI_CONF_FORCE_S3TC_ENABLE(false)
		DRI_CONF_DISABLE_S3TC(false)
		DRI_CONF_COLOR_REDUCTION(DRI_CONF_COLOR_REDUCTION_DITHER)
		DRI_CONF_ROUND_MODE(DRI_CONF_ROUND_TRUNC)
		DRI_CONF_DITHER_MODE(DRI_CONF_DITHER_XERRORDIFF)
		DRI_CONF_FP_OPTIMIZATION(DRI_CONF_FP_OPTIMIZATION_SPEED)
	DRI_CONF_SECTION_END
	DRI_CONF_SECTION_DEBUG
		DRI_CONF_NO_RAST(false)
	DRI_CONF_SECTION_END
DRI_CONF_END;
static const GLuint __driNConfigOptions = 17;

#ifndef RADEON_DEBUG
int RADEON_DEBUG = 0;

static const struct dri_debug_control debug_control[] = {
	{"fall", DEBUG_FALLBACKS},
	{"tex", DEBUG_TEXTURE},
	{"ioctl", DEBUG_IOCTL},
	{"prim", DEBUG_PRIMS},
	{"vert", DEBUG_VERTS},
	{"state", DEBUG_STATE},
	{"code", DEBUG_CODEGEN},
	{"vfmt", DEBUG_VFMT},
	{"vtxf", DEBUG_VFMT},
	{"verb", DEBUG_VERBOSE},
	{"dri", DEBUG_DRI},
	{"dma", DEBUG_DMA},
	{"san", DEBUG_SANITY},
	{"sync", DEBUG_SYNC},
	{"pix", DEBUG_PIXEL},
	{"mem", DEBUG_MEMORY},
	{"allmsg", ~DEBUG_SYNC}, /* avoid the term "sync" because the parser uses strstr */
	{NULL, 0}
};
#endif /* RADEON_DEBUG */

#endif /* RADEON_COMMON && defined(RADEON_COMMON_FOR_R300) */

extern const struct dri_extension card_extensions[];

static int getSwapInfo( __DRIdrawablePrivate *dPriv, __DRIswapInfo * sInfo );

static int
radeonGetParam(int fd, int param, void *value)
{
  int ret;
  drm_radeon_getparam_t gp;

  gp.param = param;
  gp.value = value;

  ret = drmCommandWriteRead( fd, DRM_RADEON_GETPARAM, &gp, sizeof(gp));
  return ret;
}

static const __DRIconfig **
radeonFillInModes( __DRIscreenPrivate *psp,
		   unsigned pixel_bits, unsigned depth_bits,
		   unsigned stencil_bits, GLboolean have_back_buffer )
{
    __DRIconfig **configs;
    __GLcontextModes *m;
    unsigned depth_buffer_factor;
    unsigned back_buffer_factor;
    GLenum fb_format;
    GLenum fb_type;
    int i;

    /* Right now GLX_SWAP_COPY_OML isn't supported, but it would be easy
     * enough to add support.  Basically, if a context is created with an
     * fbconfig where the swap method is GLX_SWAP_COPY_OML, pageflipping
     * will never be used.
     */
    static const GLenum back_buffer_modes[] = {
	GLX_NONE, GLX_SWAP_UNDEFINED_OML /*, GLX_SWAP_COPY_OML */
    };

    uint8_t depth_bits_array[2];
    uint8_t stencil_bits_array[2];


    depth_bits_array[0] = depth_bits;
    depth_bits_array[1] = depth_bits;

    /* Just like with the accumulation buffer, always provide some modes
     * with a stencil buffer.  It will be a sw fallback, but some apps won't
     * care about that.
     */
    stencil_bits_array[0] = 0;
    stencil_bits_array[1] = (stencil_bits == 0) ? 8 : stencil_bits;

    depth_buffer_factor = ((depth_bits != 0) || (stencil_bits != 0)) ? 2 : 1;
    back_buffer_factor  = (have_back_buffer) ? 2 : 1;

    if ( pixel_bits == 16 ) {
        fb_format = GL_RGB;
        fb_type = GL_UNSIGNED_SHORT_5_6_5;
    }
    else {
        fb_format = GL_BGRA;
        fb_type = GL_UNSIGNED_INT_8_8_8_8_REV;
    }

    configs = driCreateConfigs(fb_format, fb_type,
			       depth_bits_array, stencil_bits_array,
			       depth_buffer_factor,
			       back_buffer_modes, back_buffer_factor);
    if (configs == NULL) {
	fprintf( stderr, "[%s:%u] Error creating FBConfig!\n",
		 __func__, __LINE__ );
	return NULL;
    }

    /* Mark the visual as slow if there are "fake" stencil bits.
     */
    for (i = 0; configs[i]; i++) {
	m = &configs[i]->modes;
	if ((m->stencilBits != 0) && (m->stencilBits != stencil_bits)) {
	    m->visualRating = GLX_SLOW_CONFIG;
	}
    }

    return (const __DRIconfig **) configs;
}

#if !RADEON_COMMON
static const __DRItexOffsetExtension radeonTexOffsetExtension = {
    { __DRI_TEX_OFFSET, __DRI_TEX_OFFSET_VERSION },
    radeonSetTexOffset,
};
#endif

#if RADEON_COMMON && defined(RADEON_COMMON_FOR_R200)
static const __DRIallocateExtension r200AllocateExtension = {
    { __DRI_ALLOCATE, __DRI_ALLOCATE_VERSION },
    r200AllocateMemoryMESA,
    r200FreeMemoryMESA,
    r200GetMemoryOffsetMESA
};

static const __DRItexOffsetExtension r200texOffsetExtension = {
    { __DRI_TEX_OFFSET, __DRI_TEX_OFFSET_VERSION },
   r200SetTexOffset,
};
#endif

#if RADEON_COMMON && defined(RADEON_COMMON_FOR_R300)
static const __DRItexOffsetExtension r300texOffsetExtension = {
    { __DRI_TEX_OFFSET, __DRI_TEX_OFFSET_VERSION },
   r300SetTexOffset,
};
#endif

/* Create the device specific screen private data struct.
 */
static radeonScreenPtr
radeonCreateScreen( __DRIscreenPrivate *sPriv )
{
   radeonScreenPtr screen;
   RADEONDRIPtr dri_priv = (RADEONDRIPtr)sPriv->pDevPriv;
   unsigned char *RADEONMMIO;
   int i;
   int ret;
   uint32_t temp;

   if (sPriv->devPrivSize != sizeof(RADEONDRIRec)) {
      fprintf(stderr,"\nERROR!  sizeof(RADEONDRIRec) does not match passed size from device driver\n");
      return GL_FALSE;
   }

   /* Allocate the private area */
   screen = (radeonScreenPtr) CALLOC( sizeof(*screen) );
   if ( !screen ) {
      __driUtilMessage("%s: Could not allocate memory for screen structure",
		       __FUNCTION__);
      return NULL;
   }

#if DO_DEBUG && RADEON_COMMON && defined(RADEON_COMMON_FOR_R300)
	RADEON_DEBUG = driParseDebugString(getenv("RADEON_DEBUG"), debug_control);
#endif

   /* parse information in __driConfigOptions */
   driParseOptionInfo (&screen->optionCache,
		       __driConfigOptions, __driNConfigOptions);

   /* This is first since which regions we map depends on whether or
    * not we are using a PCI card.
    */
   screen->card_type = (dri_priv->IsPCI ? RADEON_CARD_PCI : RADEON_CARD_AGP);
   {
      int ret;
      ret = radeonGetParam( sPriv->fd, RADEON_PARAM_GART_BUFFER_OFFSET,
			    &screen->gart_buffer_offset);

      if (ret) {
	 FREE( screen );
	 fprintf(stderr, "drm_radeon_getparam_t (RADEON_PARAM_GART_BUFFER_OFFSET): %d\n", ret);
	 return NULL;
      }

      ret = radeonGetParam( sPriv->fd, RADEON_PARAM_GART_BASE,
			    &screen->gart_base);
      if (ret) {
	 FREE( screen );
	 fprintf(stderr, "drm_radeon_getparam_t (RADEON_PARAM_GART_BASE): %d\n", ret);
	 return NULL;
      }

      ret = radeonGetParam( sPriv->fd, RADEON_PARAM_IRQ_NR,
			    &screen->irq);
      if (ret) {
	 FREE( screen );
	 fprintf(stderr, "drm_radeon_getparam_t (RADEON_PARAM_IRQ_NR): %d\n", ret);
	 return NULL;
      }
      screen->drmSupportsCubeMapsR200 = (sPriv->drm_version.minor >= 7);
      screen->drmSupportsBlendColor = (sPriv->drm_version.minor >= 11);
      screen->drmSupportsTriPerf = (sPriv->drm_version.minor >= 16);
      screen->drmSupportsFragShader = (sPriv->drm_version.minor >= 18);
      screen->drmSupportsPointSprites = (sPriv->drm_version.minor >= 13);
      screen->drmSupportsCubeMapsR100 = (sPriv->drm_version.minor >= 15);
      screen->drmSupportsVertexProgram = (sPriv->drm_version.minor >= 25);
   }

   screen->mmio.handle = dri_priv->registerHandle;
   screen->mmio.size   = dri_priv->registerSize;
   if ( drmMap( sPriv->fd,
		screen->mmio.handle,
		screen->mmio.size,
		&screen->mmio.map ) ) {
      FREE( screen );
      __driUtilMessage("%s: drmMap failed\n", __FUNCTION__ );
      return NULL;
   }

   RADEONMMIO = screen->mmio.map;

   screen->status.handle = dri_priv->statusHandle;
   screen->status.size   = dri_priv->statusSize;
   if ( drmMap( sPriv->fd,
		screen->status.handle,
		screen->status.size,
		&screen->status.map ) ) {
      drmUnmap( screen->mmio.map, screen->mmio.size );
      FREE( screen );
      __driUtilMessage("%s: drmMap (2) failed\n", __FUNCTION__ );
      return NULL;
   }
   screen->scratch = (__volatile__ uint32_t *)
      ((GLubyte *)screen->status.map + RADEON_SCRATCH_REG_OFFSET);

   screen->buffers = drmMapBufs( sPriv->fd );
   if ( !screen->buffers ) {
      drmUnmap( screen->status.map, screen->status.size );
      drmUnmap( screen->mmio.map, screen->mmio.size );
      FREE( screen );
      __driUtilMessage("%s: drmMapBufs failed\n", __FUNCTION__ );
      return NULL;
   }

   if ( dri_priv->gartTexHandle && dri_priv->gartTexMapSize ) {
      screen->gartTextures.handle = dri_priv->gartTexHandle;
      screen->gartTextures.size   = dri_priv->gartTexMapSize;
      if ( drmMap( sPriv->fd,
		   screen->gartTextures.handle,
		   screen->gartTextures.size,
		   (drmAddressPtr)&screen->gartTextures.map ) ) {
	 drmUnmapBufs( screen->buffers );
	 drmUnmap( screen->status.map, screen->status.size );
	 drmUnmap( screen->mmio.map, screen->mmio.size );
	 FREE( screen );
	 __driUtilMessage("%s: drmMap failed for GART texture area\n", __FUNCTION__);
	 return NULL;
      }

      screen->gart_texture_offset = dri_priv->gartTexOffset + screen->gart_base;
   }

   screen->chip_flags = 0;
   /* XXX: add more chipsets */
   switch ( dri_priv->deviceID ) {
   case PCI_CHIP_RADEON_LY:
   case PCI_CHIP_RADEON_LZ:
   case PCI_CHIP_RADEON_QY:
   case PCI_CHIP_RADEON_QZ:
   case PCI_CHIP_RN50_515E:
   case PCI_CHIP_RN50_5969:
      screen->chip_family = CHIP_FAMILY_RV100;
      break;

   case PCI_CHIP_RS100_4136:
   case PCI_CHIP_RS100_4336:
      screen->chip_family = CHIP_FAMILY_RS100;
      break;

   case PCI_CHIP_RS200_4137:
   case PCI_CHIP_RS200_4337:
   case PCI_CHIP_RS250_4237:
   case PCI_CHIP_RS250_4437:
      screen->chip_family = CHIP_FAMILY_RS200;
      break;

   case PCI_CHIP_RADEON_QD:
   case PCI_CHIP_RADEON_QE:
   case PCI_CHIP_RADEON_QF:
   case PCI_CHIP_RADEON_QG:
      /* all original radeons (7200) presumably have a stencil op bug */
      screen->chip_family = CHIP_FAMILY_R100;
      screen->chip_flags = RADEON_CHIPSET_TCL | RADEON_CHIPSET_BROKEN_STENCIL;
      break;

   case PCI_CHIP_RV200_QW:
   case PCI_CHIP_RV200_QX:
   case PCI_CHIP_RADEON_LW:
   case PCI_CHIP_RADEON_LX:
      screen->chip_family = CHIP_FAMILY_RV200;
      screen->chip_flags = RADEON_CHIPSET_TCL;
      break;

   case PCI_CHIP_R200_BB:
   case PCI_CHIP_R200_BC:
   case PCI_CHIP_R200_QH:
   case PCI_CHIP_R200_QL:
   case PCI_CHIP_R200_QM:
      screen->chip_family = CHIP_FAMILY_R200;
      screen->chip_flags = RADEON_CHIPSET_TCL;
      break;

   case PCI_CHIP_RV250_If:
   case PCI_CHIP_RV250_Ig:
   case PCI_CHIP_RV250_Ld:
   case PCI_CHIP_RV250_Lf:
   case PCI_CHIP_RV250_Lg:
      screen->chip_family = CHIP_FAMILY_RV250;
      screen->chip_flags = R200_CHIPSET_YCBCR_BROKEN | RADEON_CHIPSET_TCL;
      break;

   case PCI_CHIP_RV280_5960:
   case PCI_CHIP_RV280_5961:
   case PCI_CHIP_RV280_5962:
   case PCI_CHIP_RV280_5964:
   case PCI_CHIP_RV280_5965:
   case PCI_CHIP_RV280_5C61:
   case PCI_CHIP_RV280_5C63:
      screen->chip_family = CHIP_FAMILY_RV280;
      screen->chip_flags = RADEON_CHIPSET_TCL;
      break;

   case PCI_CHIP_RS300_5834:
   case PCI_CHIP_RS300_5835:
   case PCI_CHIP_RS350_7834:
   case PCI_CHIP_RS350_7835:
      screen->chip_family = CHIP_FAMILY_RS300;
      break;

      /* 9500 with 1 pipe verified by: Reid Linnemann <lreid@cs.okstate.edu> */
   case PCI_CHIP_R300_AD:
      screen->chip_family = CHIP_FAMILY_RV350;
      screen->chip_flags = RADEON_CHIPSET_TCL;
      break;
   case PCI_CHIP_R300_AE:
   case PCI_CHIP_R300_AF:
   case PCI_CHIP_R300_AG:
   case PCI_CHIP_R300_ND:
   case PCI_CHIP_R300_NE:
   case PCI_CHIP_R300_NF:
   case PCI_CHIP_R300_NG:
      screen->chip_family = CHIP_FAMILY_R300;
      screen->chip_flags = RADEON_CHIPSET_TCL;
      break;

   case PCI_CHIP_RV350_AP:
   case PCI_CHIP_RV350_AQ:
   case PCI_CHIP_RV350_AR:
   case PCI_CHIP_RV350_AS:
   case PCI_CHIP_RV350_AT:
   case PCI_CHIP_RV350_AV:
   case PCI_CHIP_RV350_AU:
   case PCI_CHIP_RV350_NP:
   case PCI_CHIP_RV350_NQ:
   case PCI_CHIP_RV350_NR:
   case PCI_CHIP_RV350_NS:
   case PCI_CHIP_RV350_NT:
   case PCI_CHIP_RV350_NV:
      screen->chip_family = CHIP_FAMILY_RV350;
      screen->chip_flags = RADEON_CHIPSET_TCL;
      break;

   case PCI_CHIP_R350_AH:
   case PCI_CHIP_R350_AI:
   case PCI_CHIP_R350_AJ:
   case PCI_CHIP_R350_AK:
   case PCI_CHIP_R350_NH:
   case PCI_CHIP_R350_NI:
   case PCI_CHIP_R360_NJ:
   case PCI_CHIP_R350_NK:
      screen->chip_family = CHIP_FAMILY_R350;
      screen->chip_flags = RADEON_CHIPSET_TCL;
      break;

   case PCI_CHIP_RV370_5460:
   case PCI_CHIP_RV370_5462:
   case PCI_CHIP_RV370_5464:
   case PCI_CHIP_RV370_5B60:
   case PCI_CHIP_RV370_5B62:
   case PCI_CHIP_RV370_5B63:
   case PCI_CHIP_RV370_5B64:
   case PCI_CHIP_RV370_5B65:
   case PCI_CHIP_RV380_3150:
   case PCI_CHIP_RV380_3152:
   case PCI_CHIP_RV380_3154:
   case PCI_CHIP_RV380_3E50:
   case PCI_CHIP_RV380_3E54:
      screen->chip_family = CHIP_FAMILY_RV380;
      screen->chip_flags = RADEON_CHIPSET_TCL;
      break;

   case PCI_CHIP_R420_JN:
   case PCI_CHIP_R420_JH:
   case PCI_CHIP_R420_JI:
   case PCI_CHIP_R420_JJ:
   case PCI_CHIP_R420_JK:
   case PCI_CHIP_R420_JL:
   case PCI_CHIP_R420_JM:
   case PCI_CHIP_R420_JO:
   case PCI_CHIP_R420_JP:
   case PCI_CHIP_R420_JT:
   case PCI_CHIP_R481_4B49:
   case PCI_CHIP_R481_4B4A:
   case PCI_CHIP_R481_4B4B:
   case PCI_CHIP_R481_4B4C:
   case PCI_CHIP_R423_UH:
   case PCI_CHIP_R423_UI:
   case PCI_CHIP_R423_UJ:
   case PCI_CHIP_R423_UK:
   case PCI_CHIP_R430_554C:
   case PCI_CHIP_R430_554D:
   case PCI_CHIP_R430_554E:
   case PCI_CHIP_R430_554F:
   case PCI_CHIP_R423_5550:
   case PCI_CHIP_R423_UQ:
   case PCI_CHIP_R423_UR:
   case PCI_CHIP_R423_UT:
   case PCI_CHIP_R430_5D48:
   case PCI_CHIP_R430_5D49:
   case PCI_CHIP_R430_5D4A:
   case PCI_CHIP_R480_5D4C:
   case PCI_CHIP_R480_5D4D:
   case PCI_CHIP_R480_5D4E:
   case PCI_CHIP_R480_5D4F:
   case PCI_CHIP_R480_5D50:
   case PCI_CHIP_R480_5D52:
   case PCI_CHIP_R423_5D57:
      screen->chip_family = CHIP_FAMILY_R420;
      screen->chip_flags = RADEON_CHIPSET_TCL;
      break;

   case PCI_CHIP_RV410_5E4C:
   case PCI_CHIP_RV410_5E4F:
   case PCI_CHIP_RV410_564A:
   case PCI_CHIP_RV410_564B:
   case PCI_CHIP_RV410_564F:
   case PCI_CHIP_RV410_5652:
   case PCI_CHIP_RV410_5653:
   case PCI_CHIP_RV410_5657:
   case PCI_CHIP_RV410_5E48:
   case PCI_CHIP_RV410_5E4A:
   case PCI_CHIP_RV410_5E4B:
   case PCI_CHIP_RV410_5E4D:
      screen->chip_family = CHIP_FAMILY_RV410;
      screen->chip_flags = RADEON_CHIPSET_TCL;
      break;

   case PCI_CHIP_RS480_5954:
   case PCI_CHIP_RS480_5955:
   case PCI_CHIP_RS482_5974:
   case PCI_CHIP_RS482_5975:
   case PCI_CHIP_RS400_5A41:
   case PCI_CHIP_RS400_5A42:
   case PCI_CHIP_RC410_5A61:
   case PCI_CHIP_RC410_5A62:
      screen->chip_family = CHIP_FAMILY_RS400;
      break;

   case PCI_CHIP_RS690_791E:
   case PCI_CHIP_RS690_791F:
      screen->chip_family = CHIP_FAMILY_RS690;
      break;
   case PCI_CHIP_RS740_796C:
   case PCI_CHIP_RS740_796D:
   case PCI_CHIP_RS740_796E:
   case PCI_CHIP_RS740_796F:
      screen->chip_family = CHIP_FAMILY_RS740;
      break;

   case PCI_CHIP_R520_7100:
   case PCI_CHIP_R520_7101:
   case PCI_CHIP_R520_7102:
   case PCI_CHIP_R520_7103:
   case PCI_CHIP_R520_7104:
   case PCI_CHIP_R520_7105:
   case PCI_CHIP_R520_7106:
   case PCI_CHIP_R520_7108:
   case PCI_CHIP_R520_7109:
   case PCI_CHIP_R520_710A:
   case PCI_CHIP_R520_710B:
   case PCI_CHIP_R520_710C:
   case PCI_CHIP_R520_710E:
   case PCI_CHIP_R520_710F:
      screen->chip_family = CHIP_FAMILY_R520;
      screen->chip_flags = RADEON_CHIPSET_TCL;
      break;

   case PCI_CHIP_RV515_7140:
   case PCI_CHIP_RV515_7141:
   case PCI_CHIP_RV515_7142:
   case PCI_CHIP_RV515_7143:
   case PCI_CHIP_RV515_7144:
   case PCI_CHIP_RV515_7145:
   case PCI_CHIP_RV515_7146:
   case PCI_CHIP_RV515_7147:
   case PCI_CHIP_RV515_7149:
   case PCI_CHIP_RV515_714A:
   case PCI_CHIP_RV515_714B:
   case PCI_CHIP_RV515_714C:
   case PCI_CHIP_RV515_714D:
   case PCI_CHIP_RV515_714E:
   case PCI_CHIP_RV515_714F:
   case PCI_CHIP_RV515_7151:
   case PCI_CHIP_RV515_7152:
   case PCI_CHIP_RV515_7153:
   case PCI_CHIP_RV515_715E:
   case PCI_CHIP_RV515_715F:
   case PCI_CHIP_RV515_7180:
   case PCI_CHIP_RV515_7181:
   case PCI_CHIP_RV515_7183:
   case PCI_CHIP_RV515_7186:
   case PCI_CHIP_RV515_7187:
   case PCI_CHIP_RV515_7188:
   case PCI_CHIP_RV515_718A:
   case PCI_CHIP_RV515_718B:
   case PCI_CHIP_RV515_718C:
   case PCI_CHIP_RV515_718D:
   case PCI_CHIP_RV515_718F:
   case PCI_CHIP_RV515_7193:
   case PCI_CHIP_RV515_7196:
   case PCI_CHIP_RV515_719B:
   case PCI_CHIP_RV515_719F:
   case PCI_CHIP_RV515_7200:
   case PCI_CHIP_RV515_7210:
   case PCI_CHIP_RV515_7211:
      screen->chip_family = CHIP_FAMILY_RV515;
      screen->chip_flags = RADEON_CHIPSET_TCL;
      break;

   case PCI_CHIP_RV530_71C0:
   case PCI_CHIP_RV530_71C1:
   case PCI_CHIP_RV530_71C2:
   case PCI_CHIP_RV530_71C3:
   case PCI_CHIP_RV530_71C4:
   case PCI_CHIP_RV530_71C5:
   case PCI_CHIP_RV530_71C6:
   case PCI_CHIP_RV530_71C7:
   case PCI_CHIP_RV530_71CD:
   case PCI_CHIP_RV530_71CE:
   case PCI_CHIP_RV530_71D2:
   case PCI_CHIP_RV530_71D4:
   case PCI_CHIP_RV530_71D5:
   case PCI_CHIP_RV530_71D6:
   case PCI_CHIP_RV530_71DA:
   case PCI_CHIP_RV530_71DE:
      screen->chip_family = CHIP_FAMILY_RV530;
      screen->chip_flags = RADEON_CHIPSET_TCL;
      break;

   case PCI_CHIP_R580_7240:
   case PCI_CHIP_R580_7243:
   case PCI_CHIP_R580_7244:
   case PCI_CHIP_R580_7245:
   case PCI_CHIP_R580_7246:
   case PCI_CHIP_R580_7247:
   case PCI_CHIP_R580_7248:
   case PCI_CHIP_R580_7249:
   case PCI_CHIP_R580_724A:
   case PCI_CHIP_R580_724B:
   case PCI_CHIP_R580_724C:
   case PCI_CHIP_R580_724D:
   case PCI_CHIP_R580_724E:
   case PCI_CHIP_R580_724F:
   case PCI_CHIP_R580_7284:
      screen->chip_family = CHIP_FAMILY_R580;
      screen->chip_flags = RADEON_CHIPSET_TCL;
      break;

   case PCI_CHIP_RV570_7280:
   case PCI_CHIP_RV560_7281:
   case PCI_CHIP_RV560_7283:
   case PCI_CHIP_RV560_7287:
   case PCI_CHIP_RV570_7288:
   case PCI_CHIP_RV570_7289:
   case PCI_CHIP_RV570_728B:
   case PCI_CHIP_RV570_728C:
   case PCI_CHIP_RV560_7290:
   case PCI_CHIP_RV560_7291:
   case PCI_CHIP_RV560_7293:
   case PCI_CHIP_RV560_7297:
      screen->chip_family = CHIP_FAMILY_RV560;
      screen->chip_flags = RADEON_CHIPSET_TCL;
      break;

   default:
      fprintf(stderr, "unknown chip id 0x%x, can't guess.\n",
	      dri_priv->deviceID);
      return NULL;
   }
   if ((screen->chip_family == CHIP_FAMILY_R350 || screen->chip_family == CHIP_FAMILY_R300) &&
       sPriv->ddx_version.minor < 2) {
      fprintf(stderr, "xf86-video-ati-6.6.2 or newer needed for Radeon 9500/9700/9800 cards.\n");
      return NULL;
   }

   if ((sPriv->drm_version.minor < 29) && (screen->chip_family >= CHIP_FAMILY_RV515)) {
      fprintf(stderr, "R500 support requires a newer drm.\n");
      return NULL;
   }

   if (getenv("R300_NO_TCL"))
     screen->chip_flags &= ~RADEON_CHIPSET_TCL;

   if (screen->chip_family <= CHIP_FAMILY_RS200)
      screen->chip_flags |= RADEON_CLASS_R100;
   else if (screen->chip_family <= CHIP_FAMILY_RV280)
      screen->chip_flags |= RADEON_CLASS_R200;
   else
      screen->chip_flags |= RADEON_CLASS_R300;

   screen->cpp = dri_priv->bpp / 8;
   screen->AGPMode = dri_priv->AGPMode;

   ret = radeonGetParam( sPriv->fd, RADEON_PARAM_FB_LOCATION,
                         &temp);
   if (ret) {
       if (screen->chip_family < CHIP_FAMILY_RS690)
	   screen->fbLocation      = ( INREG( RADEON_MC_FB_LOCATION ) & 0xffff) << 16;
       else {
           FREE( screen );
           fprintf(stderr, "Unable to get fb location need newer drm\n");
           return NULL;
       }
   } else {
       screen->fbLocation = (temp & 0xffff) << 16;
   }

   if (screen->chip_family >= CHIP_FAMILY_RV515) {
       ret = radeonGetParam( sPriv->fd, RADEON_PARAM_NUM_GB_PIPES,
			     &temp);
       if (ret) {
	   fprintf(stderr, "Unable to get num_pipes, need newer drm\n");
	   switch (screen->chip_family) {
	   case CHIP_FAMILY_R300:
	   case CHIP_FAMILY_R350:
	       screen->num_gb_pipes = 2;
	       break;
	   case CHIP_FAMILY_R420:
	   case CHIP_FAMILY_R520:
	   case CHIP_FAMILY_R580:
	   case CHIP_FAMILY_RV560:
	   case CHIP_FAMILY_RV570:
	       screen->num_gb_pipes = 4;
	       break;
	   case CHIP_FAMILY_RV350:
	   case CHIP_FAMILY_RV515:
	   case CHIP_FAMILY_RV530:
	   case CHIP_FAMILY_RV410:
	   default:
	       screen->num_gb_pipes = 1;
	       break;
	   }
       } else {
	   screen->num_gb_pipes = temp;
       }
   }

   if ( sPriv->drm_version.minor >= 10 ) {
      drm_radeon_setparam_t sp;

      sp.param = RADEON_SETPARAM_FB_LOCATION;
      sp.value = screen->fbLocation;

      drmCommandWrite( sPriv->fd, DRM_RADEON_SETPARAM,
		       &sp, sizeof( sp ) );
   }

   screen->frontOffset	= dri_priv->frontOffset;
   screen->frontPitch	= dri_priv->frontPitch;
   screen->backOffset	= dri_priv->backOffset;
   screen->backPitch	= dri_priv->backPitch;
   screen->depthOffset	= dri_priv->depthOffset;
   screen->depthPitch	= dri_priv->depthPitch;

   /* Check if ddx has set up a surface reg to cover depth buffer */
   screen->depthHasSurface = (sPriv->ddx_version.major > 4) ||
      /* these chips don't use tiled z without hyperz. So always pretend
         we have set up a surface which will cause linear reads/writes */
      (IS_R100_CLASS(screen) &&
      !(screen->chip_flags & RADEON_CHIPSET_TCL));

   if ( dri_priv->textureSize == 0 ) {
      screen->texOffset[RADEON_LOCAL_TEX_HEAP] = screen->gart_texture_offset;
      screen->texSize[RADEON_LOCAL_TEX_HEAP] = dri_priv->gartTexMapSize;
      screen->logTexGranularity[RADEON_LOCAL_TEX_HEAP] =
	 dri_priv->log2GARTTexGran;
   } else {
      screen->texOffset[RADEON_LOCAL_TEX_HEAP] = dri_priv->textureOffset
				               + screen->fbLocation;
      screen->texSize[RADEON_LOCAL_TEX_HEAP] = dri_priv->textureSize;
      screen->logTexGranularity[RADEON_LOCAL_TEX_HEAP] =
	 dri_priv->log2TexGran;
   }

   if ( !screen->gartTextures.map || dri_priv->textureSize == 0
	|| getenv( "RADEON_GARTTEXTURING_FORCE_DISABLE" ) ) {
      screen->numTexHeaps = RADEON_NR_TEX_HEAPS - 1;
      screen->texOffset[RADEON_GART_TEX_HEAP] = 0;
      screen->texSize[RADEON_GART_TEX_HEAP] = 0;
      screen->logTexGranularity[RADEON_GART_TEX_HEAP] = 0;
   } else {
      screen->numTexHeaps = RADEON_NR_TEX_HEAPS;
      screen->texOffset[RADEON_GART_TEX_HEAP] = screen->gart_texture_offset;
      screen->texSize[RADEON_GART_TEX_HEAP] = dri_priv->gartTexMapSize;
      screen->logTexGranularity[RADEON_GART_TEX_HEAP] =
	 dri_priv->log2GARTTexGran;
   }

   i = 0;
   screen->extensions[i++] = &driCopySubBufferExtension.base;
   screen->extensions[i++] = &driFrameTrackingExtension.base;
   screen->extensions[i++] = &driReadDrawableExtension;

   if ( screen->irq != 0 ) {
       screen->extensions[i++] = &driSwapControlExtension.base;
       screen->extensions[i++] = &driMediaStreamCounterExtension.base;
   }

#if !RADEON_COMMON
   screen->extensions[i++] = &radeonTexOffsetExtension.base;
#endif

#if RADEON_COMMON && defined(RADEON_COMMON_FOR_R200)
   if (IS_R200_CLASS(screen))
       screen->extensions[i++] = &r200AllocateExtension.base;

   screen->extensions[i++] = &r200texOffsetExtension.base;
#endif

#if RADEON_COMMON && defined(RADEON_COMMON_FOR_R300)
   screen->extensions[i++] = &r300texOffsetExtension.base;
#endif

   screen->extensions[i++] = NULL;
   sPriv->extensions = screen->extensions;

   screen->driScreen = sPriv;
   screen->sarea_priv_offset = dri_priv->sarea_priv_offset;
   return screen;
}

/* Destroy the device specific screen private data struct.
 */
static void
radeonDestroyScreen( __DRIscreenPrivate *sPriv )
{
   radeonScreenPtr screen = (radeonScreenPtr)sPriv->private;

   if (!screen)
      return;

   if ( screen->gartTextures.map ) {
      drmUnmap( screen->gartTextures.map, screen->gartTextures.size );
   }
   drmUnmapBufs( screen->buffers );
   drmUnmap( screen->status.map, screen->status.size );
   drmUnmap( screen->mmio.map, screen->mmio.size );

   /* free all option information */
   driDestroyOptionInfo (&screen->optionCache);

   FREE( screen );
   sPriv->private = NULL;
}


/* Initialize the driver specific screen private data.
 */
static GLboolean
radeonInitDriver( __DRIscreenPrivate *sPriv )
{
   sPriv->private = (void *) radeonCreateScreen( sPriv );
   if ( !sPriv->private ) {
      radeonDestroyScreen( sPriv );
      return GL_FALSE;
   }

   return GL_TRUE;
}


/**
 * Create the Mesa framebuffer and renderbuffers for a given window/drawable.
 *
 * \todo This function (and its interface) will need to be updated to support
 * pbuffers.
 */
static GLboolean
radeonCreateBuffer( __DRIscreenPrivate *driScrnPriv,
                    __DRIdrawablePrivate *driDrawPriv,
                    const __GLcontextModes *mesaVis,
                    GLboolean isPixmap )
{
   radeonScreenPtr screen = (radeonScreenPtr) driScrnPriv->private;

   if (isPixmap) {
      return GL_FALSE; /* not implemented */
   }
   else {
      const GLboolean swDepth = GL_FALSE;
      const GLboolean swAlpha = GL_FALSE;
      const GLboolean swAccum = mesaVis->accumRedBits > 0;
      const GLboolean swStencil = mesaVis->stencilBits > 0 &&
         mesaVis->depthBits != 24;
      struct gl_framebuffer *fb = _mesa_create_framebuffer(mesaVis);

      /* front color renderbuffer */
      {
         driRenderbuffer *frontRb
            = driNewRenderbuffer(GL_RGBA,
                                 driScrnPriv->pFB + screen->frontOffset,
                                 screen->cpp,
                                 screen->frontOffset, screen->frontPitch,
                                 driDrawPriv);
         radeonSetSpanFunctions(frontRb, mesaVis);
         _mesa_add_renderbuffer(fb, BUFFER_FRONT_LEFT, &frontRb->Base);
      }

      /* back color renderbuffer */
      if (mesaVis->doubleBufferMode) {
         driRenderbuffer *backRb
            = driNewRenderbuffer(GL_RGBA,
                                 driScrnPriv->pFB + screen->backOffset,
                                 screen->cpp,
                                 screen->backOffset, screen->backPitch,
                                 driDrawPriv);
         radeonSetSpanFunctions(backRb, mesaVis);
         _mesa_add_renderbuffer(fb, BUFFER_BACK_LEFT, &backRb->Base);
      }

      /* depth renderbuffer */
      if (mesaVis->depthBits == 16) {
         driRenderbuffer *depthRb
            = driNewRenderbuffer(GL_DEPTH_COMPONENT16,
                                 driScrnPriv->pFB + screen->depthOffset,
                                 screen->cpp,
                                 screen->depthOffset, screen->depthPitch,
                                 driDrawPriv);
         radeonSetSpanFunctions(depthRb, mesaVis);
         _mesa_add_renderbuffer(fb, BUFFER_DEPTH, &depthRb->Base);
	 depthRb->depthHasSurface = screen->depthHasSurface;
      }
      else if (mesaVis->depthBits == 24) {
         driRenderbuffer *depthRb
            = driNewRenderbuffer(GL_DEPTH_COMPONENT24,
                                 driScrnPriv->pFB + screen->depthOffset,
                                 screen->cpp,
                                 screen->depthOffset, screen->depthPitch,
                                 driDrawPriv);
         radeonSetSpanFunctions(depthRb, mesaVis);
         _mesa_add_renderbuffer(fb, BUFFER_DEPTH, &depthRb->Base);
	 depthRb->depthHasSurface = screen->depthHasSurface;
      }

      /* stencil renderbuffer */
      if (mesaVis->stencilBits > 0 && !swStencil) {
         driRenderbuffer *stencilRb
            = driNewRenderbuffer(GL_STENCIL_INDEX8_EXT,
                                 driScrnPriv->pFB + screen->depthOffset,
                                 screen->cpp,
                                 screen->depthOffset, screen->depthPitch,
                                 driDrawPriv);
         radeonSetSpanFunctions(stencilRb, mesaVis);
         _mesa_add_renderbuffer(fb, BUFFER_STENCIL, &stencilRb->Base);
	 stencilRb->depthHasSurface = screen->depthHasSurface;
      }

      _mesa_add_soft_renderbuffers(fb,
                                   GL_FALSE, /* color */
                                   swDepth,
                                   swStencil,
                                   swAccum,
                                   swAlpha,
                                   GL_FALSE /* aux */);
      driDrawPriv->driverPrivate = (void *) fb;

      return (driDrawPriv->driverPrivate != NULL);
   }
}


static void
radeonDestroyBuffer(__DRIdrawablePrivate *driDrawPriv)
{
   _mesa_unreference_framebuffer((GLframebuffer **)(&(driDrawPriv->driverPrivate)));
}

#if RADEON_COMMON && defined(RADEON_COMMON_FOR_R300)
/**
 * Choose the appropriate CreateContext function based on the chipset.
 * Eventually, all drivers will go through this process.
 */
static GLboolean radeonCreateContext(const __GLcontextModes * glVisual,
				     __DRIcontextPrivate * driContextPriv,
				     void *sharedContextPriv)
{
	__DRIscreenPrivate *sPriv = driContextPriv->driScreenPriv;
	radeonScreenPtr screen = (radeonScreenPtr) (sPriv->private);

	if (IS_R300_CLASS(screen))
		return r300CreateContext(glVisual, driContextPriv, sharedContextPriv);
        return GL_FALSE;
}

/**
 * Choose the appropriate DestroyContext function based on the chipset.
 */
static void radeonDestroyContext(__DRIcontextPrivate * driContextPriv)
{
	radeonContextPtr radeon = (radeonContextPtr) driContextPriv->driverPrivate;

	if (IS_R300_CLASS(radeon->radeonScreen))
		return r300DestroyContext(driContextPriv);
}


#endif

/**
 * This is the driver specific part of the createNewScreen entry point.
 *
 * \todo maybe fold this into intelInitDriver
 *
 * \return the __GLcontextModes supported by this driver
 */
static const __DRIconfig **
radeonInitScreen(__DRIscreenPrivate *psp)
{
#if !RADEON_COMMON
   static const char *driver_name = "Radeon";
   static const __DRIutilversion2 ddx_expected = { 4, 5, 0, 0 };
   static const __DRIversion dri_expected = { 4, 0, 0 };
   static const __DRIversion drm_expected = { 1, 6, 0 };
#elif RADEON_COMMON && defined(RADEON_COMMON_FOR_R200)
   static const char *driver_name = "R200";
   static const __DRIutilversion2 ddx_expected = { 4, 5, 0, 0 };
   static const __DRIversion dri_expected = { 4, 0, 0 };
   static const __DRIversion drm_expected = { 1, 6, 0 };
#elif RADEON_COMMON && defined(RADEON_COMMON_FOR_R300)
   static const char *driver_name = "R300";
   static const __DRIutilversion2 ddx_expected = { 4, 5, 0, 0 };
   static const __DRIversion dri_expected = { 4, 0, 0 };
   static const __DRIversion drm_expected = { 1, 24, 0 };
#endif
   RADEONDRIPtr dri_priv = (RADEONDRIPtr) psp->pDevPriv;

   if ( ! driCheckDriDdxDrmVersions3( driver_name,
				      &psp->dri_version, & dri_expected,
				      &psp->ddx_version, & ddx_expected,
				      &psp->drm_version, & drm_expected ) ) {
      return NULL;
   }

   /* Calling driInitExtensions here, with a NULL context pointer,
    * does not actually enable the extensions.  It just makes sure
    * that all the dispatch offsets for all the extensions that
    * *might* be enables are known.  This is needed because the
    * dispatch offsets need to be known when _mesa_context_create
    * is called, but we can't enable the extensions until we have a
    * context pointer.
    *
    * Hello chicken.  Hello egg.  How are you two today?
    */
   driInitExtensions( NULL, card_extensions, GL_FALSE );
#if RADEON_COMMON && defined(RADEON_COMMON_FOR_R200)
   driInitExtensions( NULL, blend_extensions, GL_FALSE );
   driInitSingleExtension( NULL, ARB_vp_extension );
   driInitSingleExtension( NULL, NV_vp_extension );
   driInitSingleExtension( NULL, ATI_fs_extension );
   driInitExtensions( NULL, point_extensions, GL_FALSE );
#endif

   if (!radeonInitDriver(psp))
       return NULL;

   return radeonFillInModes( psp,
			     dri_priv->bpp,
			     (dri_priv->bpp == 16) ? 16 : 24,
			     (dri_priv->bpp == 16) ? 0  : 8,
			     (dri_priv->backOffset != dri_priv->depthOffset) );
}


/**
 * Get information about previous buffer swaps.
 */
static int
getSwapInfo( __DRIdrawablePrivate *dPriv, __DRIswapInfo * sInfo )
{
#if !RADEON_COMMON || (RADEON_COMMON && defined(RADEON_COMMON_FOR_R300))
   radeonContextPtr  rmesa;
#elif RADEON_COMMON && defined(RADEON_COMMON_FOR_R200)
   r200ContextPtr  rmesa;
#endif

   if ( (dPriv == NULL) || (dPriv->driContextPriv == NULL)
	|| (dPriv->driContextPriv->driverPrivate == NULL)
	|| (sInfo == NULL) ) {
      return -1;
   }

   rmesa = dPriv->driContextPriv->driverPrivate;
   sInfo->swap_count = rmesa->swap_count;
   sInfo->swap_ust = rmesa->swap_ust;
   sInfo->swap_missed_count = rmesa->swap_missed_count;

   sInfo->swap_missed_usage = (sInfo->swap_missed_count != 0)
       ? driCalculateSwapUsage( dPriv, 0, rmesa->swap_missed_ust )
       : 0.0;

   return 0;
}

#if !RADEON_COMMON || (RADEON_COMMON && defined(RADEON_COMMON_FOR_R300))
const struct __DriverAPIRec driDriverAPI = {
   .InitScreen      = radeonInitScreen,
   .DestroyScreen   = radeonDestroyScreen,
   .CreateContext   = radeonCreateContext,
   .DestroyContext  = radeonDestroyContext,
   .CreateBuffer    = radeonCreateBuffer,
   .DestroyBuffer   = radeonDestroyBuffer,
   .SwapBuffers     = radeonSwapBuffers,
   .MakeCurrent     = radeonMakeCurrent,
   .UnbindContext   = radeonUnbindContext,
   .GetSwapInfo     = getSwapInfo,
   .GetDrawableMSC  = driDrawableGetMSC32,
   .WaitForMSC      = driWaitForMSC32,
   .WaitForSBC      = NULL,
   .SwapBuffersMSC  = NULL,
   .CopySubBuffer   = radeonCopySubBuffer,
};
#else
const struct __DriverAPIRec driDriverAPI = {
   .InitScreen      = radeonInitScreen,
   .DestroyScreen   = radeonDestroyScreen,
   .CreateContext   = r200CreateContext,
   .DestroyContext  = r200DestroyContext,
   .CreateBuffer    = radeonCreateBuffer,
   .DestroyBuffer   = radeonDestroyBuffer,
   .SwapBuffers     = r200SwapBuffers,
   .MakeCurrent     = r200MakeCurrent,
   .UnbindContext   = r200UnbindContext,
   .GetSwapInfo     = getSwapInfo,
   .GetDrawableMSC  = driDrawableGetMSC32,
   .WaitForMSC      = driWaitForMSC32,
   .WaitForSBC      = NULL,
   .SwapBuffersMSC  = NULL,
   .CopySubBuffer   = r200CopySubBuffer,
};
#endif
