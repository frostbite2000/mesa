/*
Copyright (C) The Weather Channel, Inc.  2002.
Copyright (C) 2004 Nicolai Haehnle.
All Rights Reserved.

The Weather Channel (TM) funded Tungsten Graphics to develop the
initial release of the Radeon 8500 driver under the XFree86 license.
This notice must be preserved.

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
 * \file
 *
 * \author Nicolai Haehnle <prefect_@gmx.net>
 */

#include "main/glheader.h"
#include "main/state.h"
#include "main/imports.h"
#include "main/enums.h"
#include "main/macros.h"
#include "main/context.h"
#include "main/dd.h"
#include "main/simple_list.h"
#include "main/api_arrayelt.h"
#include "main/texformat.h"

#include "swrast/swrast.h"
#include "swrast_setup/swrast_setup.h"
#include "shader/prog_parameter.h"
#include "shader/prog_statevars.h"
#include "vbo/vbo.h"
#include "tnl/tnl.h"

#include "radeon_ioctl.h"
#include "radeon_state.h"
#include "r300_context.h"
#include "r300_ioctl.h"
#include "r300_state.h"
#include "r300_reg.h"
#include "r300_emit.h"
#include "r300_fragprog.h"
#include "r300_tex.h"

#include "drirenderbuffer.h"

extern int future_hw_tcl_on;
extern void _tnl_UpdateFixedFunctionProgram(GLcontext * ctx);

static void r300BlendColor(GLcontext * ctx, const GLfloat cf[4])
{
	r300ContextPtr rmesa = R300_CONTEXT(ctx);

	R300_STATECHANGE(rmesa, blend_color);

	if (rmesa->radeon.radeonScreen->chip_family >= CHIP_FAMILY_RV515) {
		GLuint r = IROUND(cf[0]*1023.0f);
		GLuint g = IROUND(cf[1]*1023.0f);
		GLuint b = IROUND(cf[2]*1023.0f);
		GLuint a = IROUND(cf[3]*1023.0f);

		rmesa->hw.blend_color.cmd[1] = r | (a << 16);
		rmesa->hw.blend_color.cmd[2] = b | (g << 16);
	} else {
		GLubyte color[4];
		CLAMPED_FLOAT_TO_UBYTE(color[0], cf[0]);
		CLAMPED_FLOAT_TO_UBYTE(color[1], cf[1]);
		CLAMPED_FLOAT_TO_UBYTE(color[2], cf[2]);
		CLAMPED_FLOAT_TO_UBYTE(color[3], cf[3]);

		rmesa->hw.blend_color.cmd[1] = PACK_COLOR_8888(color[3], color[0],
							color[1], color[2]);
	}
}

/**
 * Calculate the hardware blend factor setting.  This same function is used
 * for source and destination of both alpha and RGB.
 *
 * \returns
 * The hardware register value for the specified blend factor.  This value
 * will need to be shifted into the correct position for either source or
 * destination factor.
 *
 * \todo
 * Since the two cases where source and destination are handled differently
 * are essentially error cases, they should never happen.  Determine if these
 * cases can be removed.
 */
static int blend_factor(GLenum factor, GLboolean is_src)
{
	switch (factor) {
	case GL_ZERO:
		return R300_BLEND_GL_ZERO;
		break;
	case GL_ONE:
		return R300_BLEND_GL_ONE;
		break;
	case GL_DST_COLOR:
		return R300_BLEND_GL_DST_COLOR;
		break;
	case GL_ONE_MINUS_DST_COLOR:
		return R300_BLEND_GL_ONE_MINUS_DST_COLOR;
		break;
	case GL_SRC_COLOR:
		return R300_BLEND_GL_SRC_COLOR;
		break;
	case GL_ONE_MINUS_SRC_COLOR:
		return R300_BLEND_GL_ONE_MINUS_SRC_COLOR;
		break;
	case GL_SRC_ALPHA:
		return R300_BLEND_GL_SRC_ALPHA;
		break;
	case GL_ONE_MINUS_SRC_ALPHA:
		return R300_BLEND_GL_ONE_MINUS_SRC_ALPHA;
		break;
	case GL_DST_ALPHA:
		return R300_BLEND_GL_DST_ALPHA;
		break;
	case GL_ONE_MINUS_DST_ALPHA:
		return R300_BLEND_GL_ONE_MINUS_DST_ALPHA;
		break;
	case GL_SRC_ALPHA_SATURATE:
		return (is_src) ? R300_BLEND_GL_SRC_ALPHA_SATURATE :
		    R300_BLEND_GL_ZERO;
		break;
	case GL_CONSTANT_COLOR:
		return R300_BLEND_GL_CONST_COLOR;
		break;
	case GL_ONE_MINUS_CONSTANT_COLOR:
		return R300_BLEND_GL_ONE_MINUS_CONST_COLOR;
		break;
	case GL_CONSTANT_ALPHA:
		return R300_BLEND_GL_CONST_ALPHA;
		break;
	case GL_ONE_MINUS_CONSTANT_ALPHA:
		return R300_BLEND_GL_ONE_MINUS_CONST_ALPHA;
		break;
	default:
		fprintf(stderr, "unknown blend factor %x\n", factor);
		return (is_src) ? R300_BLEND_GL_ONE : R300_BLEND_GL_ZERO;
		break;
	}
}

/**
 * Sets both the blend equation and the blend function.
 * This is done in a single
 * function because some blend equations (i.e., \c GL_MIN and \c GL_MAX)
 * change the interpretation of the blend function.
 * Also, make sure that blend function and blend equation are set to their
 * default value if color blending is not enabled, since at least blend
 * equations GL_MIN and GL_FUNC_REVERSE_SUBTRACT will cause wrong results
 * otherwise for unknown reasons.
 */

/* helper function */
static void r300SetBlendCntl(r300ContextPtr r300, int func, int eqn,
			     int cbits, int funcA, int eqnA)
{
	GLuint new_ablend, new_cblend;

#if 0
	fprintf(stderr,
		"eqnA=%08x funcA=%08x eqn=%08x func=%08x cbits=%08x\n",
		eqnA, funcA, eqn, func, cbits);
#endif
	new_ablend = eqnA | funcA;
	new_cblend = eqn | func;

	/* Some blend factor combinations don't seem to work when the
	 * BLEND_NO_SEPARATE bit is set.
	 *
	 * Especially problematic candidates are the ONE_MINUS_* flags,
	 * but I can't see a real pattern.
	 */
#if 0
	if (new_ablend == new_cblend) {
		new_cblend |= R300_DISCARD_SRC_PIXELS_SRC_ALPHA_0;
	}
#endif
	new_cblend |= cbits;

	if ((new_ablend != r300->hw.bld.cmd[R300_BLD_ABLEND]) ||
	    (new_cblend != r300->hw.bld.cmd[R300_BLD_CBLEND])) {
		R300_STATECHANGE(r300, bld);
		r300->hw.bld.cmd[R300_BLD_ABLEND] = new_ablend;
		r300->hw.bld.cmd[R300_BLD_CBLEND] = new_cblend;
	}
}

static void r300SetBlendState(GLcontext * ctx)
{
	r300ContextPtr r300 = R300_CONTEXT(ctx);
	int func = (R300_BLEND_GL_ONE << R300_SRC_BLEND_SHIFT) |
	    (R300_BLEND_GL_ZERO << R300_DST_BLEND_SHIFT);
	int eqn = R300_COMB_FCN_ADD_CLAMP;
	int funcA = (R300_BLEND_GL_ONE << R300_SRC_BLEND_SHIFT) |
	    (R300_BLEND_GL_ZERO << R300_DST_BLEND_SHIFT);
	int eqnA = R300_COMB_FCN_ADD_CLAMP;

	if (RGBA_LOGICOP_ENABLED(ctx) || !ctx->Color.BlendEnabled) {
		r300SetBlendCntl(r300, func, eqn, 0, func, eqn);
		return;
	}

	func =
	    (blend_factor(ctx->Color.BlendSrcRGB, GL_TRUE) <<
	     R300_SRC_BLEND_SHIFT) | (blend_factor(ctx->Color.BlendDstRGB,
						   GL_FALSE) <<
				      R300_DST_BLEND_SHIFT);

	switch (ctx->Color.BlendEquationRGB) {
	case GL_FUNC_ADD:
		eqn = R300_COMB_FCN_ADD_CLAMP;
		break;

	case GL_FUNC_SUBTRACT:
		eqn = R300_COMB_FCN_SUB_CLAMP;
		break;

	case GL_FUNC_REVERSE_SUBTRACT:
		eqn = R300_COMB_FCN_RSUB_CLAMP;
		break;

	case GL_MIN:
		eqn = R300_COMB_FCN_MIN;
		func = (R300_BLEND_GL_ONE << R300_SRC_BLEND_SHIFT) |
		    (R300_BLEND_GL_ONE << R300_DST_BLEND_SHIFT);
		break;

	case GL_MAX:
		eqn = R300_COMB_FCN_MAX;
		func = (R300_BLEND_GL_ONE << R300_SRC_BLEND_SHIFT) |
		    (R300_BLEND_GL_ONE << R300_DST_BLEND_SHIFT);
		break;

	default:
		fprintf(stderr,
			"[%s:%u] Invalid RGB blend equation (0x%04x).\n",
			__FUNCTION__, __LINE__, ctx->Color.BlendEquationRGB);
		return;
	}

	funcA =
	    (blend_factor(ctx->Color.BlendSrcA, GL_TRUE) <<
	     R300_SRC_BLEND_SHIFT) | (blend_factor(ctx->Color.BlendDstA,
						   GL_FALSE) <<
				      R300_DST_BLEND_SHIFT);

	switch (ctx->Color.BlendEquationA) {
	case GL_FUNC_ADD:
		eqnA = R300_COMB_FCN_ADD_CLAMP;
		break;

	case GL_FUNC_SUBTRACT:
		eqnA = R300_COMB_FCN_SUB_CLAMP;
		break;

	case GL_FUNC_REVERSE_SUBTRACT:
		eqnA = R300_COMB_FCN_RSUB_CLAMP;
		break;

	case GL_MIN:
		eqnA = R300_COMB_FCN_MIN;
		funcA = (R300_BLEND_GL_ONE << R300_SRC_BLEND_SHIFT) |
		    (R300_BLEND_GL_ONE << R300_DST_BLEND_SHIFT);
		break;

	case GL_MAX:
		eqnA = R300_COMB_FCN_MAX;
		funcA = (R300_BLEND_GL_ONE << R300_SRC_BLEND_SHIFT) |
		    (R300_BLEND_GL_ONE << R300_DST_BLEND_SHIFT);
		break;

	default:
		fprintf(stderr,
			"[%s:%u] Invalid A blend equation (0x%04x).\n",
			__FUNCTION__, __LINE__, ctx->Color.BlendEquationA);
		return;
	}

	r300SetBlendCntl(r300,
			 func, eqn,
			 (R300_SEPARATE_ALPHA_ENABLE |
			  R300_READ_ENABLE |
			  R300_ALPHA_BLEND_ENABLE), funcA, eqnA);
}

static void r300BlendEquationSeparate(GLcontext * ctx,
				      GLenum modeRGB, GLenum modeA)
{
	r300SetBlendState(ctx);
}

static void r300BlendFuncSeparate(GLcontext * ctx,
				  GLenum sfactorRGB, GLenum dfactorRGB,
				  GLenum sfactorA, GLenum dfactorA)
{
	r300SetBlendState(ctx);
}

/**
 * Translate LogicOp enums into hardware representation.
 * Both use a very logical bit-wise layout, but unfortunately the order
 * of bits is reversed.
 */
static GLuint translate_logicop(GLenum logicop)
{
	GLuint bits = logicop - GL_CLEAR;
	bits = ((bits & 1) << 3) | ((bits & 2) << 1) | ((bits & 4) >> 1) | ((bits & 8) >> 3);
	return bits << R300_RB3D_ROPCNTL_ROP_SHIFT;
}

/**
 * Used internally to update the r300->hw hardware state to match the
 * current OpenGL state.
 */
static void r300SetLogicOpState(GLcontext *ctx)
{
	r300ContextPtr r300 = R300_CONTEXT(ctx);
	R300_STATECHANGE(r300, rop);
	if (RGBA_LOGICOP_ENABLED(ctx)) {
		r300->hw.rop.cmd[1] = R300_RB3D_ROPCNTL_ROP_ENABLE |
			translate_logicop(ctx->Color.LogicOp);
	} else {
		r300->hw.rop.cmd[1] = 0;
	}
}

/**
 * Called by Mesa when an application program changes the LogicOp state
 * via glLogicOp.
 */
static void r300LogicOpcode(GLcontext *ctx, GLenum logicop)
{
	if (RGBA_LOGICOP_ENABLED(ctx))
		r300SetLogicOpState(ctx);
}

static void r300ClipPlane( GLcontext *ctx, GLenum plane, const GLfloat *eq )
{
	r300ContextPtr rmesa = R300_CONTEXT(ctx);
	GLint p;
	GLint *ip;

	/* no VAP UCP on non-TCL chipsets */
	if (!(rmesa->radeon.radeonScreen->chip_flags & RADEON_CHIPSET_TCL))
			return;

	p = (GLint) plane - (GLint) GL_CLIP_PLANE0;
	ip = (GLint *)ctx->Transform._ClipUserPlane[p];

	R300_STATECHANGE( rmesa, vpucp[p] );
	rmesa->hw.vpucp[p].cmd[R300_VPUCP_X] = ip[0];
	rmesa->hw.vpucp[p].cmd[R300_VPUCP_Y] = ip[1];
	rmesa->hw.vpucp[p].cmd[R300_VPUCP_Z] = ip[2];
	rmesa->hw.vpucp[p].cmd[R300_VPUCP_W] = ip[3];
}

static void r300SetClipPlaneState(GLcontext * ctx, GLenum cap, GLboolean state)
{
	r300ContextPtr r300 = R300_CONTEXT(ctx);
	GLuint p;

	/* no VAP UCP on non-TCL chipsets */
	if (!(r300->radeon.radeonScreen->chip_flags & RADEON_CHIPSET_TCL))
		return;

	p = cap - GL_CLIP_PLANE0;
	R300_STATECHANGE(r300, vap_clip_cntl);
	if (state) {
		r300->hw.vap_clip_cntl.cmd[1] |= (R300_VAP_UCP_ENABLE_0 << p);
		r300ClipPlane(ctx, cap, NULL);
	} else {
		r300->hw.vap_clip_cntl.cmd[1] &= ~(R300_VAP_UCP_ENABLE_0 << p);
	}
}

/**
 * Update our tracked culling state based on Mesa's state.
 */
static void r300UpdateCulling(GLcontext * ctx)
{
	r300ContextPtr r300 = R300_CONTEXT(ctx);
	uint32_t val = 0;

	if (ctx->Polygon.CullFlag) {
		switch (ctx->Polygon.CullFaceMode) {
		case GL_FRONT:
			val = R300_CULL_FRONT;
			break;
		case GL_BACK:
			val = R300_CULL_BACK;
			break;
		case GL_FRONT_AND_BACK:
			val = R300_CULL_FRONT | R300_CULL_BACK;
			break;
		default:
			break;
		}
	}

	switch (ctx->Polygon.FrontFace) {
	case GL_CW:
		val |= R300_FRONT_FACE_CW;
		break;
	case GL_CCW:
		val |= R300_FRONT_FACE_CCW;
		break;
	default:
		break;
	}

	R300_STATECHANGE(r300, cul);
	r300->hw.cul.cmd[R300_CUL_CULL] = val;
}

static void r300SetPolygonOffsetState(GLcontext * ctx, GLboolean state)
{
	r300ContextPtr r300 = R300_CONTEXT(ctx);

	R300_STATECHANGE(r300, occlusion_cntl);
	if (state) {
		r300->hw.occlusion_cntl.cmd[1] |= (3 << 0);
	} else {
		r300->hw.occlusion_cntl.cmd[1] &= ~(3 << 0);
	}
}

static GLboolean current_fragment_program_writes_depth(GLcontext* ctx)
{
	r300ContextPtr r300 = R300_CONTEXT(ctx);

	if (r300->radeon.radeonScreen->chip_family < CHIP_FAMILY_RV515) {
		struct r300_fragment_program *fp = (struct r300_fragment_program *)
			(char *)ctx->FragmentProgram._Current;
		return (fp && fp->WritesDepth);
	} else {
		struct r500_fragment_program* fp =
			(struct r500_fragment_program*)(char*)
			ctx->FragmentProgram._Current;
		return (fp && fp->writes_depth);
	}
}

static void r300SetEarlyZState(GLcontext * ctx)
{
	r300ContextPtr r300 = R300_CONTEXT(ctx);
	GLuint topZ = R300_ZTOP_ENABLE;

	if (ctx->Color.AlphaEnabled && ctx->Color.AlphaFunc != GL_ALWAYS)
		topZ = R300_ZTOP_DISABLE;
	if (current_fragment_program_writes_depth(ctx))
		topZ = R300_ZTOP_DISABLE;

	if (topZ != r300->hw.zstencil_format.cmd[2]) {
		/* Note: This completely reemits the stencil format.
		 * I have not tested whether this is strictly necessary,
		 * or if emitting a write to ZB_ZTOP is enough.
		 */
		R300_STATECHANGE(r300, zstencil_format);
		r300->hw.zstencil_format.cmd[2] = topZ;
	}
}

static void r300SetAlphaState(GLcontext * ctx)
{
	r300ContextPtr r300 = R300_CONTEXT(ctx);
	GLubyte refByte;
	uint32_t pp_misc = 0x0;
	GLboolean really_enabled = ctx->Color.AlphaEnabled;

	CLAMPED_FLOAT_TO_UBYTE(refByte, ctx->Color.AlphaRef);

	switch (ctx->Color.AlphaFunc) {
	case GL_NEVER:
		pp_misc |= R300_FG_ALPHA_FUNC_NEVER;
		break;
	case GL_LESS:
		pp_misc |= R300_FG_ALPHA_FUNC_LESS;
		break;
	case GL_EQUAL:
		pp_misc |= R300_FG_ALPHA_FUNC_EQUAL;
		break;
	case GL_LEQUAL:
		pp_misc |= R300_FG_ALPHA_FUNC_LE;
		break;
	case GL_GREATER:
		pp_misc |= R300_FG_ALPHA_FUNC_GREATER;
		break;
	case GL_NOTEQUAL:
		pp_misc |= R300_FG_ALPHA_FUNC_NOTEQUAL;
		break;
	case GL_GEQUAL:
		pp_misc |= R300_FG_ALPHA_FUNC_GE;
		break;
	case GL_ALWAYS:
		/*pp_misc |= FG_ALPHA_FUNC_ALWAYS; */
		really_enabled = GL_FALSE;
		break;
	}

	if (really_enabled) {
		pp_misc |= R300_FG_ALPHA_FUNC_ENABLE;
		pp_misc |= R500_FG_ALPHA_FUNC_8BIT;
		pp_misc |= (refByte & R300_FG_ALPHA_FUNC_VAL_MASK);
	} else {
		pp_misc = 0x0;
	}

	R300_STATECHANGE(r300, at);
	r300->hw.at.cmd[R300_AT_ALPHA_TEST] = pp_misc;
	r300->hw.at.cmd[R300_AT_UNKNOWN] = 0;

	r300SetEarlyZState(ctx);
}

static void r300AlphaFunc(GLcontext * ctx, GLenum func, GLfloat ref)
{
	(void)func;
	(void)ref;
	r300SetAlphaState(ctx);
}

static int translate_func(int func)
{
	switch (func) {
	case GL_NEVER:
		return R300_ZS_NEVER;
	case GL_LESS:
		return R300_ZS_LESS;
	case GL_EQUAL:
		return R300_ZS_EQUAL;
	case GL_LEQUAL:
		return R300_ZS_LEQUAL;
	case GL_GREATER:
		return R300_ZS_GREATER;
	case GL_NOTEQUAL:
		return R300_ZS_NOTEQUAL;
	case GL_GEQUAL:
		return R300_ZS_GEQUAL;
	case GL_ALWAYS:
		return R300_ZS_ALWAYS;
	}
	return 0;
}

static void r300SetDepthState(GLcontext * ctx)
{
	r300ContextPtr r300 = R300_CONTEXT(ctx);

	R300_STATECHANGE(r300, zs);
	r300->hw.zs.cmd[R300_ZS_CNTL_0] &= R300_STENCIL_ENABLE|R300_STENCIL_FRONT_BACK;
	r300->hw.zs.cmd[R300_ZS_CNTL_1] &= ~(R300_ZS_MASK << R300_Z_FUNC_SHIFT);

	if (ctx->Depth.Test) {
		r300->hw.zs.cmd[R300_ZS_CNTL_0] |= R300_Z_ENABLE;
		if (ctx->Depth.Mask)
			r300->hw.zs.cmd[R300_ZS_CNTL_0] |= R300_Z_WRITE_ENABLE;
		r300->hw.zs.cmd[R300_ZS_CNTL_1] |=
		    translate_func(ctx->Depth.Func) << R300_Z_FUNC_SHIFT;
	}

	r300SetEarlyZState(ctx);
}

static void r300SetStencilState(GLcontext * ctx, GLboolean state)
{
	r300ContextPtr r300 = R300_CONTEXT(ctx);

	if (r300->state.stencil.hw_stencil) {
		R300_STATECHANGE(r300, zs);
		if (state) {
			r300->hw.zs.cmd[R300_ZS_CNTL_0] |=
			    R300_STENCIL_ENABLE;
		} else {
			r300->hw.zs.cmd[R300_ZS_CNTL_0] &=
			    ~R300_STENCIL_ENABLE;
		}
	} else {
#if R200_MERGED
		FALLBACK(&r300->radeon, RADEON_FALLBACK_STENCIL, state);
#endif
	}
}

static void r300UpdatePolygonMode(GLcontext * ctx)
{
	r300ContextPtr r300 = R300_CONTEXT(ctx);
	uint32_t hw_mode = R300_GA_POLY_MODE_DISABLE;

	/* Only do something if a polygon mode is wanted, default is GL_FILL */
	if (ctx->Polygon.FrontMode != GL_FILL ||
	    ctx->Polygon.BackMode != GL_FILL) {
		GLenum f, b;

		/* Handle GL_CW (clock wise and GL_CCW (counter clock wise)
		 * correctly by selecting the correct front and back face
		 */
		if (ctx->Polygon.FrontFace == GL_CCW) {
			f = ctx->Polygon.FrontMode;
			b = ctx->Polygon.BackMode;
		} else {
			f = ctx->Polygon.BackMode;
			b = ctx->Polygon.FrontMode;
		}

		/* Enable polygon mode */
		hw_mode |= R300_GA_POLY_MODE_DUAL;

		switch (f) {
		case GL_LINE:
			hw_mode |= R300_GA_POLY_MODE_FRONT_PTYPE_LINE;
			break;
		case GL_POINT:
			hw_mode |= R300_GA_POLY_MODE_FRONT_PTYPE_POINT;
			break;
		case GL_FILL:
			hw_mode |= R300_GA_POLY_MODE_FRONT_PTYPE_TRI;
			break;
		}

		switch (b) {
		case GL_LINE:
			hw_mode |= R300_GA_POLY_MODE_BACK_PTYPE_LINE;
			break;
		case GL_POINT:
			hw_mode |= R300_GA_POLY_MODE_BACK_PTYPE_POINT;
			break;
		case GL_FILL:
			hw_mode |= R300_GA_POLY_MODE_BACK_PTYPE_TRI;
			break;
		}
	}

	if (r300->hw.polygon_mode.cmd[1] != hw_mode) {
		R300_STATECHANGE(r300, polygon_mode);
		r300->hw.polygon_mode.cmd[1] = hw_mode;
	}

	r300->hw.polygon_mode.cmd[2] = 0x00000001;
	r300->hw.polygon_mode.cmd[3] = 0x00000000;
}

/**
 * Change the culling mode.
 *
 * \note Mesa already filters redundant calls to this function.
 */
static void r300CullFace(GLcontext * ctx, GLenum mode)
{
	(void)mode;

	r300UpdateCulling(ctx);
}

/**
 * Change the polygon orientation.
 *
 * \note Mesa already filters redundant calls to this function.
 */
static void r300FrontFace(GLcontext * ctx, GLenum mode)
{
	(void)mode;

	r300UpdateCulling(ctx);
	r300UpdatePolygonMode(ctx);
}

/**
 * Change the depth testing function.
 *
 * \note Mesa already filters redundant calls to this function.
 */
static void r300DepthFunc(GLcontext * ctx, GLenum func)
{
	(void)func;
	r300SetDepthState(ctx);
}

/**
 * Enable/Disable depth writing.
 *
 * \note Mesa already filters redundant calls to this function.
 */
static void r300DepthMask(GLcontext * ctx, GLboolean mask)
{
	(void)mask;
	r300SetDepthState(ctx);
}

/**
 * Handle glColorMask()
 */
static void r300ColorMask(GLcontext * ctx,
			  GLboolean r, GLboolean g, GLboolean b, GLboolean a)
{
	r300ContextPtr r300 = R300_CONTEXT(ctx);
	int mask = (r ? RB3D_COLOR_CHANNEL_MASK_RED_MASK0 : 0) |
	    (g ? RB3D_COLOR_CHANNEL_MASK_GREEN_MASK0 : 0) |
	    (b ? RB3D_COLOR_CHANNEL_MASK_BLUE_MASK0 : 0) |
	    (a ? RB3D_COLOR_CHANNEL_MASK_ALPHA_MASK0 : 0);

	if (mask != r300->hw.cmk.cmd[R300_CMK_COLORMASK]) {
		R300_STATECHANGE(r300, cmk);
		r300->hw.cmk.cmd[R300_CMK_COLORMASK] = mask;
	}
}

/* =============================================================
 * Fog
 */
static void r300Fogfv(GLcontext * ctx, GLenum pname, const GLfloat * param)
{
	r300ContextPtr r300 = R300_CONTEXT(ctx);
	union {
		int i;
		float f;
	} fogScale, fogStart;

	(void)param;

	fogScale.i = r300->hw.fogp.cmd[R300_FOGP_SCALE];
	fogStart.i = r300->hw.fogp.cmd[R300_FOGP_START];

	switch (pname) {
	case GL_FOG_MODE:
		switch (ctx->Fog.Mode) {
		case GL_LINEAR:
			R300_STATECHANGE(r300, fogs);
			r300->hw.fogs.cmd[R300_FOGS_STATE] =
			    (r300->hw.fogs.
			     cmd[R300_FOGS_STATE] & ~R300_FG_FOG_BLEND_FN_MASK) |
			    R300_FG_FOG_BLEND_FN_LINEAR;

			if (ctx->Fog.Start == ctx->Fog.End) {
				fogScale.f = -1.0;
				fogStart.f = 1.0;
			} else {
				fogScale.f =
				    1.0 / (ctx->Fog.End - ctx->Fog.Start);
				fogStart.f =
				    -ctx->Fog.Start / (ctx->Fog.End -
						       ctx->Fog.Start);
			}
			break;
		case GL_EXP:
			R300_STATECHANGE(r300, fogs);
			r300->hw.fogs.cmd[R300_FOGS_STATE] =
			    (r300->hw.fogs.
			     cmd[R300_FOGS_STATE] & ~R300_FG_FOG_BLEND_FN_MASK) |
			    R300_FG_FOG_BLEND_FN_EXP;
			fogScale.f = 0.0933 * ctx->Fog.Density;
			fogStart.f = 0.0;
			break;
		case GL_EXP2:
			R300_STATECHANGE(r300, fogs);
			r300->hw.fogs.cmd[R300_FOGS_STATE] =
			    (r300->hw.fogs.
			     cmd[R300_FOGS_STATE] & ~R300_FG_FOG_BLEND_FN_MASK) |
			    R300_FG_FOG_BLEND_FN_EXP2;
			fogScale.f = 0.3 * ctx->Fog.Density;
			fogStart.f = 0.0;
		default:
			return;
		}
		break;
	case GL_FOG_DENSITY:
		switch (ctx->Fog.Mode) {
		case GL_EXP:
			fogScale.f = 0.0933 * ctx->Fog.Density;
			fogStart.f = 0.0;
			break;
		case GL_EXP2:
			fogScale.f = 0.3 * ctx->Fog.Density;
			fogStart.f = 0.0;
		default:
			break;
		}
		break;
	case GL_FOG_START:
	case GL_FOG_END:
		if (ctx->Fog.Mode == GL_LINEAR) {
			if (ctx->Fog.Start == ctx->Fog.End) {
				fogScale.f = -1.0;
				fogStart.f = 1.0;
			} else {
				fogScale.f =
				    1.0 / (ctx->Fog.End - ctx->Fog.Start);
				fogStart.f =
				    -ctx->Fog.Start / (ctx->Fog.End -
						       ctx->Fog.Start);
			}
		}
		break;
	case GL_FOG_COLOR:
		R300_STATECHANGE(r300, fogc);
		r300->hw.fogc.cmd[R300_FOGC_R] =
		    (GLuint) (ctx->Fog.Color[0] * 1023.0F) & 0x3FF;
		r300->hw.fogc.cmd[R300_FOGC_G] =
		    (GLuint) (ctx->Fog.Color[1] * 1023.0F) & 0x3FF;
		r300->hw.fogc.cmd[R300_FOGC_B] =
		    (GLuint) (ctx->Fog.Color[2] * 1023.0F) & 0x3FF;
		break;
	case GL_FOG_COORD_SRC:
		break;
	default:
		return;
	}

	if (fogScale.i != r300->hw.fogp.cmd[R300_FOGP_SCALE] ||
	    fogStart.i != r300->hw.fogp.cmd[R300_FOGP_START]) {
		R300_STATECHANGE(r300, fogp);
		r300->hw.fogp.cmd[R300_FOGP_SCALE] = fogScale.i;
		r300->hw.fogp.cmd[R300_FOGP_START] = fogStart.i;
	}
}

static void r300SetFogState(GLcontext * ctx, GLboolean state)
{
	r300ContextPtr r300 = R300_CONTEXT(ctx);

	R300_STATECHANGE(r300, fogs);
	if (state) {
		r300->hw.fogs.cmd[R300_FOGS_STATE] |= R300_FG_FOG_BLEND_ENABLE;

		r300Fogfv(ctx, GL_FOG_MODE, NULL);
		r300Fogfv(ctx, GL_FOG_DENSITY, &ctx->Fog.Density);
		r300Fogfv(ctx, GL_FOG_START, &ctx->Fog.Start);
		r300Fogfv(ctx, GL_FOG_END, &ctx->Fog.End);
		r300Fogfv(ctx, GL_FOG_COLOR, ctx->Fog.Color);
	} else {
		r300->hw.fogs.cmd[R300_FOGS_STATE] &= ~R300_FG_FOG_BLEND_ENABLE;
	}
}

/* =============================================================
 * Point state
 */
static void r300PointSize(GLcontext * ctx, GLfloat size)
{
	r300ContextPtr r300 = R300_CONTEXT(ctx);
        /* same size limits for AA, non-AA points */
	size = CLAMP(size, ctx->Const.MinPointSize, ctx->Const.MaxPointSize);

	R300_STATECHANGE(r300, ps);
	r300->hw.ps.cmd[R300_PS_POINTSIZE] =
	    ((int)(size * 6) << R300_POINTSIZE_X_SHIFT) |
	    ((int)(size * 6) << R300_POINTSIZE_Y_SHIFT);
}

static void r300PointParameter(GLcontext * ctx, GLenum pname, const GLfloat * param)
{
	r300ContextPtr r300 = R300_CONTEXT(ctx);

	switch (pname) {
	case GL_POINT_SIZE_MIN:
		R300_STATECHANGE(r300, ga_point_minmax);
		r300->hw.ga_point_minmax.cmd[1] &= ~R300_GA_POINT_MINMAX_MIN_MASK;
		r300->hw.ga_point_minmax.cmd[1] |= (GLuint)(ctx->Point.MinSize * 6.0);
		break;
	case GL_POINT_SIZE_MAX:
		R300_STATECHANGE(r300, ga_point_minmax);
		r300->hw.ga_point_minmax.cmd[1] &= ~R300_GA_POINT_MINMAX_MAX_MASK;
		r300->hw.ga_point_minmax.cmd[1] |= (GLuint)(ctx->Point.MaxSize * 6.0)
			<< R300_GA_POINT_MINMAX_MAX_SHIFT;
		break;
	case GL_POINT_DISTANCE_ATTENUATION:
		break;
	case GL_POINT_FADE_THRESHOLD_SIZE:
		break;
	default:
		break;
	}
}

/* =============================================================
 * Line state
 */
static void r300LineWidth(GLcontext * ctx, GLfloat widthf)
{
	r300ContextPtr r300 = R300_CONTEXT(ctx);

	widthf = CLAMP(widthf,
                       ctx->Const.MinPointSize,
                       ctx->Const.MaxPointSize);
	R300_STATECHANGE(r300, lcntl);
	r300->hw.lcntl.cmd[1] =
	    R300_LINE_CNT_HO | R300_LINE_CNT_VE | (int)(widthf * 6.0);
}

static void r300PolygonMode(GLcontext * ctx, GLenum face, GLenum mode)
{
	(void)face;
	(void)mode;

	r300UpdatePolygonMode(ctx);
}

/* =============================================================
 * Stencil
 */

static int translate_stencil_op(int op)
{
	switch (op) {
	case GL_KEEP:
		return R300_ZS_KEEP;
	case GL_ZERO:
		return R300_ZS_ZERO;
	case GL_REPLACE:
		return R300_ZS_REPLACE;
	case GL_INCR:
		return R300_ZS_INCR;
	case GL_DECR:
		return R300_ZS_DECR;
	case GL_INCR_WRAP_EXT:
		return R300_ZS_INCR_WRAP;
	case GL_DECR_WRAP_EXT:
		return R300_ZS_DECR_WRAP;
	case GL_INVERT:
		return R300_ZS_INVERT;
	default:
		WARN_ONCE("Do not know how to translate stencil op");
		return R300_ZS_KEEP;
	}
	return 0;
}

static void r300ShadeModel(GLcontext * ctx, GLenum mode)
{
	r300ContextPtr rmesa = R300_CONTEXT(ctx);

	R300_STATECHANGE(rmesa, shade);
	rmesa->hw.shade.cmd[1] = 0x00000002;
	switch (mode) {
	case GL_FLAT:
		rmesa->hw.shade.cmd[2] = R300_RE_SHADE_MODEL_FLAT;
		break;
	case GL_SMOOTH:
		rmesa->hw.shade.cmd[2] = R300_RE_SHADE_MODEL_SMOOTH;
		break;
	default:
		return;
	}
	rmesa->hw.shade.cmd[3] = 0x00000000;
	rmesa->hw.shade.cmd[4] = 0x00000000;
}

static void r300StencilFuncSeparate(GLcontext * ctx, GLenum face,
				    GLenum func, GLint ref, GLuint mask)
{
	r300ContextPtr rmesa = R300_CONTEXT(ctx);
	GLuint refmask =
	    (((ctx->Stencil.
	       Ref[0] & 0xff) << R300_STENCILREF_SHIFT) | ((ctx->
							    Stencil.
							    ValueMask
							    [0] &
							    0xff)
							   <<
							   R300_STENCILMASK_SHIFT));

	GLuint flag;

	R300_STATECHANGE(rmesa, zs);
	rmesa->hw.zs.cmd[R300_ZS_CNTL_0] |= R300_STENCIL_FRONT_BACK;
	rmesa->hw.zs.cmd[R300_ZS_CNTL_1] &= ~((R300_ZS_MASK <<
					       R300_S_FRONT_FUNC_SHIFT)
					      | (R300_ZS_MASK <<
						 R300_S_BACK_FUNC_SHIFT));

	rmesa->hw.zs.cmd[R300_ZS_CNTL_2] &=
	    ~((R300_STENCILREF_MASK << R300_STENCILREF_SHIFT) |
	      (R300_STENCILREF_MASK << R300_STENCILMASK_SHIFT));

	flag = translate_func(ctx->Stencil.Function[0]);
	rmesa->hw.zs.cmd[R300_ZS_CNTL_1] |=
	    (flag << R300_S_FRONT_FUNC_SHIFT);

	if (ctx->Stencil._TestTwoSide)
		flag = translate_func(ctx->Stencil.Function[1]);

	rmesa->hw.zs.cmd[R300_ZS_CNTL_1] |=
	    (flag << R300_S_BACK_FUNC_SHIFT);
	rmesa->hw.zs.cmd[R300_ZS_CNTL_2] |= refmask;
}

static void r300StencilMaskSeparate(GLcontext * ctx, GLenum face, GLuint mask)
{
	r300ContextPtr rmesa = R300_CONTEXT(ctx);

	R300_STATECHANGE(rmesa, zs);
	rmesa->hw.zs.cmd[R300_ZS_CNTL_2] &=
	    ~(R300_STENCILREF_MASK <<
	      R300_STENCILWRITEMASK_SHIFT);
	rmesa->hw.zs.cmd[R300_ZS_CNTL_2] |=
	    (ctx->Stencil.
	     WriteMask[0] & R300_STENCILREF_MASK) <<
	     R300_STENCILWRITEMASK_SHIFT;
}

static void r300StencilOpSeparate(GLcontext * ctx, GLenum face,
				  GLenum fail, GLenum zfail, GLenum zpass)
{
	r300ContextPtr rmesa = R300_CONTEXT(ctx);

	R300_STATECHANGE(rmesa, zs);
	/* It is easier to mask what's left.. */
	rmesa->hw.zs.cmd[R300_ZS_CNTL_1] &=
	    (R300_ZS_MASK << R300_Z_FUNC_SHIFT) |
	    (R300_ZS_MASK << R300_S_FRONT_FUNC_SHIFT) |
	    (R300_ZS_MASK << R300_S_BACK_FUNC_SHIFT);

	rmesa->hw.zs.cmd[R300_ZS_CNTL_1] |=
	    (translate_stencil_op(ctx->Stencil.FailFunc[0]) <<
	     R300_S_FRONT_SFAIL_OP_SHIFT)
	    | (translate_stencil_op(ctx->Stencil.ZFailFunc[0]) <<
	       R300_S_FRONT_ZFAIL_OP_SHIFT)
	    | (translate_stencil_op(ctx->Stencil.ZPassFunc[0]) <<
	       R300_S_FRONT_ZPASS_OP_SHIFT);

	if (ctx->Stencil._TestTwoSide) {
		rmesa->hw.zs.cmd[R300_ZS_CNTL_1] |=
		    (translate_stencil_op(ctx->Stencil.FailFunc[1]) <<
		     R300_S_BACK_SFAIL_OP_SHIFT)
		    | (translate_stencil_op(ctx->Stencil.ZFailFunc[1]) <<
		       R300_S_BACK_ZFAIL_OP_SHIFT)
		    | (translate_stencil_op(ctx->Stencil.ZPassFunc[1]) <<
		       R300_S_BACK_ZPASS_OP_SHIFT);
	} else {
		rmesa->hw.zs.cmd[R300_ZS_CNTL_1] |=
		    (translate_stencil_op(ctx->Stencil.FailFunc[0]) <<
		     R300_S_BACK_SFAIL_OP_SHIFT)
		    | (translate_stencil_op(ctx->Stencil.ZFailFunc[0]) <<
		       R300_S_BACK_ZFAIL_OP_SHIFT)
		    | (translate_stencil_op(ctx->Stencil.ZPassFunc[0]) <<
		       R300_S_BACK_ZPASS_OP_SHIFT);
	}
}

/* =============================================================
 * Window position and viewport transformation
 */

/*
 * To correctly position primitives:
 */
#define SUBPIXEL_X 0.125
#define SUBPIXEL_Y 0.125

static void r300UpdateWindow(GLcontext * ctx)
{
	r300ContextPtr rmesa = R300_CONTEXT(ctx);
	__DRIdrawablePrivate *dPriv = rmesa->radeon.dri.drawable;
	GLfloat xoffset = dPriv ? (GLfloat) dPriv->x : 0;
	GLfloat yoffset = dPriv ? (GLfloat) dPriv->y + dPriv->h : 0;
	const GLfloat *v = ctx->Viewport._WindowMap.m;

	GLfloat sx = v[MAT_SX];
	GLfloat tx = v[MAT_TX] + xoffset + SUBPIXEL_X;
	GLfloat sy = -v[MAT_SY];
	GLfloat ty = (-v[MAT_TY]) + yoffset + SUBPIXEL_Y;
	GLfloat sz = v[MAT_SZ] * rmesa->state.depth.scale;
	GLfloat tz = v[MAT_TZ] * rmesa->state.depth.scale;

	R300_FIREVERTICES(rmesa);
	R300_STATECHANGE(rmesa, vpt);

	rmesa->hw.vpt.cmd[R300_VPT_XSCALE] = r300PackFloat32(sx);
	rmesa->hw.vpt.cmd[R300_VPT_XOFFSET] = r300PackFloat32(tx);
	rmesa->hw.vpt.cmd[R300_VPT_YSCALE] = r300PackFloat32(sy);
	rmesa->hw.vpt.cmd[R300_VPT_YOFFSET] = r300PackFloat32(ty);
	rmesa->hw.vpt.cmd[R300_VPT_ZSCALE] = r300PackFloat32(sz);
	rmesa->hw.vpt.cmd[R300_VPT_ZOFFSET] = r300PackFloat32(tz);
}

static void r300Viewport(GLcontext * ctx, GLint x, GLint y,
			 GLsizei width, GLsizei height)
{
	/* Don't pipeline viewport changes, conflict with window offset
	 * setting below.  Could apply deltas to rescue pipelined viewport
	 * values, or keep the originals hanging around.
	 */
	r300UpdateWindow(ctx);
}

static void r300DepthRange(GLcontext * ctx, GLclampd nearval, GLclampd farval)
{
	r300UpdateWindow(ctx);
}

void r300UpdateViewportOffset(GLcontext * ctx)
{
	r300ContextPtr rmesa = R300_CONTEXT(ctx);
	__DRIdrawablePrivate *dPriv = ((radeonContextPtr) rmesa)->dri.drawable;
	GLfloat xoffset = (GLfloat) dPriv->x;
	GLfloat yoffset = (GLfloat) dPriv->y + dPriv->h;
	const GLfloat *v = ctx->Viewport._WindowMap.m;

	GLfloat tx = v[MAT_TX] + xoffset + SUBPIXEL_X;
	GLfloat ty = (-v[MAT_TY]) + yoffset + SUBPIXEL_Y;

	if (rmesa->hw.vpt.cmd[R300_VPT_XOFFSET] != r300PackFloat32(tx) ||
	    rmesa->hw.vpt.cmd[R300_VPT_YOFFSET] != r300PackFloat32(ty)) {
		/* Note: this should also modify whatever data the context reset
		 * code uses...
		 */
		R300_STATECHANGE(rmesa, vpt);
		rmesa->hw.vpt.cmd[R300_VPT_XOFFSET] = r300PackFloat32(tx);
		rmesa->hw.vpt.cmd[R300_VPT_YOFFSET] = r300PackFloat32(ty);

	}

	radeonUpdateScissor(ctx);
}

/**
 * Tell the card where to render (offset, pitch).
 * Effected by glDrawBuffer, etc
 */
void r300UpdateDrawBuffer(GLcontext * ctx)
{
	r300ContextPtr rmesa = R300_CONTEXT(ctx);
	r300ContextPtr r300 = rmesa;
	struct gl_framebuffer *fb = ctx->DrawBuffer;
	driRenderbuffer *drb;

	if (fb->_ColorDrawBufferIndexes[0] == BUFFER_FRONT_LEFT) {
		/* draw to front */
		drb =
		    (driRenderbuffer *) fb->Attachment[BUFFER_FRONT_LEFT].
		    Renderbuffer;
	} else if (fb->_ColorDrawBufferIndexes[0] == BUFFER_BACK_LEFT) {
		/* draw to back */
		drb =
		    (driRenderbuffer *) fb->Attachment[BUFFER_BACK_LEFT].
		    Renderbuffer;
	} else {
		/* drawing to multiple buffers, or none */
		return;
	}

	assert(drb);
	assert(drb->flippedPitch);

	R300_STATECHANGE(rmesa, cb);

	r300->hw.cb.cmd[R300_CB_OFFSET] = drb->flippedOffset +	//r300->radeon.state.color.drawOffset +
	    r300->radeon.radeonScreen->fbLocation;
	r300->hw.cb.cmd[R300_CB_PITCH] = drb->flippedPitch;	//r300->radeon.state.color.drawPitch;

	if (r300->radeon.radeonScreen->cpp == 4)
		r300->hw.cb.cmd[R300_CB_PITCH] |= R300_COLOR_FORMAT_ARGB8888;
	else
		r300->hw.cb.cmd[R300_CB_PITCH] |= R300_COLOR_FORMAT_RGB565;

	if (r300->radeon.sarea->tiling_enabled)
		r300->hw.cb.cmd[R300_CB_PITCH] |= R300_COLOR_TILE_ENABLE;
#if 0
	R200_STATECHANGE(rmesa, ctx);

	/* Note: we used the (possibly) page-flipped values */
	rmesa->hw.ctx.cmd[CTX_RB3D_COLOROFFSET]
	    = ((drb->flippedOffset + rmesa->r200Screen->fbLocation)
	       & R200_COLOROFFSET_MASK);
	rmesa->hw.ctx.cmd[CTX_RB3D_COLORPITCH] = drb->flippedPitch;

	if (rmesa->sarea->tiling_enabled) {
		rmesa->hw.ctx.cmd[CTX_RB3D_COLORPITCH] |=
		    R200_COLOR_TILE_ENABLE;
	}
#endif
}

static void
r300FetchStateParameter(GLcontext * ctx,
			const gl_state_index state[STATE_LENGTH],
			GLfloat * value)
{
	r300ContextPtr r300 = R300_CONTEXT(ctx);

	switch (state[0]) {
	case STATE_INTERNAL:
		switch (state[1]) {
		case STATE_R300_WINDOW_DIMENSION:
			value[0] = r300->radeon.dri.drawable->w * 0.5f;	/* width*0.5 */
			value[1] = r300->radeon.dri.drawable->h * 0.5f;	/* height*0.5 */
			value[2] = 0.5F;	/* for moving range [-1 1] -> [0 1] */
			value[3] = 1.0F;	/* not used */
			break;

		case STATE_R300_TEXRECT_FACTOR:{
				struct gl_texture_object *t =
				    ctx->Texture.Unit[state[2]].CurrentRect;

				if (t && t->Image[0][t->BaseLevel]) {
					struct gl_texture_image *image =
					    t->Image[0][t->BaseLevel];
					value[0] = 1.0 / image->Width2;
					value[1] = 1.0 / image->Height2;
				} else {
					value[0] = 1.0;
					value[1] = 1.0;
				}
				value[2] = 1.0;
				value[3] = 1.0;
				break;
			}

		default:
			break;
		}
		break;

	default:
		break;
	}
}

/**
 * Update R300's own internal state parameters.
 * For now just STATE_R300_WINDOW_DIMENSION
 */
void r300UpdateStateParameters(GLcontext * ctx, GLuint new_state)
{
	struct r300_fragment_program *fp;
	struct gl_program_parameter_list *paramList;
	GLuint i;

	if (!(new_state & (_NEW_BUFFERS | _NEW_PROGRAM)))
		return;

	fp = (struct r300_fragment_program *)ctx->FragmentProgram._Current;
	if (!fp)
		return;

	paramList = fp->mesa_program.Base.Parameters;

	if (!paramList)
		return;

	for (i = 0; i < paramList->NumParameters; i++) {
		if (paramList->Parameters[i].Type == PROGRAM_STATE_VAR) {
			r300FetchStateParameter(ctx,
						paramList->Parameters[i].
						StateIndexes,
						paramList->ParameterValues[i]);
		}
	}
}

/* =============================================================
 * Polygon state
 */
static void r300PolygonOffset(GLcontext * ctx, GLfloat factor, GLfloat units)
{
	r300ContextPtr rmesa = R300_CONTEXT(ctx);
	GLfloat constant = units;

	switch (ctx->Visual.depthBits) {
	case 16:
		constant *= 4.0;
		break;
	case 24:
		constant *= 2.0;
		break;
	}

	factor *= 12.0;

/*    fprintf(stderr, "%s f:%f u:%f\n", __FUNCTION__, factor, constant); */

	R300_STATECHANGE(rmesa, zbs);
	rmesa->hw.zbs.cmd[R300_ZBS_T_FACTOR] = r300PackFloat32(factor);
	rmesa->hw.zbs.cmd[R300_ZBS_T_CONSTANT] = r300PackFloat32(constant);
	rmesa->hw.zbs.cmd[R300_ZBS_W_FACTOR] = r300PackFloat32(factor);
	rmesa->hw.zbs.cmd[R300_ZBS_W_CONSTANT] = r300PackFloat32(constant);
}

/* Routing and texture-related */

/* r300 doesnt handle GL_CLAMP and GL_MIRROR_CLAMP_EXT correctly when filter is NEAREST.
 * Since texwrap produces same results for GL_CLAMP and GL_CLAMP_TO_EDGE we use them instead.
 * We need to recalculate wrap modes whenever filter mode is changed because someone might do:
 * glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
 * glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
 * glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
 * Since r300 completely ignores R300_TX_CLAMP when either min or mag is nearest it cant handle
 * combinations where only one of them is nearest.
 */
static unsigned long gen_fixed_filter(unsigned long f)
{
	unsigned long mag, min, needs_fixing = 0;
	//return f;

	/* We ignore MIRROR bit so we dont have to do everything twice */
	if ((f & ((7 - 1) << R300_TX_WRAP_S_SHIFT)) ==
	    (R300_TX_CLAMP << R300_TX_WRAP_S_SHIFT)) {
		needs_fixing |= 1;
	}
	if ((f & ((7 - 1) << R300_TX_WRAP_T_SHIFT)) ==
	    (R300_TX_CLAMP << R300_TX_WRAP_T_SHIFT)) {
		needs_fixing |= 2;
	}
	if ((f & ((7 - 1) << R300_TX_WRAP_R_SHIFT)) ==
	    (R300_TX_CLAMP << R300_TX_WRAP_R_SHIFT)) {
		needs_fixing |= 4;
	}

	if (!needs_fixing)
		return f;

	mag = f & R300_TX_MAG_FILTER_MASK;
	min = f & (R300_TX_MIN_FILTER_MASK|R300_TX_MIN_FILTER_MIP_MASK);

	/* TODO: Check for anisto filters too */
	if ((mag != R300_TX_MAG_FILTER_NEAREST)
	    && (min != R300_TX_MIN_FILTER_NEAREST))
		return f;

	/* r300 cant handle these modes hence we force nearest to linear */
	if ((mag == R300_TX_MAG_FILTER_NEAREST)
	    && (min != R300_TX_MIN_FILTER_NEAREST)) {
		f &= ~R300_TX_MAG_FILTER_NEAREST;
		f |= R300_TX_MAG_FILTER_LINEAR;
		return f;
	}

	if ((min == R300_TX_MIN_FILTER_NEAREST)
	    && (mag != R300_TX_MAG_FILTER_NEAREST)) {
		f &= ~R300_TX_MIN_FILTER_NEAREST;
		f |= R300_TX_MIN_FILTER_LINEAR;
		return f;
	}

	/* Both are nearest */
	if (needs_fixing & 1) {
		f &= ~((7 - 1) << R300_TX_WRAP_S_SHIFT);
		f |= R300_TX_CLAMP_TO_EDGE << R300_TX_WRAP_S_SHIFT;
	}
	if (needs_fixing & 2) {
		f &= ~((7 - 1) << R300_TX_WRAP_T_SHIFT);
		f |= R300_TX_CLAMP_TO_EDGE << R300_TX_WRAP_T_SHIFT;
	}
	if (needs_fixing & 4) {
		f &= ~((7 - 1) << R300_TX_WRAP_R_SHIFT);
		f |= R300_TX_CLAMP_TO_EDGE << R300_TX_WRAP_R_SHIFT;
	}
	return f;
}

static void r300SetupFragmentShaderTextures(GLcontext *ctx, int *tmu_mappings)
{
	r300ContextPtr r300 = R300_CONTEXT(ctx);
	int i;
	struct r300_fragment_program *fp = (struct r300_fragment_program *)
	    (char *)ctx->FragmentProgram._Current;
	struct r300_fragment_program_code *code = &fp->code;

	R300_STATECHANGE(r300, fpt);

	for (i = 0; i < code->tex.length; i++) {
		int unit;
		int opcode;
		unsigned long val;

		unit = code->tex.inst[i] >> R300_TEX_ID_SHIFT;
		unit &= 15;

		val = code->tex.inst[i];
		val &= ~R300_TEX_ID_MASK;

		opcode =
			(val & R300_TEX_INST_MASK) >> R300_TEX_INST_SHIFT;
		if (opcode == R300_TEX_OP_KIL) {
			r300->hw.fpt.cmd[R300_FPT_INSTR_0 + i] = val;
		} else {
			if (tmu_mappings[unit] >= 0) {
				val |=
					tmu_mappings[unit] <<
					R300_TEX_ID_SHIFT;
				r300->hw.fpt.cmd[R300_FPT_INSTR_0 + i] = val;
			} else {
				// We get here when the corresponding texture image is incomplete
				// (e.g. incomplete mipmaps etc.)
				r300->hw.fpt.cmd[R300_FPT_INSTR_0 + i] = val;
			}
		}
	}

	r300->hw.fpt.cmd[R300_FPT_CMD_0] =
		cmdpacket0(R300_US_TEX_INST_0, code->tex.length);
}

static void r500SetupFragmentShaderTextures(GLcontext *ctx, int *tmu_mappings)
{
	int i;
	struct r500_fragment_program *fp = (struct r500_fragment_program *)
	    (char *)ctx->FragmentProgram._Current;
	struct r500_fragment_program_code *code = &fp->code;

	/* find all the texture instructions and relocate the texture units */
	for (i = 0; i < code->inst_end + 1; i++) {
		if ((code->inst[i].inst0 & 0x3) == R500_INST_TYPE_TEX) {
			uint32_t val;
			int unit, opcode, new_unit;

			val = code->inst[i].inst1;

			unit = (val >> 16) & 0xf;

			val &= ~(0xf << 16);

			opcode = val & (0x7 << 22);
			if (opcode == R500_TEX_INST_TEXKILL) {
				new_unit = 0;
			} else {
				if (tmu_mappings[unit] >= 0) {
					new_unit = tmu_mappings[unit];
				} else {
					new_unit = 0;
				}
			}
			val |= R500_TEX_ID(new_unit);
			code->inst[i].inst1 = val;
		}
	}
}

static GLuint translate_lod_bias(GLfloat bias)
{
	GLint b = (int)(bias*32);
	if (b >= (1 << 9))
		b = (1 << 9)-1;
	else if (b < -(1 << 9))
		b = -(1 << 9);
	return (((GLuint)b) << R300_LOD_BIAS_SHIFT) & R300_LOD_BIAS_MASK;
}

static void r300SetupTextures(GLcontext * ctx)
{
	int i, mtu;
	struct r300_tex_obj *t;
	r300ContextPtr r300 = R300_CONTEXT(ctx);
	int hw_tmu = 0;
	int last_hw_tmu = -1;	/* -1 translates into no setup costs for fields */
	int tmu_mappings[R300_MAX_TEXTURE_UNITS] = { -1, };
	struct r300_fragment_program *fp = (struct r300_fragment_program *)
	    (char *)ctx->FragmentProgram._Current;

	R300_STATECHANGE(r300, txe);
	R300_STATECHANGE(r300, tex.filter);
	R300_STATECHANGE(r300, tex.filter_1);
	R300_STATECHANGE(r300, tex.size);
	R300_STATECHANGE(r300, tex.format);
	R300_STATECHANGE(r300, tex.pitch);
	R300_STATECHANGE(r300, tex.offset);
	R300_STATECHANGE(r300, tex.chroma_key);
	R300_STATECHANGE(r300, tex.border_color);

	r300->hw.txe.cmd[R300_TXE_ENABLE] = 0x0;

	mtu = r300->radeon.glCtx->Const.MaxTextureUnits;
	if (RADEON_DEBUG & DEBUG_STATE)
		fprintf(stderr, "mtu=%d\n", mtu);

	if (mtu > R300_MAX_TEXTURE_UNITS) {
		fprintf(stderr,
			"Aiiee ! mtu=%d is greater than R300_MAX_TEXTURE_UNITS=%d\n",
			mtu, R300_MAX_TEXTURE_UNITS);
		_mesa_exit(-1);
	}

	/* We cannot let disabled tmu offsets pass DRM */
	for (i = 0; i < mtu; i++) {
		if (ctx->Texture.Unit[i]._ReallyEnabled) {

#if 0				/* Enables old behaviour */
			hw_tmu = i;
#endif
			tmu_mappings[i] = hw_tmu;

			t = r300->state.texture.unit[i].texobj;
			/* XXX questionable fix for bug 9170: */
			if (!t)
				continue;

			if ((t->format & 0xffffff00) == 0xffffff00) {
				WARN_ONCE
				    ("unknown texture format (entry %x) encountered. Help me !\n",
				     t->format & 0xff);
			}

			if (RADEON_DEBUG & DEBUG_STATE)
				fprintf(stderr,
					"Activating texture unit %d\n", i);

			r300->hw.txe.cmd[R300_TXE_ENABLE] |= (1 << hw_tmu);

			r300->hw.tex.filter.cmd[R300_TEX_VALUE_0 +
						hw_tmu] =
			    gen_fixed_filter(t->filter) | (hw_tmu << 28);
			/* Note: There is a LOD bias per texture unit and a LOD bias
			 * per texture object. We add them here to get the correct behaviour.
			 * (The per-texture object LOD bias was introduced in OpenGL 1.4
			 * and is not present in the EXT_texture_object extension).
			 */
			r300->hw.tex.filter_1.cmd[R300_TEX_VALUE_0 + hw_tmu] =
				t->filter_1 |
				translate_lod_bias(ctx->Texture.Unit[i].LodBias + t->base.tObj->LodBias);
			r300->hw.tex.size.cmd[R300_TEX_VALUE_0 + hw_tmu] =
			    t->size;
			r300->hw.tex.format.cmd[R300_TEX_VALUE_0 +
						hw_tmu] = t->format;
			r300->hw.tex.pitch.cmd[R300_TEX_VALUE_0 + hw_tmu] =
			    t->pitch_reg;
			r300->hw.tex.offset.cmd[R300_TEX_VALUE_0 +
						hw_tmu] = t->offset;

			if (t->offset & R300_TXO_MACRO_TILE) {
				WARN_ONCE("macro tiling enabled!\n");
			}

			if (t->offset & R300_TXO_MICRO_TILE) {
				WARN_ONCE("micro tiling enabled!\n");
			}

			r300->hw.tex.chroma_key.cmd[R300_TEX_VALUE_0 +
						    hw_tmu] = 0x0;
			r300->hw.tex.border_color.cmd[R300_TEX_VALUE_0 +
						      hw_tmu] =
			    t->pp_border_color;

			last_hw_tmu = hw_tmu;

			hw_tmu++;
		}
	}

	r300->hw.tex.filter.cmd[R300_TEX_CMD_0] =
	    cmdpacket0(R300_TX_FILTER0_0, last_hw_tmu + 1);
	r300->hw.tex.filter_1.cmd[R300_TEX_CMD_0] =
	    cmdpacket0(R300_TX_FILTER1_0, last_hw_tmu + 1);
	r300->hw.tex.size.cmd[R300_TEX_CMD_0] =
	    cmdpacket0(R300_TX_SIZE_0, last_hw_tmu + 1);
	r300->hw.tex.format.cmd[R300_TEX_CMD_0] =
	    cmdpacket0(R300_TX_FORMAT_0, last_hw_tmu + 1);
	r300->hw.tex.pitch.cmd[R300_TEX_CMD_0] =
	    cmdpacket0(R300_TX_FORMAT2_0, last_hw_tmu + 1);
	r300->hw.tex.offset.cmd[R300_TEX_CMD_0] =
	    cmdpacket0(R300_TX_OFFSET_0, last_hw_tmu + 1);
	r300->hw.tex.chroma_key.cmd[R300_TEX_CMD_0] =
	    cmdpacket0(R300_TX_CHROMA_KEY_0, last_hw_tmu + 1);
	r300->hw.tex.border_color.cmd[R300_TEX_CMD_0] =
	    cmdpacket0(R300_TX_BORDER_COLOR_0, last_hw_tmu + 1);

	if (!fp)		/* should only happenen once, just after context is created */
		return;

	if (r300->radeon.radeonScreen->chip_family < CHIP_FAMILY_RV515) {
		if (fp->mesa_program.UsesKill && last_hw_tmu < 0) {
			// The KILL operation requires the first texture unit
			// to be enabled.
			r300->hw.txe.cmd[R300_TXE_ENABLE] |= 1;
			r300->hw.tex.filter.cmd[R300_TEX_VALUE_0] = 0;
			r300->hw.tex.filter.cmd[R300_TEX_CMD_0] =
				cmdpacket0(R300_TX_FILTER0_0, 1);
		}
		r300SetupFragmentShaderTextures(ctx, tmu_mappings);
	} else
		r500SetupFragmentShaderTextures(ctx, tmu_mappings);

	if (RADEON_DEBUG & DEBUG_STATE)
		fprintf(stderr, "TX_ENABLE: %08x  last_hw_tmu=%d\n",
			r300->hw.txe.cmd[R300_TXE_ENABLE], last_hw_tmu);
}

union r300_outputs_written {
	GLuint vp_outputs;	/* hw_tcl_on */
	 DECLARE_RENDERINPUTS(index_bitset);	/* !hw_tcl_on */
};

#define R300_OUTPUTS_WRITTEN_TEST(ow, vp_result, tnl_attrib) \
	((hw_tcl_on) ? (ow).vp_outputs & (1 << (vp_result)) : \
	RENDERINPUTS_TEST( (ow.index_bitset), (tnl_attrib) ))

static void r300SetupRSUnit(GLcontext * ctx)
{
	r300ContextPtr r300 = R300_CONTEXT(ctx);
	/* I'm still unsure if these are needed */
	GLuint interp_col[8];
        TNLcontext *tnl = TNL_CONTEXT(ctx);
	struct vertex_buffer *VB = &tnl->vb;
	union r300_outputs_written OutputsWritten;
	GLuint InputsRead;
	int fp_reg, high_rr;
	int col_interp_nr;
	int rs_tex_count = 0, rs_col_count = 0;
	int i, count;

	memset(interp_col, 0, sizeof(interp_col));

	if (hw_tcl_on)
		OutputsWritten.vp_outputs = CURRENT_VERTEX_SHADER(ctx)->key.OutputsWritten;
	else
		RENDERINPUTS_COPY(OutputsWritten.index_bitset, r300->state.render_inputs_bitset);

	if (ctx->FragmentProgram._Current)
		InputsRead = ctx->FragmentProgram._Current->Base.InputsRead;
	else {
		fprintf(stderr, "No ctx->FragmentProgram._Current!!\n");
		return;		/* This should only ever happen once.. */
	}

	R300_STATECHANGE(r300, ri);
	R300_STATECHANGE(r300, rc);
	R300_STATECHANGE(r300, rr);

	fp_reg = col_interp_nr = high_rr = 0;

	r300->hw.rr.cmd[R300_RR_INST_1] = 0;

	if (InputsRead & FRAG_BIT_WPOS) {
		for (i = 0; i < ctx->Const.MaxTextureUnits; i++)
			if (!(InputsRead & (FRAG_BIT_TEX0 << i)))
				break;

		if (i == ctx->Const.MaxTextureUnits) {
			fprintf(stderr, "\tno free texcoord found...\n");
			_mesa_exit(-1);
		}

		InputsRead |= (FRAG_BIT_TEX0 << i);
		InputsRead &= ~FRAG_BIT_WPOS;
	}

	if (InputsRead & FRAG_BIT_COL0) {
		count = VB->AttribPtr[_TNL_ATTRIB_COLOR0]->size;
		interp_col[0] |= R300_RS_COL_PTR(rs_col_count);
		if (count == 3)
			interp_col[0] |= R300_RS_COL_FMT(R300_RS_COL_FMT_RGB1);
		rs_col_count += count;
	}
	else
		interp_col[0] = R300_RS_COL_FMT(R300_RS_COL_FMT_0001);

	if (InputsRead & FRAG_BIT_COL1) {
		count = VB->AttribPtr[_TNL_ATTRIB_COLOR1]->size;
		if (count == 3)
			interp_col[1] |= R300_RS_COL_FMT(R300_RS_COL_FMT_RGB0);
		interp_col[1] |= R300_RS_COL_PTR(1);
		rs_col_count += count;
	}

	if (InputsRead & FRAG_BIT_FOGC) {
		/* XXX FIX THIS
		 * Just turn off the bit for now.
		 * Need to do something similar to the color/texcoord inputs.
		 */
		InputsRead &= ~FRAG_BIT_FOGC;
	}

	for (i = 0; i < ctx->Const.MaxTextureUnits; i++) {
		int swiz;

		/* with TCL we always seem to route 4 components */
		if (hw_tcl_on)
		  count = 4;
		else
		  count = VB->AttribPtr[_TNL_ATTRIB_TEX(i)]->size;

		r300->hw.ri.cmd[R300_RI_INTERP_0 + i] = interp_col[i] | rs_tex_count;
		switch(count) {
		case 4: swiz = R300_RS_SEL_S(0) | R300_RS_SEL_T(1) | R300_RS_SEL_R(2) | R300_RS_SEL_Q(3); break;
		case 3: swiz = R300_RS_SEL_S(0) | R300_RS_SEL_T(1) | R300_RS_SEL_R(2) | R300_RS_SEL_Q(R300_RS_SEL_K1); break;
		default:
		case 1:
		case 2: swiz = R300_RS_SEL_S(0) | R300_RS_SEL_T(1) | R300_RS_SEL_R(R300_RS_SEL_K0) | R300_RS_SEL_Q(R300_RS_SEL_K1); break;
		};

		r300->hw.ri.cmd[R300_RI_INTERP_0 + i] |= swiz;

		r300->hw.rr.cmd[R300_RR_INST_0 + fp_reg] = 0;
		if (InputsRead & (FRAG_BIT_TEX0 << i)) {

			rs_tex_count += count;

			//assert(r300->state.texture.tc_count != 0);
			r300->hw.rr.cmd[R300_RR_INST_0 + fp_reg] |= R300_RS_INST_TEX_CN_WRITE | i	/* source INTERP */
			    | (fp_reg << R300_RS_INST_TEX_ADDR_SHIFT);
			high_rr = fp_reg;

			/* Passing invalid data here can lock the GPU. */
			if (R300_OUTPUTS_WRITTEN_TEST(OutputsWritten, VERT_RESULT_TEX0 + i, _TNL_ATTRIB_TEX(i))) {
				InputsRead &= ~(FRAG_BIT_TEX0 << i);
				fp_reg++;
			} else {
				WARN_ONCE("fragprog wants coords for tex%d, vp doesn't provide them!\n", i);
			}
		}
	}

	if (InputsRead & FRAG_BIT_COL0) {
		if (R300_OUTPUTS_WRITTEN_TEST(OutputsWritten, VERT_RESULT_COL0, _TNL_ATTRIB_COLOR0)) {
			r300->hw.rr.cmd[R300_RR_INST_0] |= R300_RS_INST_COL_ID(0) | R300_RS_INST_COL_CN_WRITE | (fp_reg++ << R300_RS_INST_COL_ADDR_SHIFT);
			InputsRead &= ~FRAG_BIT_COL0;
			col_interp_nr++;
		} else {
			WARN_ONCE("fragprog wants col0, vp doesn't provide it\n");
		}
	}

	if (InputsRead & FRAG_BIT_COL1) {
		if (R300_OUTPUTS_WRITTEN_TEST(OutputsWritten, VERT_RESULT_COL1, _TNL_ATTRIB_COLOR1)) {
			r300->hw.rr.cmd[R300_RR_INST_1] |= R300_RS_INST_COL_ID(1) | R300_RS_INST_COL_CN_WRITE | (fp_reg++ << R300_RS_INST_COL_ADDR_SHIFT);
			InputsRead &= ~FRAG_BIT_COL1;
			if (high_rr < 1)
				high_rr = 1;
			col_interp_nr++;
		} else {
			WARN_ONCE("fragprog wants col1, vp doesn't provide it\n");
		}
	}

	/* Need at least one. This might still lock as the values are undefined... */
	if (rs_tex_count == 0 && col_interp_nr == 0) {
		r300->hw.rr.cmd[R300_RR_INST_0] |= R300_RS_INST_COL_ID(0) | R300_RS_INST_COL_CN_WRITE | (fp_reg++ << R300_RS_INST_COL_ADDR_SHIFT);
		col_interp_nr++;
	}

	r300->hw.rc.cmd[1] = 0 | (rs_tex_count << R300_IT_COUNT_SHIFT)
	  | (col_interp_nr << R300_IC_COUNT_SHIFT)
	  | R300_HIRES_EN;

	assert(high_rr >= 0);
	r300->hw.rr.cmd[R300_RR_CMD_0] = cmdpacket0(R300_RS_INST_0, high_rr + 1);
	r300->hw.rc.cmd[2] = high_rr;

	if (InputsRead)
		WARN_ONCE("Don't know how to satisfy InputsRead=0x%08x\n", InputsRead);
}

static void r500SetupRSUnit(GLcontext * ctx)
{
	r300ContextPtr r300 = R300_CONTEXT(ctx);
	/* I'm still unsure if these are needed */
	GLuint interp_col[8];
	union r300_outputs_written OutputsWritten;
        TNLcontext *tnl = TNL_CONTEXT(ctx);
	struct vertex_buffer *VB = &tnl->vb;
	GLuint InputsRead;
	int fp_reg, high_rr;
	int rs_col_count = 0;
	int in_texcoords, col_interp_nr;
	int i, count;

	memset(interp_col, 0, sizeof(interp_col));
	if (hw_tcl_on)
		OutputsWritten.vp_outputs = CURRENT_VERTEX_SHADER(ctx)->key.OutputsWritten;
	else
		RENDERINPUTS_COPY(OutputsWritten.index_bitset, r300->state.render_inputs_bitset);

	if (ctx->FragmentProgram._Current)
		InputsRead = ctx->FragmentProgram._Current->Base.InputsRead;
	else {
		fprintf(stderr, "No ctx->FragmentProgram._Current!!\n");
		return;		/* This should only ever happen once.. */
	}

	R300_STATECHANGE(r300, ri);
	R300_STATECHANGE(r300, rc);
	R300_STATECHANGE(r300, rr);

	fp_reg = col_interp_nr = high_rr = in_texcoords = 0;

	r300->hw.rr.cmd[R300_RR_INST_1] = 0;

	if (InputsRead & FRAG_BIT_WPOS) {
		for (i = 0; i < ctx->Const.MaxTextureUnits; i++)
			if (!(InputsRead & (FRAG_BIT_TEX0 << i)))
				break;

		if (i == ctx->Const.MaxTextureUnits) {
			fprintf(stderr, "\tno free texcoord found...\n");
			_mesa_exit(-1);
		}

		InputsRead |= (FRAG_BIT_TEX0 << i);
		InputsRead &= ~FRAG_BIT_WPOS;
	}

	if (InputsRead & FRAG_BIT_COL0) {
		count = VB->AttribPtr[_TNL_ATTRIB_COLOR0]->size;
		interp_col[0] |= R500_RS_COL_PTR(rs_col_count);
		if (count == 3)
			interp_col[0] |= R500_RS_COL_FMT(R300_RS_COL_FMT_RGB1);
		rs_col_count += count;
	}
	else
		interp_col[0] = R500_RS_COL_FMT(R300_RS_COL_FMT_0001);

	if (InputsRead & FRAG_BIT_COL1) {
		count = VB->AttribPtr[_TNL_ATTRIB_COLOR1]->size;
		interp_col[1] |= R500_RS_COL_PTR(1);
		if (count == 3)
			interp_col[1] |= R500_RS_COL_FMT(R300_RS_COL_FMT_RGB0);
		rs_col_count += count;
	}

	for (i = 0; i < ctx->Const.MaxTextureUnits; i++) {
		GLuint swiz = 0;

		/* with TCL we always seem to route 4 components */
		if (InputsRead & (FRAG_BIT_TEX0 << i)) {

		  if (hw_tcl_on)
		    count = 4;
		  else
		    count = VB->AttribPtr[_TNL_ATTRIB_TEX(i)]->size;

		  /* always have on texcoord */
		  swiz |= in_texcoords++ << R500_RS_IP_TEX_PTR_S_SHIFT;
		  if (count >= 2)
		    swiz |= in_texcoords++ << R500_RS_IP_TEX_PTR_T_SHIFT;
		  else
		    swiz |= R500_RS_IP_PTR_K0 << R500_RS_IP_TEX_PTR_T_SHIFT;

		  if (count >= 3)
		    swiz |= in_texcoords++ << R500_RS_IP_TEX_PTR_R_SHIFT;
		  else
		    swiz |= R500_RS_IP_PTR_K0 << R500_RS_IP_TEX_PTR_R_SHIFT;

		  if (count == 4)
		    swiz |= in_texcoords++ << R500_RS_IP_TEX_PTR_Q_SHIFT;
		  else
		    swiz |= R500_RS_IP_PTR_K1 << R500_RS_IP_TEX_PTR_Q_SHIFT;

		} else
		   swiz = (R500_RS_IP_PTR_K0 << R500_RS_IP_TEX_PTR_S_SHIFT) |
		          (R500_RS_IP_PTR_K0 << R500_RS_IP_TEX_PTR_T_SHIFT) |
		          (R500_RS_IP_PTR_K0 << R500_RS_IP_TEX_PTR_R_SHIFT) |
		          (R500_RS_IP_PTR_K1 << R500_RS_IP_TEX_PTR_Q_SHIFT);

		r300->hw.ri.cmd[R300_RI_INTERP_0 + i] = interp_col[i] | swiz;

		r300->hw.rr.cmd[R300_RR_INST_0 + fp_reg] = 0;
		if (InputsRead & (FRAG_BIT_TEX0 << i)) {
			//assert(r300->state.texture.tc_count != 0);
			r300->hw.rr.cmd[R300_RR_INST_0 + fp_reg] |= R500_RS_INST_TEX_CN_WRITE | i	/* source INTERP */
			    | (fp_reg << R500_RS_INST_TEX_ADDR_SHIFT);
			high_rr = fp_reg;

			/* Passing invalid data here can lock the GPU. */
			if (R300_OUTPUTS_WRITTEN_TEST(OutputsWritten, VERT_RESULT_TEX0 + i, _TNL_ATTRIB_TEX(i))) {
				InputsRead &= ~(FRAG_BIT_TEX0 << i);
				fp_reg++;
			} else {
				WARN_ONCE("fragprog wants coords for tex%d, vp doesn't provide them!\n", i);
			}
		}
	}

	if (InputsRead & FRAG_BIT_COL0) {
		if (R300_OUTPUTS_WRITTEN_TEST(OutputsWritten, VERT_RESULT_COL0, _TNL_ATTRIB_COLOR0)) {
			r300->hw.rr.cmd[R300_RR_INST_0] |= R500_RS_INST_COL_CN_WRITE | (fp_reg++ << R500_RS_INST_COL_ADDR_SHIFT);
			InputsRead &= ~FRAG_BIT_COL0;
			col_interp_nr++;
		} else {
			WARN_ONCE("fragprog wants col0, vp doesn't provide it\n");
		}
	}

	if (InputsRead & FRAG_BIT_COL1) {
		if (R300_OUTPUTS_WRITTEN_TEST(OutputsWritten, VERT_RESULT_COL1, _TNL_ATTRIB_COLOR1)) {
			r300->hw.rr.cmd[R300_RR_INST_1] |= (1 << 12) | R500_RS_INST_COL_CN_WRITE |  (fp_reg++ << R500_RS_INST_COL_ADDR_SHIFT);
			InputsRead &= ~FRAG_BIT_COL1;
			if (high_rr < 1)
				high_rr = 1;
			col_interp_nr++;
		} else {
			WARN_ONCE("fragprog wants col1, vp doesn't provide it\n");
		}
	}

	/* Need at least one. This might still lock as the values are undefined... */
	if (in_texcoords == 0 && col_interp_nr == 0) {
		r300->hw.rr.cmd[R300_RR_INST_0] |= 0 | R500_RS_INST_COL_CN_WRITE | (fp_reg++ << R500_RS_INST_COL_ADDR_SHIFT);
		col_interp_nr++;
	}

	r300->hw.rc.cmd[1] = 0 | (in_texcoords << R300_IT_COUNT_SHIFT)
	  | (col_interp_nr << R300_IC_COUNT_SHIFT)
	  | R300_HIRES_EN;

	assert(high_rr >= 0);
	r300->hw.rr.cmd[R300_RR_CMD_0] = cmdpacket0(R500_RS_INST_0, high_rr + 1);
	r300->hw.rc.cmd[2] = 0xC0 | high_rr;

	if (InputsRead)
		WARN_ONCE("Don't know how to satisfy InputsRead=0x%08x\n", InputsRead);
}




#define bump_vpu_count(ptr, new_count)   do{\
	drm_r300_cmd_header_t* _p=((drm_r300_cmd_header_t*)(ptr));\
	int _nc=(new_count)/4; \
	assert(_nc < 256); \
	if(_nc>_p->vpu.count)_p->vpu.count=_nc;\
	}while(0)

static INLINE void r300SetupVertexProgramFragment(r300ContextPtr r300, int dest, struct r300_vertex_shader_fragment *vsf)
{
	int i;

	if (vsf->length == 0)
		return;

	if (vsf->length & 0x3) {
		fprintf(stderr, "VERTEX_SHADER_FRAGMENT must have length divisible by 4\n");
		_mesa_exit(-1);
	}

	switch ((dest >> 8) & 0xf) {
	case 0:
		R300_STATECHANGE(r300, vpi);
		for (i = 0; i < vsf->length; i++)
			r300->hw.vpi.cmd[R300_VPI_INSTR_0 + i + 4 * (dest & 0xff)] = (vsf->body.d[i]);
		bump_vpu_count(r300->hw.vpi.cmd, vsf->length + 4 * (dest & 0xff));
		break;

	case 2:
		R300_STATECHANGE(r300, vpp);
		for (i = 0; i < vsf->length; i++)
			r300->hw.vpp.cmd[R300_VPP_PARAM_0 + i + 4 * (dest & 0xff)] = (vsf->body.d[i]);
		bump_vpu_count(r300->hw.vpp.cmd, vsf->length + 4 * (dest & 0xff));
		break;
	case 4:
		R300_STATECHANGE(r300, vps);
		for (i = 0; i < vsf->length; i++)
			r300->hw.vps.cmd[1 + i + 4 * (dest & 0xff)] = (vsf->body.d[i]);
		bump_vpu_count(r300->hw.vps.cmd, vsf->length + 4 * (dest & 0xff));
		break;
	default:
		fprintf(stderr, "%s:%s don't know how to handle dest %04x\n", __FILE__, __FUNCTION__, dest);
		_mesa_exit(-1);
	}
}

#define MIN3(a, b, c)	((a) < (b) ? MIN2(a, c) : MIN2(b, c))


static void r300VapCntl(r300ContextPtr rmesa, GLuint input_count,
			GLuint output_count, GLuint temp_count)
{
    int vtx_mem_size;
    int pvs_num_slots;
    int pvs_num_cntrls;

    /* Flush PVS engine before changing PVS_NUM_SLOTS, PVS_NUM_CNTRLS.
     * See r500 docs 6.5.2 - done in emit */

    /* avoid division by zero */
    if (input_count == 0) input_count = 1;
    if (output_count == 0) output_count = 1;
    if (temp_count == 0) temp_count = 1;

    if (rmesa->radeon.radeonScreen->chip_family >= CHIP_FAMILY_RV515)
	vtx_mem_size = 128;
    else
	vtx_mem_size = 72;

    pvs_num_slots = MIN3(10, vtx_mem_size/input_count, vtx_mem_size/output_count);
    pvs_num_cntrls = MIN2(6, vtx_mem_size/temp_count);

    R300_STATECHANGE(rmesa, vap_cntl);
    if (rmesa->radeon.radeonScreen->chip_flags & RADEON_CHIPSET_TCL) {
	rmesa->hw.vap_cntl.cmd[R300_VAP_CNTL_INSTR] =
	    (pvs_num_slots << R300_PVS_NUM_SLOTS_SHIFT) |
	    (pvs_num_cntrls << R300_PVS_NUM_CNTLRS_SHIFT) |
	    (12 << R300_VF_MAX_VTX_NUM_SHIFT);
	if (rmesa->radeon.radeonScreen->chip_family >= CHIP_FAMILY_RV515)
	    rmesa->hw.vap_cntl.cmd[R300_VAP_CNTL_INSTR] |= R500_TCL_STATE_OPTIMIZATION;
    } else
	/* not sure about non-tcl */
	rmesa->hw.vap_cntl.cmd[R300_VAP_CNTL_INSTR] = ((10 << R300_PVS_NUM_SLOTS_SHIFT) |
				    (5 << R300_PVS_NUM_CNTLRS_SHIFT) |
				    (5 << R300_VF_MAX_VTX_NUM_SHIFT));

    if (rmesa->radeon.radeonScreen->chip_family == CHIP_FAMILY_RV515)
	rmesa->hw.vap_cntl.cmd[R300_VAP_CNTL_INSTR] |= (2 << R300_PVS_NUM_FPUS_SHIFT);
    else if ((rmesa->radeon.radeonScreen->chip_family == CHIP_FAMILY_RV530) ||
	     (rmesa->radeon.radeonScreen->chip_family == CHIP_FAMILY_RV560) ||
	     (rmesa->radeon.radeonScreen->chip_family == CHIP_FAMILY_RV570))
	rmesa->hw.vap_cntl.cmd[R300_VAP_CNTL_INSTR] |= (5 << R300_PVS_NUM_FPUS_SHIFT);
    else if ((rmesa->radeon.radeonScreen->chip_family == CHIP_FAMILY_RV410) ||
	     (rmesa->radeon.radeonScreen->chip_family == CHIP_FAMILY_R420))
	rmesa->hw.vap_cntl.cmd[R300_VAP_CNTL_INSTR] |= (6 << R300_PVS_NUM_FPUS_SHIFT);
    else if ((rmesa->radeon.radeonScreen->chip_family == CHIP_FAMILY_R520) ||
	     (rmesa->radeon.radeonScreen->chip_family == CHIP_FAMILY_R580))
	rmesa->hw.vap_cntl.cmd[R300_VAP_CNTL_INSTR] |= (8 << R300_PVS_NUM_FPUS_SHIFT);
    else
	rmesa->hw.vap_cntl.cmd[R300_VAP_CNTL_INSTR] |= (4 << R300_PVS_NUM_FPUS_SHIFT);

}

static void r300SetupDefaultVertexProgram(r300ContextPtr rmesa)
{
	struct r300_vertex_shader_state *prog = &(rmesa->state.vertex_shader);
	GLuint o_reg = 0;
	GLuint i_reg = 0;
	int i;
	int inst_count = 0;
	int param_count = 0;
	int program_end = 0;

	for (i = VERT_ATTRIB_POS; i < VERT_ATTRIB_MAX; i++) {
		if (rmesa->state.sw_tcl_inputs[i] != -1) {
			prog->program.body.i[program_end + 0] = PVS_OP_DST_OPERAND(VE_MULTIPLY, GL_FALSE, GL_FALSE, o_reg++, VSF_FLAG_ALL, PVS_DST_REG_OUT);
			prog->program.body.i[program_end + 1] = PVS_SRC_OPERAND(rmesa->state.sw_tcl_inputs[i], PVS_SRC_SELECT_X, PVS_SRC_SELECT_Y, PVS_SRC_SELECT_Z, PVS_SRC_SELECT_W, PVS_SRC_REG_INPUT, VSF_FLAG_NONE);
			prog->program.body.i[program_end + 2] = PVS_SRC_OPERAND(rmesa->state.sw_tcl_inputs[i], PVS_SRC_SELECT_FORCE_1, PVS_SRC_SELECT_FORCE_1, PVS_SRC_SELECT_FORCE_1, PVS_SRC_SELECT_FORCE_1, PVS_SRC_REG_INPUT, VSF_FLAG_NONE);
			prog->program.body.i[program_end + 3] = PVS_SRC_OPERAND(rmesa->state.sw_tcl_inputs[i], PVS_SRC_SELECT_FORCE_1, PVS_SRC_SELECT_FORCE_1, PVS_SRC_SELECT_FORCE_1, PVS_SRC_SELECT_FORCE_1, PVS_SRC_REG_INPUT, VSF_FLAG_NONE);
			program_end += 4;
			i_reg++;
		}
	}

	prog->program.length = program_end;

	r300SetupVertexProgramFragment(rmesa, R300_PVS_CODE_START,
				       &(prog->program));
	inst_count = (prog->program.length / 4) - 1;

	r300VapCntl(rmesa, i_reg, o_reg, 0);

	R300_STATECHANGE(rmesa, pvs);
	rmesa->hw.pvs.cmd[R300_PVS_CNTL_1] =
	    (0 << R300_PVS_FIRST_INST_SHIFT) |
	    (inst_count << R300_PVS_XYZW_VALID_INST_SHIFT) |
	    (inst_count << R300_PVS_LAST_INST_SHIFT);
	rmesa->hw.pvs.cmd[R300_PVS_CNTL_2] =
	    (0 << R300_PVS_CONST_BASE_OFFSET_SHIFT) |
	    (param_count << R300_PVS_MAX_CONST_ADDR_SHIFT);
	rmesa->hw.pvs.cmd[R300_PVS_CNTL_3] =
	    (inst_count << R300_PVS_LAST_VTX_SRC_INST_SHIFT);
}

static int bit_count (int x)
{
    x = ((x & 0xaaaaaaaaU) >> 1) + (x & 0x55555555U);
    x = ((x & 0xccccccccU) >> 2) + (x & 0x33333333U);
    x = (x >> 16) + (x & 0xffff);
    x = ((x & 0xf0f0) >> 4) + (x & 0x0f0f);
    return (x >> 8) + (x & 0x00ff);
}

static void r300SetupRealVertexProgram(r300ContextPtr rmesa)
{
	GLcontext *ctx = rmesa->radeon.glCtx;
	struct r300_vertex_program *prog = (struct r300_vertex_program *)CURRENT_VERTEX_SHADER(ctx);
	int inst_count = 0;
	int param_count = 0;

	/* FIXME: r300SetupVertexProgramFragment */
	R300_STATECHANGE(rmesa, vpp);
	param_count =
	    r300VertexProgUpdateParams(ctx,
				       (struct r300_vertex_program_cont *)
				       ctx->VertexProgram._Current,
				       (float *)&rmesa->hw.vpp.
				       cmd[R300_VPP_PARAM_0]);
	bump_vpu_count(rmesa->hw.vpp.cmd, param_count);
	param_count /= 4;

	r300SetupVertexProgramFragment(rmesa, R300_PVS_CODE_START, &(prog->program));
	inst_count = (prog->program.length / 4) - 1;

	r300VapCntl(rmesa, bit_count(prog->key.InputsRead),
		    bit_count(prog->key.OutputsWritten), prog->num_temporaries);

	R300_STATECHANGE(rmesa, pvs);
	rmesa->hw.pvs.cmd[R300_PVS_CNTL_1] =
	  (0 << R300_PVS_FIRST_INST_SHIFT) |
	  (inst_count << R300_PVS_XYZW_VALID_INST_SHIFT) |
	  (inst_count << R300_PVS_LAST_INST_SHIFT);
	rmesa->hw.pvs.cmd[R300_PVS_CNTL_2] =
	  (0 << R300_PVS_CONST_BASE_OFFSET_SHIFT) |
	  (param_count << R300_PVS_MAX_CONST_ADDR_SHIFT);
	rmesa->hw.pvs.cmd[R300_PVS_CNTL_3] =
	  (inst_count << R300_PVS_LAST_VTX_SRC_INST_SHIFT);
}

static void r300SetupVertexProgram(r300ContextPtr rmesa)
{
	GLcontext *ctx = rmesa->radeon.glCtx;

	/* Reset state, in case we don't use something */
	((drm_r300_cmd_header_t *) rmesa->hw.vpp.cmd)->vpu.count = 0;
	((drm_r300_cmd_header_t *) rmesa->hw.vpi.cmd)->vpu.count = 0;
	((drm_r300_cmd_header_t *) rmesa->hw.vps.cmd)->vpu.count = 0;

	/* Not sure why this doesnt work...
	   0x400 area might have something to do with pixel shaders as it appears right after pfs programming.
	   0x406 is set to { 0.0, 0.0, 1.0, 0.0 } most of the time but should change with smooth points and in other rare cases. */
	//setup_vertex_shader_fragment(rmesa, 0x406, &unk4);
	if (hw_tcl_on && ((struct r300_vertex_program *)CURRENT_VERTEX_SHADER(ctx))->translated) {
		r300SetupRealVertexProgram(rmesa);
	} else {
		/* FIXME: This needs to be replaced by vertex shader generation code. */
		r300SetupDefaultVertexProgram(rmesa);
	}

}

/**
 * Enable/Disable states.
 *
 * \note Mesa already filters redundant calls to this function.
 */
static void r300Enable(GLcontext * ctx, GLenum cap, GLboolean state)
{
	if (RADEON_DEBUG & DEBUG_STATE)
		fprintf(stderr, "%s( %s = %s )\n", __FUNCTION__,
			_mesa_lookup_enum_by_nr(cap),
			state ? "GL_TRUE" : "GL_FALSE");

	switch (cap) {
	case GL_TEXTURE_1D:
	case GL_TEXTURE_2D:
	case GL_TEXTURE_3D:
		/* empty */
		break;
	case GL_FOG:
		r300SetFogState(ctx, state);
		break;
	case GL_ALPHA_TEST:
		r300SetAlphaState(ctx);
		break;
	case GL_COLOR_LOGIC_OP:
		r300SetLogicOpState(ctx);
		/* fall-through, because logic op overrides blending */
	case GL_BLEND:
		r300SetBlendState(ctx);
		break;
	case GL_CLIP_PLANE0:
	case GL_CLIP_PLANE1:
	case GL_CLIP_PLANE2:
	case GL_CLIP_PLANE3:
	case GL_CLIP_PLANE4:
	case GL_CLIP_PLANE5:
		r300SetClipPlaneState(ctx, cap, state);
		break;
	case GL_DEPTH_TEST:
		r300SetDepthState(ctx);
		break;
	case GL_STENCIL_TEST:
		r300SetStencilState(ctx, state);
		break;
	case GL_CULL_FACE:
		r300UpdateCulling(ctx);
		break;
	case GL_POLYGON_OFFSET_POINT:
	case GL_POLYGON_OFFSET_LINE:
	case GL_POLYGON_OFFSET_FILL:
		r300SetPolygonOffsetState(ctx, state);
		break;
	default:
		radeonEnable(ctx, cap, state);
		break;
	}
}

/**
 * Completely recalculates hardware state based on the Mesa state.
 */
static void r300ResetHwState(r300ContextPtr r300)
{
	GLcontext *ctx = r300->radeon.glCtx;
	int has_tcl = 1;

	if (!(r300->radeon.radeonScreen->chip_flags & RADEON_CHIPSET_TCL))
		has_tcl = 0;

	if (RADEON_DEBUG & DEBUG_STATE)
		fprintf(stderr, "%s\n", __FUNCTION__);

	r300UpdateWindow(ctx);

	r300ColorMask(ctx,
		      ctx->Color.ColorMask[RCOMP],
		      ctx->Color.ColorMask[GCOMP],
		      ctx->Color.ColorMask[BCOMP], ctx->Color.ColorMask[ACOMP]);

	r300Enable(ctx, GL_DEPTH_TEST, ctx->Depth.Test);
	r300DepthMask(ctx, ctx->Depth.Mask);
	r300DepthFunc(ctx, ctx->Depth.Func);

	/* stencil */
	r300Enable(ctx, GL_STENCIL_TEST, ctx->Stencil.Enabled);
	r300StencilMaskSeparate(ctx, 0, ctx->Stencil.WriteMask[0]);
	r300StencilFuncSeparate(ctx, 0, ctx->Stencil.Function[0],
				ctx->Stencil.Ref[0], ctx->Stencil.ValueMask[0]);
	r300StencilOpSeparate(ctx, 0, ctx->Stencil.FailFunc[0],
			      ctx->Stencil.ZFailFunc[0],
			      ctx->Stencil.ZPassFunc[0]);

	r300UpdateCulling(ctx);

	r300UpdateTextureState(ctx);

	r300SetBlendState(ctx);
	r300SetLogicOpState(ctx);

	r300AlphaFunc(ctx, ctx->Color.AlphaFunc, ctx->Color.AlphaRef);
	r300Enable(ctx, GL_ALPHA_TEST, ctx->Color.AlphaEnabled);

	r300->hw.vte.cmd[1] = R300_VPORT_X_SCALE_ENA
	    | R300_VPORT_X_OFFSET_ENA
	    | R300_VPORT_Y_SCALE_ENA
	    | R300_VPORT_Y_OFFSET_ENA
	    | R300_VPORT_Z_SCALE_ENA
	    | R300_VPORT_Z_OFFSET_ENA | R300_VTX_W0_FMT;
	r300->hw.vte.cmd[2] = 0x00000008;

	r300->hw.vap_vf_max_vtx_indx.cmd[1] = 0x00FFFFFF;
	r300->hw.vap_vf_max_vtx_indx.cmd[2] = 0x00000000;

#ifdef MESA_LITTLE_ENDIAN
	r300->hw.vap_cntl_status.cmd[1] = R300_VC_NO_SWAP;
#else
	r300->hw.vap_cntl_status.cmd[1] = R300_VC_32BIT_SWAP;
#endif

	/* disable VAP/TCL on non-TCL capable chips */
	if (!has_tcl)
		r300->hw.vap_cntl_status.cmd[1] |= R300_VAP_TCL_BYPASS;

	r300->hw.vap_psc_sgn_norm_cntl.cmd[1] = 0xAAAAAAAA;

	/* XXX: Other families? */
	if (has_tcl) {
		r300->hw.vap_clip_cntl.cmd[1] = R300_PS_UCP_MODE_DIST_COP;

		r300->hw.vap_clip.cmd[1] = r300PackFloat32(1.0); /* X */
		r300->hw.vap_clip.cmd[2] = r300PackFloat32(1.0); /* X */
		r300->hw.vap_clip.cmd[3] = r300PackFloat32(1.0); /* Y */
		r300->hw.vap_clip.cmd[4] = r300PackFloat32(1.0); /* Y */

		switch (r300->radeon.radeonScreen->chip_family) {
		case CHIP_FAMILY_R300:
			r300->hw.vap_pvs_vtx_timeout_reg.cmd[1] = R300_2288_R300;
			break;
		default:
			r300->hw.vap_pvs_vtx_timeout_reg.cmd[1] = R300_2288_RV350;
			break;
		}
	}

	r300->hw.gb_enable.cmd[1] = R300_GB_POINT_STUFF_ENABLE
	    | R300_GB_LINE_STUFF_ENABLE
	    | R300_GB_TRIANGLE_STUFF_ENABLE;

	r300->hw.gb_misc.cmd[R300_GB_MISC_MSPOS_0] = 0x66666666;
	r300->hw.gb_misc.cmd[R300_GB_MISC_MSPOS_1] = 0x06666666;

	r300->hw.gb_misc.cmd[R300_GB_MISC_TILE_CONFIG] =
	    R300_GB_TILE_ENABLE | R300_GB_TILE_SIZE_16 /*| R300_GB_SUBPIXEL_1_16*/;
	switch (r300->radeon.radeonScreen->num_gb_pipes) {
	case 1:
	default:
		r300->hw.gb_misc.cmd[R300_GB_MISC_TILE_CONFIG] |=
		    R300_GB_TILE_PIPE_COUNT_RV300;
		break;
	case 2:
		r300->hw.gb_misc.cmd[R300_GB_MISC_TILE_CONFIG] |=
		    R300_GB_TILE_PIPE_COUNT_R300;
		break;
	case 3:
		r300->hw.gb_misc.cmd[R300_GB_MISC_TILE_CONFIG] |=
		    R300_GB_TILE_PIPE_COUNT_R420_3P;
		break;
	case 4:
		r300->hw.gb_misc.cmd[R300_GB_MISC_TILE_CONFIG] |=
		    R300_GB_TILE_PIPE_COUNT_R420;
		break;
	}

	/* XXX: set to 0 when fog is disabled? */
	r300->hw.gb_misc.cmd[R300_GB_MISC_SELECT] = R300_GB_FOG_SELECT_1_1_W;

	/* XXX: Enable anti-aliasing? */
	r300->hw.gb_misc.cmd[R300_GB_MISC_AA_CONFIG] = GB_AA_CONFIG_AA_DISABLE;

	r300->hw.ga_point_s0.cmd[1] = r300PackFloat32(0.0);
	r300->hw.ga_point_s0.cmd[2] = r300PackFloat32(0.0);
	r300->hw.ga_point_s0.cmd[3] = r300PackFloat32(1.0);
	r300->hw.ga_point_s0.cmd[4] = r300PackFloat32(1.0);

	r300->hw.ga_triangle_stipple.cmd[1] = 0x00050005;

	r300PointSize(ctx, 1.0);

	r300->hw.ga_point_minmax.cmd[1] = 0x18000006;
	r300->hw.ga_point_minmax.cmd[2] = 0x00020006;
	r300->hw.ga_point_minmax.cmd[3] = r300PackFloat32(1.0 / 192.0);

	r300LineWidth(ctx, 1.0);

	r300->hw.ga_line_stipple.cmd[1] = 0;
	r300->hw.ga_line_stipple.cmd[2] = r300PackFloat32(0.0);
	r300->hw.ga_line_stipple.cmd[3] = r300PackFloat32(1.0);

	r300ShadeModel(ctx, ctx->Light.ShadeModel);

	r300PolygonMode(ctx, GL_FRONT, ctx->Polygon.FrontMode);
	r300PolygonMode(ctx, GL_BACK, ctx->Polygon.BackMode);
	r300->hw.zbias_cntl.cmd[1] = 0x00000000;

	r300PolygonOffset(ctx, ctx->Polygon.OffsetFactor,
			  ctx->Polygon.OffsetUnits);
	r300Enable(ctx, GL_POLYGON_OFFSET_POINT, ctx->Polygon.OffsetPoint);
	r300Enable(ctx, GL_POLYGON_OFFSET_LINE, ctx->Polygon.OffsetLine);
	r300Enable(ctx, GL_POLYGON_OFFSET_FILL, ctx->Polygon.OffsetFill);

	r300->hw.su_depth_scale.cmd[1] = 0x4B7FFFFF;
	r300->hw.su_depth_scale.cmd[2] = 0x00000000;

	r300->hw.sc_hyperz.cmd[1] = 0x0000001C;
	r300->hw.sc_hyperz.cmd[2] = 0x2DA49525;

	r300->hw.sc_screendoor.cmd[1] = 0x00FFFFFF;

	r300->hw.us_out_fmt.cmd[1] = R500_OUT_FMT_C4_8  |
	  R500_C0_SEL_B | R500_C1_SEL_G | R500_C2_SEL_R | R500_C3_SEL_A;
	r300->hw.us_out_fmt.cmd[2] = R500_OUT_FMT_UNUSED |
	  R500_C0_SEL_B | R500_C1_SEL_G | R500_C2_SEL_R | R500_C3_SEL_A;
	r300->hw.us_out_fmt.cmd[3] = R500_OUT_FMT_UNUSED |
	  R500_C0_SEL_B | R500_C1_SEL_G | R500_C2_SEL_R | R500_C3_SEL_A;
	r300->hw.us_out_fmt.cmd[4] = R500_OUT_FMT_UNUSED |
	  R500_C0_SEL_B | R500_C1_SEL_G | R500_C2_SEL_R | R500_C3_SEL_A;
	r300->hw.us_out_fmt.cmd[5] = R300_W_FMT_W24;

	r300Enable(ctx, GL_FOG, ctx->Fog.Enabled);
	r300Fogfv(ctx, GL_FOG_MODE, NULL);
	r300Fogfv(ctx, GL_FOG_DENSITY, &ctx->Fog.Density);
	r300Fogfv(ctx, GL_FOG_START, &ctx->Fog.Start);
	r300Fogfv(ctx, GL_FOG_END, &ctx->Fog.End);
	r300Fogfv(ctx, GL_FOG_COLOR, ctx->Fog.Color);
	r300Fogfv(ctx, GL_FOG_COORDINATE_SOURCE_EXT, NULL);

	r300->hw.fg_depth_src.cmd[1] = 0;

	r300->hw.rb3d_cctl.cmd[1] = 0;

	r300BlendColor(ctx, ctx->Color.BlendColor);

	/* Again, r300ClearBuffer uses this */
	r300->hw.cb.cmd[R300_CB_OFFSET] =
	    r300->radeon.state.color.drawOffset +
	    r300->radeon.radeonScreen->fbLocation;
	r300->hw.cb.cmd[R300_CB_PITCH] = r300->radeon.state.color.drawPitch;

	if (r300->radeon.radeonScreen->cpp == 4)
		r300->hw.cb.cmd[R300_CB_PITCH] |= R300_COLOR_FORMAT_ARGB8888;
	else
		r300->hw.cb.cmd[R300_CB_PITCH] |= R300_COLOR_FORMAT_RGB565;

	if (r300->radeon.sarea->tiling_enabled)
		r300->hw.cb.cmd[R300_CB_PITCH] |= R300_COLOR_TILE_ENABLE;

	r300->hw.rb3d_dither_ctl.cmd[1] = 0;
	r300->hw.rb3d_dither_ctl.cmd[2] = 0;
	r300->hw.rb3d_dither_ctl.cmd[3] = 0;
	r300->hw.rb3d_dither_ctl.cmd[4] = 0;
	r300->hw.rb3d_dither_ctl.cmd[5] = 0;
	r300->hw.rb3d_dither_ctl.cmd[6] = 0;
	r300->hw.rb3d_dither_ctl.cmd[7] = 0;
	r300->hw.rb3d_dither_ctl.cmd[8] = 0;
	r300->hw.rb3d_dither_ctl.cmd[9] = 0;

	r300->hw.rb3d_aaresolve_ctl.cmd[1] = 0;

	r300->hw.rb3d_discard_src_pixel_lte_threshold.cmd[1] = 0x00000000;
	r300->hw.rb3d_discard_src_pixel_lte_threshold.cmd[2] = 0xffffffff;

	r300->hw.zb.cmd[R300_ZB_OFFSET] =
	    r300->radeon.radeonScreen->depthOffset +
	    r300->radeon.radeonScreen->fbLocation;
	r300->hw.zb.cmd[R300_ZB_PITCH] = r300->radeon.radeonScreen->depthPitch;

	if (r300->radeon.sarea->tiling_enabled) {
		/* XXX: Turn off when clearing buffers ? */
		r300->hw.zb.cmd[R300_ZB_PITCH] |= R300_DEPTHMACROTILE_ENABLE;

		if (ctx->Visual.depthBits == 24)
			r300->hw.zb.cmd[R300_ZB_PITCH] |=
			    R300_DEPTHMICROTILE_TILED;
	}

	r300->hw.zb_depthclearvalue.cmd[1] = 0;

	switch (ctx->Visual.depthBits) {
	case 16:
		r300->hw.zstencil_format.cmd[1] = R300_DEPTHFORMAT_16BIT_INT_Z;
		break;
	case 24:
		r300->hw.zstencil_format.cmd[1] = R300_DEPTHFORMAT_24BIT_INT_Z_8BIT_STENCIL;
		break;
	default:
		fprintf(stderr, "Error: Unsupported depth %d... exiting\n", ctx->Visual.depthBits);
		_mesa_exit(-1);
	}

	r300->hw.zstencil_format.cmd[2] = R300_ZTOP_DISABLE;
	r300->hw.zstencil_format.cmd[3] = 0x00000003;
	r300->hw.zstencil_format.cmd[4] = 0x00000000;
	r300SetEarlyZState(ctx);

	r300->hw.unk4F30.cmd[1] = 0;
	r300->hw.unk4F30.cmd[2] = 0;

	r300->hw.zb_hiz_offset.cmd[1] = 0;

	r300->hw.zb_hiz_pitch.cmd[1] = 0;

	r300VapCntl(r300, 0, 0, 0);
	if (has_tcl) {
		r300->hw.vps.cmd[R300_VPS_ZERO_0] = 0;
		r300->hw.vps.cmd[R300_VPS_ZERO_1] = 0;
		r300->hw.vps.cmd[R300_VPS_POINTSIZE] = r300PackFloat32(1.0);
		r300->hw.vps.cmd[R300_VPS_ZERO_3] = 0;
	}

	r300->hw.all_dirty = GL_TRUE;
}

void r300UpdateShaders(r300ContextPtr rmesa)
{
	GLcontext *ctx;
	struct r300_vertex_program *vp;
	int i;

	ctx = rmesa->radeon.glCtx;

	if (rmesa->NewGLState && hw_tcl_on) {
		rmesa->NewGLState = 0;

		for (i = _TNL_FIRST_MAT; i <= _TNL_LAST_MAT; i++) {
			rmesa->temp_attrib[i] =
			    TNL_CONTEXT(ctx)->vb.AttribPtr[i];
			TNL_CONTEXT(ctx)->vb.AttribPtr[i] =
			    &rmesa->dummy_attrib[i];
		}

		_tnl_UpdateFixedFunctionProgram(ctx);

		for (i = _TNL_FIRST_MAT; i <= _TNL_LAST_MAT; i++) {
			TNL_CONTEXT(ctx)->vb.AttribPtr[i] =
			    rmesa->temp_attrib[i];
		}

		r300SelectVertexShader(rmesa);
		vp = (struct r300_vertex_program *)
		    CURRENT_VERTEX_SHADER(ctx);
		/*if (vp->translated == GL_FALSE)
		   r300TranslateVertexShader(vp); */
		if (vp->translated == GL_FALSE) {
			fprintf(stderr, "Failing back to sw-tcl\n");
			hw_tcl_on = future_hw_tcl_on = 0;
			r300ResetHwState(rmesa);

			r300UpdateStateParameters(ctx, _NEW_PROGRAM);
			return;
		}
	}
	r300UpdateStateParameters(ctx, _NEW_PROGRAM);
}

static const GLfloat *get_fragmentprogram_constant(GLcontext *ctx,
	struct gl_program *program, struct prog_src_register srcreg)
{
	static const GLfloat dummy[4] = { 0, 0, 0, 0 };

	switch(srcreg.File) {
	case PROGRAM_LOCAL_PARAM:
		return program->LocalParams[srcreg.Index];
	case PROGRAM_ENV_PARAM:
		return ctx->FragmentProgram.Parameters[srcreg.Index];
	case PROGRAM_STATE_VAR:
	case PROGRAM_NAMED_PARAM:
	case PROGRAM_CONSTANT:
		return program->Parameters->ParameterValues[srcreg.Index];
	default:
		_mesa_problem(ctx, "get_fragmentprogram_constant: Unknown\n");
		return dummy;
	}
}


static void r300SetupPixelShader(r300ContextPtr rmesa)
{
	GLcontext *ctx = rmesa->radeon.glCtx;
	struct r300_fragment_program *fp = (struct r300_fragment_program *)
	    (char *)ctx->FragmentProgram._Current;
	struct r300_fragment_program_code *code;
	int i, k;

	if (!fp)		/* should only happenen once, just after context is created */
		return;

	r300TranslateFragmentShader(rmesa, fp);
	if (!fp->translated) {
		fprintf(stderr, "%s: No valid fragment shader, exiting\n",
			__FUNCTION__);
		return;
	}
	code = &fp->code;

	r300SetupTextures(ctx);

	R300_STATECHANGE(rmesa, fpi[0]);
	R300_STATECHANGE(rmesa, fpi[1]);
	R300_STATECHANGE(rmesa, fpi[2]);
	R300_STATECHANGE(rmesa, fpi[3]);
	rmesa->hw.fpi[0].cmd[R300_FPI_CMD_0] = cmdpacket0(R300_US_ALU_RGB_INST_0, code->alu.length);
	rmesa->hw.fpi[1].cmd[R300_FPI_CMD_0] = cmdpacket0(R300_US_ALU_RGB_ADDR_0, code->alu.length);
	rmesa->hw.fpi[2].cmd[R300_FPI_CMD_0] = cmdpacket0(R300_US_ALU_ALPHA_INST_0, code->alu.length);
	rmesa->hw.fpi[3].cmd[R300_FPI_CMD_0] = cmdpacket0(R300_US_ALU_ALPHA_ADDR_0, code->alu.length);
	for (i = 0; i < code->alu.length; i++) {
		rmesa->hw.fpi[0].cmd[R300_FPI_INSTR_0 + i] = code->alu.inst[i].inst0;
		rmesa->hw.fpi[1].cmd[R300_FPI_INSTR_0 + i] = code->alu.inst[i].inst1;
		rmesa->hw.fpi[2].cmd[R300_FPI_INSTR_0 + i] = code->alu.inst[i].inst2;
		rmesa->hw.fpi[3].cmd[R300_FPI_INSTR_0 + i] = code->alu.inst[i].inst3;
	}

	R300_STATECHANGE(rmesa, fp);
	rmesa->hw.fp.cmd[R300_FP_CNTL0] = code->cur_node | (code->first_node_has_tex << 3);
	rmesa->hw.fp.cmd[R300_FP_CNTL1] = code->max_temp_idx;
	rmesa->hw.fp.cmd[R300_FP_CNTL2] =
	  (0 << R300_PFS_CNTL_ALU_OFFSET_SHIFT) |
	  ((code->alu.length-1) << R300_PFS_CNTL_ALU_END_SHIFT) |
	  (0 << R300_PFS_CNTL_TEX_OFFSET_SHIFT) |
	  ((code->tex.length ? code->tex.length-1 : 0) << R300_PFS_CNTL_TEX_END_SHIFT);
	/* I just want to say, the way these nodes are stored.. weird.. */
	for (i = 0, k = (4 - (code->cur_node + 1)); i < 4; i++, k++) {
		if (i < (code->cur_node + 1)) {
			rmesa->hw.fp.cmd[R300_FP_NODE0 + k] =
			  (code->node[i].alu_offset << R300_ALU_START_SHIFT) |
			  (code->node[i].alu_end << R300_ALU_SIZE_SHIFT) |
			  (code->node[i].tex_offset << R300_TEX_START_SHIFT) |
			  (code->node[i].tex_end << R300_TEX_SIZE_SHIFT) |
			  code->node[i].flags;
		} else {
			rmesa->hw.fp.cmd[R300_FP_NODE0 + (3 - i)] = 0;
		}
	}

	R300_STATECHANGE(rmesa, fpp);
	rmesa->hw.fpp.cmd[R300_FPP_CMD_0] = cmdpacket0(R300_PFS_PARAM_0_X, code->const_nr * 4);
	for (i = 0; i < code->const_nr; i++) {
		const GLfloat *constant = get_fragmentprogram_constant(ctx,
			&fp->mesa_program.Base, code->constant[i]);
		rmesa->hw.fpp.cmd[R300_FPP_PARAM_0 + 4 * i + 0] = r300PackFloat24(constant[0]);
		rmesa->hw.fpp.cmd[R300_FPP_PARAM_0 + 4 * i + 1] = r300PackFloat24(constant[1]);
		rmesa->hw.fpp.cmd[R300_FPP_PARAM_0 + 4 * i + 2] = r300PackFloat24(constant[2]);
		rmesa->hw.fpp.cmd[R300_FPP_PARAM_0 + 4 * i + 3] = r300PackFloat24(constant[3]);
	}
}

#define bump_r500fp_count(ptr, new_count)   do{\
	drm_r300_cmd_header_t* _p=((drm_r300_cmd_header_t*)(ptr));\
	int _nc=(new_count)/6; \
	assert(_nc < 256); \
	if(_nc>_p->r500fp.count)_p->r500fp.count=_nc;\
} while(0)

#define bump_r500fp_const_count(ptr, new_count)   do{\
	drm_r300_cmd_header_t* _p=((drm_r300_cmd_header_t*)(ptr));\
	int _nc=(new_count)/4; \
	assert(_nc < 256); \
	if(_nc>_p->r500fp.count)_p->r500fp.count=_nc;\
} while(0)

static void r500SetupPixelShader(r300ContextPtr rmesa)
{
	GLcontext *ctx = rmesa->radeon.glCtx;
	struct r500_fragment_program *fp = (struct r500_fragment_program *)
	    (char *)ctx->FragmentProgram._Current;
	int i;
	struct r500_fragment_program_code *code;

	if (!fp)		/* should only happenen once, just after context is created */
		return;

	((drm_r300_cmd_header_t *) rmesa->hw.r500fp.cmd)->r500fp.count = 0;
	((drm_r300_cmd_header_t *) rmesa->hw.r500fp_const.cmd)->r500fp.count = 0;

	r500TranslateFragmentShader(rmesa, fp);
	if (!fp->translated) {
		fprintf(stderr, "%s: No valid fragment shader, exiting\n",
			__FUNCTION__);
		return;
	}
	code = &fp->code;

	if (fp->mesa_program.FogOption != GL_NONE) {
		/* Enable HW fog. Try not to squish GL context.
		 * (Anybody sane remembered to set glFog() opts first!) */
		r300SetFogState(ctx, GL_TRUE);
		ctx->Fog.Mode = fp->mesa_program.FogOption;
		r300Fogfv(ctx, GL_FOG_MODE, NULL);
	} else
		/* Make sure HW is matching GL context. */
		r300SetFogState(ctx, ctx->Fog.Enabled);

	r300SetupTextures(ctx);

	R300_STATECHANGE(rmesa, fp);
	rmesa->hw.fp.cmd[R500_FP_PIXSIZE] = code->max_temp_idx;

	rmesa->hw.fp.cmd[R500_FP_CODE_ADDR] =
	    R500_US_CODE_START_ADDR(code->inst_offset) |
	    R500_US_CODE_END_ADDR(code->inst_end);
	rmesa->hw.fp.cmd[R500_FP_CODE_RANGE] =
	    R500_US_CODE_RANGE_ADDR(code->inst_offset) |
	    R500_US_CODE_RANGE_SIZE(code->inst_end);
	rmesa->hw.fp.cmd[R500_FP_CODE_OFFSET] =
	    R500_US_CODE_OFFSET_ADDR(0); /* FIXME when we add flow control */

	R300_STATECHANGE(rmesa, r500fp);
	/* Emit our shader... */
	for (i = 0; i < code->inst_end+1; i++) {
		rmesa->hw.r500fp.cmd[i*6+1] = code->inst[i].inst0;
		rmesa->hw.r500fp.cmd[i*6+2] = code->inst[i].inst1;
		rmesa->hw.r500fp.cmd[i*6+3] = code->inst[i].inst2;
		rmesa->hw.r500fp.cmd[i*6+4] = code->inst[i].inst3;
		rmesa->hw.r500fp.cmd[i*6+5] = code->inst[i].inst4;
		rmesa->hw.r500fp.cmd[i*6+6] = code->inst[i].inst5;
	}

	bump_r500fp_count(rmesa->hw.r500fp.cmd, (code->inst_end + 1) * 6);

	R300_STATECHANGE(rmesa, r500fp_const);
	for (i = 0; i < code->const_nr; i++) {
		const GLfloat *constant = get_fragmentprogram_constant(ctx,
			&fp->mesa_program.Base, code->constant[i]);
		rmesa->hw.r500fp_const.cmd[R300_FPP_PARAM_0 + 4 * i + 0] = r300PackFloat32(constant[0]);
		rmesa->hw.r500fp_const.cmd[R300_FPP_PARAM_0 + 4 * i + 1] = r300PackFloat32(constant[1]);
		rmesa->hw.r500fp_const.cmd[R300_FPP_PARAM_0 + 4 * i + 2] = r300PackFloat32(constant[2]);
		rmesa->hw.r500fp_const.cmd[R300_FPP_PARAM_0 + 4 * i + 3] = r300PackFloat32(constant[3]);
	}
	bump_r500fp_const_count(rmesa->hw.r500fp_const.cmd, code->const_nr * 4);

}

void r300UpdateShaderStates(r300ContextPtr rmesa)
{
	GLcontext *ctx;
	ctx = rmesa->radeon.glCtx;

	r300UpdateTextureState(ctx);
	r300SetEarlyZState(ctx);

	GLuint fgdepthsrc = R300_FG_DEPTH_SRC_SCAN;
	if (current_fragment_program_writes_depth(ctx))
		fgdepthsrc = R300_FG_DEPTH_SRC_SHADER;
	if (fgdepthsrc != rmesa->hw.fg_depth_src.cmd[1]) {
		R300_STATECHANGE(rmesa, fg_depth_src);
		rmesa->hw.fg_depth_src.cmd[1] = fgdepthsrc;
	}

	if (rmesa->radeon.radeonScreen->chip_family >= CHIP_FAMILY_RV515)
		r500SetupPixelShader(rmesa);
	else
		r300SetupPixelShader(rmesa);

	if (rmesa->radeon.radeonScreen->chip_family >= CHIP_FAMILY_RV515)
		r500SetupRSUnit(ctx);
	else
		r300SetupRSUnit(ctx);

	if ((rmesa->radeon.radeonScreen->chip_flags & RADEON_CHIPSET_TCL))
		r300SetupVertexProgram(rmesa);

}

/**
 * Called by Mesa after an internal state update.
 */
static void r300InvalidateState(GLcontext * ctx, GLuint new_state)
{
	r300ContextPtr r300 = R300_CONTEXT(ctx);

	_swrast_InvalidateState(ctx, new_state);
	_swsetup_InvalidateState(ctx, new_state);
	_vbo_InvalidateState(ctx, new_state);
	_tnl_InvalidateState(ctx, new_state);
	_ae_invalidate_state(ctx, new_state);

	if (new_state & (_NEW_BUFFERS | _NEW_COLOR | _NEW_PIXEL)) {
		r300UpdateDrawBuffer(ctx);
	}

	r300UpdateStateParameters(ctx, new_state);

	r300->NewGLState |= new_state;
}

/**
 * Calculate initial hardware state and register state functions.
 * Assumes that the command buffer and state atoms have been
 * initialized already.
 */
void r300InitState(r300ContextPtr r300)
{
	GLcontext *ctx = r300->radeon.glCtx;
	GLuint depth_fmt;

	radeonInitState(&r300->radeon);

	switch (ctx->Visual.depthBits) {
	case 16:
		r300->state.depth.scale = 1.0 / (GLfloat) 0xffff;
		depth_fmt = R300_DEPTHFORMAT_16BIT_INT_Z;
		break;
	case 24:
		r300->state.depth.scale = 1.0 / (GLfloat) 0xffffff;
		depth_fmt = R300_DEPTHFORMAT_24BIT_INT_Z_8BIT_STENCIL;
		break;
	default:
		fprintf(stderr, "Error: Unsupported depth %d... exiting\n",
			ctx->Visual.depthBits);
		_mesa_exit(-1);
	}

	/* Only have hw stencil when depth buffer is 24 bits deep */
	r300->state.stencil.hw_stencil = (ctx->Visual.stencilBits > 0 &&
					  ctx->Visual.depthBits == 24);

	memset(&(r300->state.texture), 0, sizeof(r300->state.texture));

	r300ResetHwState(r300);
}

static void r300RenderMode(GLcontext * ctx, GLenum mode)
{
	r300ContextPtr rmesa = R300_CONTEXT(ctx);
	(void)rmesa;
	(void)mode;
}

void r300UpdateClipPlanes( GLcontext *ctx )
{
	r300ContextPtr rmesa = R300_CONTEXT(ctx);
	GLuint p;

	for (p = 0; p < ctx->Const.MaxClipPlanes; p++) {
		if (ctx->Transform.ClipPlanesEnabled & (1 << p)) {
			GLint *ip = (GLint *)ctx->Transform._ClipUserPlane[p];

			R300_STATECHANGE( rmesa, vpucp[p] );
			rmesa->hw.vpucp[p].cmd[R300_VPUCP_X] = ip[0];
			rmesa->hw.vpucp[p].cmd[R300_VPUCP_Y] = ip[1];
			rmesa->hw.vpucp[p].cmd[R300_VPUCP_Z] = ip[2];
			rmesa->hw.vpucp[p].cmd[R300_VPUCP_W] = ip[3];
		}
	}
}

/**
 * Initialize driver's state callback functions
 */
void r300InitStateFuncs(struct dd_function_table *functions)
{
	radeonInitStateFuncs(functions);

	functions->UpdateState = r300InvalidateState;
	functions->AlphaFunc = r300AlphaFunc;
	functions->BlendColor = r300BlendColor;
	functions->BlendEquationSeparate = r300BlendEquationSeparate;
	functions->BlendFuncSeparate = r300BlendFuncSeparate;
	functions->Enable = r300Enable;
	functions->ColorMask = r300ColorMask;
	functions->DepthFunc = r300DepthFunc;
	functions->DepthMask = r300DepthMask;
	functions->CullFace = r300CullFace;
	functions->Fogfv = r300Fogfv;
	functions->FrontFace = r300FrontFace;
	functions->ShadeModel = r300ShadeModel;
	functions->LogicOpcode = r300LogicOpcode;

	/* ARB_point_parameters */
	functions->PointParameterfv = r300PointParameter;

	/* Stencil related */
	functions->StencilFuncSeparate = r300StencilFuncSeparate;
	functions->StencilMaskSeparate = r300StencilMaskSeparate;
	functions->StencilOpSeparate = r300StencilOpSeparate;

	/* Viewport related */
	functions->Viewport = r300Viewport;
	functions->DepthRange = r300DepthRange;
	functions->PointSize = r300PointSize;
	functions->LineWidth = r300LineWidth;

	functions->PolygonOffset = r300PolygonOffset;
	functions->PolygonMode = r300PolygonMode;

	functions->RenderMode = r300RenderMode;

	functions->ClipPlane = r300ClipPlane;
}
