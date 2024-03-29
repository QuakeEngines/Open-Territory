/*
===========================================================================
Copyright (C) 1999-2005 Id Software, Inc.
Copyright (C) 2006-2010 Robert Beckebans <trebor_7@users.sourceforge.net>

This file is part of XreaL source code.

XreaL source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

XreaL source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with XreaL source code; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/
// tr_shade.c
#include "tr_local.h"
#include "gl_shader.h"




//#define USE_GLSL_OPTIMIZER 1

#if defined(USE_GLSL_OPTIMIZER)
#include "../../libs/glsl-optimizer/src/glsl/glsl_optimizer.h"

static struct glslopt_ctx* s_glslOptimizer;
#endif

/*
=================================================================================
THIS ENTIRE FILE IS BACK END!

This file deals with applying shaders to surface data in the tess struct.
=================================================================================
*/

static void GLSL_PrintInfoLog(GLhandleARB object, qboolean developerOnly)
{
	char           *msg;
	static char     msgPart[1024];
	int             maxLength = 0;
	int             i;

	glGetObjectParameterivARB(object, GL_OBJECT_INFO_LOG_LENGTH_ARB, &maxLength);

	msg = (char *) Com_Allocate(maxLength);

	glGetInfoLogARB(object, maxLength, &maxLength, msg);

	if(developerOnly)
	{
		ri.Printf(PRINT_DEVELOPER, "compile log:\n");
	}
	else
	{
		ri.Printf(PRINT_ALL, "compile log:\n");
	}

	for(i = 0; i < maxLength; i += 1024)
	{
		Q_strncpyz(msgPart, msg + i, sizeof(msgPart));

		if(developerOnly)
			ri.Printf(PRINT_DEVELOPER, "%s\n", msgPart);
		else
			ri.Printf(PRINT_ALL, "%s\n", msgPart);
	}

	Com_Dealloc(msg);
}

static void GLSL_PrintShaderSource(GLhandleARB object)
{
	char           *msg;
	static char     msgPart[1024];
	int             maxLength = 0;
	int             i;

	glGetObjectParameterivARB(object, GL_OBJECT_SHADER_SOURCE_LENGTH_ARB, &maxLength);

	msg = (char *) Com_Allocate(maxLength);

	glGetShaderSourceARB(object, maxLength, &maxLength, msg);

	for(i = 0; i < maxLength; i += 1024)
	{
		Q_strncpyz(msgPart, msg + i, sizeof(msgPart));
		ri.Printf(PRINT_ALL, "%s\n", msgPart);
	}

	Com_Dealloc(msg);
}

static void GLSL_LoadGPUShader(GLhandleARB program, const char *name, const char *_libs, const char *_compileMacros, GLenum shaderType, qboolean optimize)
{
	char            filename[MAX_QPATH];
	GLcharARB      *mainBuffer = NULL;
	int             mainSize;
	GLint           compiled;
	GLhandleARB     shader;
	char           *token;

	int				libsSize;
	char           *libsBuffer;		// all libs concatenated

	char          **libs = (char **) &_libs;
	char          **compileMacros = (char **) &_compileMacros;

	GL_CheckErrors();

	// load libs
	libsSize = 0;
	libsBuffer = NULL;
	while(1)
	{
		int				libSize;
		char           *libBuffer;		// single extra lib file

		token = COM_ParseExt2(libs, qfalse);
		if(!token[0])
		{
			break;
		}

		if(shaderType == GL_VERTEX_SHADER_ARB)
		{
			Com_sprintf(filename, sizeof(filename), "glsl/%s_vp.glsl", token);
			ri.Printf(PRINT_ALL, "...loading vertex shader '%s'\n", filename);
		}
		else
		{
			Com_sprintf(filename, sizeof(filename), "glsl/%s_fp.glsl", token);
		}
	
		libSize = ri.FS_ReadFile(filename, (void **)&libBuffer);
		if(!libBuffer)
		{
			ri.Error(ERR_DROP, "Couldn't load %s", filename);
		}

		// append it to the libsBuffer
		libsBuffer = (char* ) realloc(libsBuffer, libsSize + libSize);
		
		memset(libsBuffer + libsSize, 0, libSize);
		libsSize += libSize;

		Q_strcat(libsBuffer, libsSize, libBuffer);
		//Q_strncpyz(libsBuffer + libsSize, libBuffer, libSize -1);

		

		ri.FS_FreeFile(libBuffer);
	}

	// load main() program
	if(shaderType == GL_VERTEX_SHADER_ARB)
	{
		Com_sprintf(filename, sizeof(filename), "glsl/%s_vp.glsl", name);
		ri.Printf(PRINT_ALL, "...loading vertex main() shader '%s'\n", filename);
	}
	else
	{
		Com_sprintf(filename, sizeof(filename), "glsl/%s_fp.glsl", name);
		ri.Printf(PRINT_ALL, "...loading fragment main() shader '%s'\n", filename);
	}

	mainSize = ri.FS_ReadFile(filename, (void **)&mainBuffer);
	if(!mainBuffer)
	{
		ri.Error(ERR_DROP, "Couldn't load %s", filename);
	}

	shader = glCreateShaderObjectARB(shaderType);

	GL_CheckErrors();

	{
		static char     bufferExtra[32000];
		int             sizeExtra;

		char           *bufferFinal = NULL;
		int             sizeFinal;

		float           fbufWidthScale, fbufHeightScale;
		float           npotWidthScale, npotHeightScale;

		Com_Memset(bufferExtra, 0, sizeof(bufferExtra));

		// HACK: abuse the GLSL preprocessor to turn GLSL 1.20 shaders into 1.30 ones
		if(glConfig.driverType == GLDRV_OPENGL3)
		{
			Q_strcat(bufferExtra, sizeof(bufferExtra), "#version 130\n");

			if(shaderType == GL_VERTEX_SHADER_ARB)
			{
				Q_strcat(bufferExtra, sizeof(bufferExtra), "#define attribute in\n");
				Q_strcat(bufferExtra, sizeof(bufferExtra), "#define varying out\n");
			}
			else
			{
				Q_strcat(bufferExtra, sizeof(bufferExtra), "#define varying in\n");

				Q_strcat(bufferExtra, sizeof(bufferExtra), "out vec4 out_Color;\n");
				Q_strcat(bufferExtra, sizeof(bufferExtra), "#define gl_FragColor out_Color\n");
			}

			Q_strcat(bufferExtra, sizeof(bufferExtra), "#define textureCube texture\n");
		}
		else
		{
			Q_strcat(bufferExtra, sizeof(bufferExtra), "#version 120\n");
		}


		while(1)
		{
			token = COM_ParseExt2(compileMacros, qfalse);
			if(!token[0])
			{
				break;
			}

			Q_strcat(bufferExtra, sizeof(bufferExtra), va("#ifndef %s\n#define %s 1\n#endif\n", token, token));
		}

#if defined(COMPAT_ET) || defined(COMPAT_ET)
		Q_strcat(bufferExtra, sizeof(bufferExtra), "#ifndef COMPAT_Q3A\n#define COMPAT_Q3A 1\n#endif\n");
#endif

		// HACK: add some macros to avoid extra uniforms and save speed and code maintenance
		Q_strcat(bufferExtra, sizeof(bufferExtra),
				 va("#ifndef r_SpecularExponent\n#define r_SpecularExponent %f\n#endif\n", r_specularExponent->value));
		Q_strcat(bufferExtra, sizeof(bufferExtra),
				 va("#ifndef r_SpecularScale\n#define r_SpecularScale %f\n#endif\n", r_specularScale->value));
		//Q_strcat(bufferExtra, sizeof(bufferExtra),
		//       va("#ifndef r_NormalScale\n#define r_NormalScale %f\n#endif\n", r_normalScale->value));


		Q_strcat(bufferExtra, sizeof(bufferExtra), "#ifndef M_PI\n#define M_PI 3.14159265358979323846f\n#endif\n");

		Q_strcat(bufferExtra, sizeof(bufferExtra), va("#ifndef MAX_SHADOWMAPS\n#define MAX_SHADOWMAPS %i\n#endif\n", MAX_SHADOWMAPS));

		Q_strcat(bufferExtra, sizeof(bufferExtra),
						 va("#ifndef deformGen_t\n"
							"#define deformGen_t\n"
							"#define DGEN_WAVE_SIN %i\n"
							"#define DGEN_WAVE_SQUARE %i\n"
							"#define DGEN_WAVE_TRIANGLE %i\n"
							"#define DGEN_WAVE_SAWTOOTH %i\n"
							"#define DGEN_WAVE_INVERSE_SAWTOOTH %i\n"
							"#define DGEN_BULGE %i\n"
							"#define DGEN_MOVE %i\n"
							"#endif\n",
							DGEN_WAVE_SIN,
							DGEN_WAVE_SQUARE,
							DGEN_WAVE_TRIANGLE,
							DGEN_WAVE_SAWTOOTH,
							DGEN_WAVE_INVERSE_SAWTOOTH,
							DGEN_BULGE,
							DGEN_MOVE));

		Q_strcat(bufferExtra, sizeof(bufferExtra),
						 va("#ifndef colorGen_t\n"
							"#define colorGen_t\n"
							"#define CGEN_VERTEX %i\n"
							"#define CGEN_ONE_MINUS_VERTEX %i\n"
							"#endif\n",
							CGEN_VERTEX,
							CGEN_ONE_MINUS_VERTEX));

		Q_strcat(bufferExtra, sizeof(bufferExtra),
								 va("#ifndef alphaGen_t\n"
									"#define alphaGen_t\n"
									"#define AGEN_VERTEX %i\n"
									"#define AGEN_ONE_MINUS_VERTEX %i\n"
									"#endif\n",
									AGEN_VERTEX,
									AGEN_ONE_MINUS_VERTEX));

		Q_strcat(bufferExtra, sizeof(bufferExtra),
								 va("#ifndef alphaTest_t\n"
									"#define alphaTest_t\n"
									"#define ATEST_GT_0 %i\n"
									"#define ATEST_LT_128 %i\n"
									"#define ATEST_GE_128 %i\n"
									"#endif\n",
									ATEST_GT_0,
									ATEST_LT_128,
									ATEST_GE_128));

		fbufWidthScale = Q_recip((float)glConfig.vidWidth);
		fbufHeightScale = Q_recip((float)glConfig.vidHeight);
		Q_strcat(bufferExtra, sizeof(bufferExtra),
				 va("#ifndef r_FBufScale\n#define r_FBufScale vec2(%f, %f)\n#endif\n", fbufWidthScale, fbufHeightScale));

		if(glConfig2.textureNPOTAvailable)
		{
			npotWidthScale = 1;
			npotHeightScale = 1;
		}
		else
		{
			npotWidthScale = (float)glConfig.vidWidth / (float)NearestPowerOfTwo(glConfig.vidWidth);
			npotHeightScale = (float)glConfig.vidHeight / (float)NearestPowerOfTwo(glConfig.vidHeight);
		}
		Q_strcat(bufferExtra, sizeof(bufferExtra),
				 va("#ifndef r_NPOTScale\n#define r_NPOTScale vec2(%f, %f)\n#endif\n", npotWidthScale, npotHeightScale));

		if(glConfig.driverType == GLDRV_MESA)
		{
			Q_strcat(bufferExtra, sizeof(bufferExtra), "#ifndef MESA\n#define MESA 1\n#endif\n");
		}

		if(glConfig.hardwareType == GLHW_ATI)
		{
			Q_strcat(bufferExtra, sizeof(bufferExtra), "#ifndef GLHW_ATI\n#define GLHW_ATI 1\n#endif\n");
		}
		else if(glConfig.hardwareType == GLHW_ATI_DX10)
		{
			Q_strcat(bufferExtra, sizeof(bufferExtra), "#ifndef GLHW_ATI_DX10\n#define GLHW_ATI_DX10 1\n#endif\n");
		}
		else if(glConfig.hardwareType == GLHW_NV_DX10)
		{
			Q_strcat(bufferExtra, sizeof(bufferExtra), "#ifndef GLHW_NV_DX10\n#define GLHW_NV_DX10 1\n#endif\n");
		}

		if(r_shadows->integer >= SHADOWING_VSM16 && glConfig2.textureFloatAvailable && glConfig2.framebufferObjectAvailable)
		{
			if(r_shadows->integer == SHADOWING_ESM)
			{
				Q_strcat(bufferExtra, sizeof(bufferExtra), "#ifndef ESM\n#define ESM 1\n#endif\n");

				if(r_debugShadowMaps->integer)
				{
					Q_strcat(bufferExtra, sizeof(bufferExtra),
							 va("#ifndef DEBUG_ESM\n#define DEBUG_ESM %i\n#endif\n", r_debugShadowMaps->integer));
				}

				if(r_lightBleedReduction->value)
				{
					Q_strcat(bufferExtra, sizeof(bufferExtra),
							 va("#ifndef r_LightBleedReduction\n#define r_LightBleedReduction %f\n#endif\n",
								r_lightBleedReduction->value));
				}

				if(r_overDarkeningFactor->value)
				{
					Q_strcat(bufferExtra, sizeof(bufferExtra),
							 va("#ifndef r_OverDarkeningFactor\n#define r_OverDarkeningFactor %f\n#endif\n",
								r_overDarkeningFactor->value));
				}

				if(r_shadowMapDepthScale->value)
				{
					Q_strcat(bufferExtra, sizeof(bufferExtra),
							 va("#ifndef r_ShadowMapDepthScale\n#define r_ShadowMapDepthScale %f\n#endif\n",
								r_shadowMapDepthScale->value));
				}
			}
			else
			{
				Q_strcat(bufferExtra, sizeof(bufferExtra), "#ifndef VSM\n#define VSM 1\n#endif\n");

				if(glConfig.hardwareType == GLHW_ATI)
				{
					Q_strcat(bufferExtra, sizeof(bufferExtra), "#ifndef VSM_CLAMP\n#define VSM_CLAMP 1\n#endif\n");
				}

				if((glConfig.hardwareType == GLHW_NV_DX10 || glConfig.hardwareType == GLHW_ATI_DX10) && r_shadows->integer == SHADOWING_VSM32)
				{
					Q_strcat(bufferExtra, sizeof(bufferExtra), "#ifndef VSM_EPSILON\n#define VSM_EPSILON 0.000001\n#endif\n");
				}
				else
				{
					Q_strcat(bufferExtra, sizeof(bufferExtra), "#ifndef VSM_EPSILON\n#define VSM_EPSILON 0.0001\n#endif\n");
				}

				if(r_debugShadowMaps->integer)
				{
					Q_strcat(bufferExtra, sizeof(bufferExtra),
							 va("#ifndef DEBUG_VSM\n#define DEBUG_VSM %i\n#endif\n", r_debugShadowMaps->integer));
				}

				if(r_lightBleedReduction->value)
				{
					Q_strcat(bufferExtra, sizeof(bufferExtra),
							 va("#ifndef r_LightBleedReduction\n#define r_LightBleedReduction %f\n#endif\n",
								r_lightBleedReduction->value));
				}
			}

			if(r_softShadows->integer == 1)
			{
				Q_strcat(bufferExtra, sizeof(bufferExtra), "#ifndef PCF_2X2\n#define PCF_2X2 1\n#endif\n");
			}
			else if(r_softShadows->integer == 2)
			{
				Q_strcat(bufferExtra, sizeof(bufferExtra), "#ifndef PCF_3X3\n#define PCF_3X3 1\n#endif\n");
			}
			else if(r_softShadows->integer == 3)
			{
				Q_strcat(bufferExtra, sizeof(bufferExtra), "#ifndef PCF_4X4\n#define PCF_4X4 1\n#endif\n");
			}
			else if(r_softShadows->integer == 4)
			{
				Q_strcat(bufferExtra, sizeof(bufferExtra), "#ifndef PCF_5X5\n#define PCF_5X5 1\n#endif\n");
			}
			else if(r_softShadows->integer == 5)
			{
				Q_strcat(bufferExtra, sizeof(bufferExtra), "#ifndef PCF_6X6\n#define PCF_6X6 1\n#endif\n");
			}
			else if(r_softShadows->integer == 6)
			{
				Q_strcat(bufferExtra, sizeof(bufferExtra), "#ifndef PCSS\n#define PCSS 1\n#endif\n");
			}

			if(r_parallelShadowSplits->integer)
			{
				Q_strcat(bufferExtra, sizeof(bufferExtra),
						 va("#ifndef r_ParallelShadowSplits_%i\n#define r_ParallelShadowSplits_%i\n#endif\n", r_parallelShadowSplits->integer, r_parallelShadowSplits->integer));
			}

			if(r_showParallelShadowSplits->integer)
			{
				Q_strcat(bufferExtra, sizeof(bufferExtra), "#ifndef r_ShowParallelShadowSplits\n#define r_ShowParallelShadowSplits 1\n#endif\n");
			}
		}

		if(r_deferredShading->integer && glConfig2.maxColorAttachments >= 4 && glConfig2.textureFloatAvailable &&
		   glConfig2.drawBuffersAvailable && glConfig2.maxDrawBuffers >= 4)
		{

			if(r_deferredShading->integer == DS_PREPASS_LIGHTING)
			{
				Q_strcat(bufferExtra, sizeof(bufferExtra), "#ifndef r_DeferredLighting\n#define r_DeferredLighting 1\n#endif\n");
			}

			/*
			if(glConfig2.framebufferMixedFormatsAvailable)
			{
				Q_strcat(bufferExtra, sizeof(bufferExtra),
						 "#ifndef GL_EXTX_framebuffer_mixed_formats\n#define GL_EXTX_framebuffer_mixed_formats 1\n#endif\n");
			}
			*/
		}

		if(r_hdrRendering->integer && glConfig2.framebufferObjectAvailable && glConfig2.textureFloatAvailable)
		{
			Q_strcat(bufferExtra, sizeof(bufferExtra), "#ifndef r_HDRRendering\n#define r_HDRRendering 1\n#endif\n");

			Q_strcat(bufferExtra, sizeof(bufferExtra),
					 va("#ifndef r_HDRContrastThreshold\n#define r_HDRContrastThreshold %f\n#endif\n",
						r_hdrContrastThreshold->value));

			Q_strcat(bufferExtra, sizeof(bufferExtra),
					 va("#ifndef r_HDRContrastOffset\n#define r_HDRContrastOffset %f\n#endif\n",
						r_hdrContrastOffset->value));

			Q_strcat(bufferExtra, sizeof(bufferExtra),
					 va("#ifndef r_HDRToneMappingOperator\n#define r_HDRToneMappingOperator_%i\n#endif\n",
						r_hdrToneMappingOperator->integer));

			Q_strcat(bufferExtra, sizeof(bufferExtra),
					 va("#ifndef r_HDRGamma\n#define r_HDRGamma %f\n#endif\n",
						r_hdrGamma->value));
		}

		if(r_precomputedLighting->integer)
		{
			Q_strcat(bufferExtra, sizeof(bufferExtra),
					 "#ifndef r_precomputedLighting\n#define r_precomputedLighting 1\n#endif\n");
		}

		if(r_heatHazeFix->integer && glConfig2.framebufferBlitAvailable && glConfig.hardwareType != GLHW_ATI && glConfig.hardwareType != GLHW_ATI_DX10 && glConfig.driverType != GLDRV_MESA)
		{
			Q_strcat(bufferExtra, sizeof(bufferExtra), "#ifndef r_heatHazeFix\n#define r_heatHazeFix 1\n#endif\n");
		}

		if(r_showLightMaps->integer)
		{
			Q_strcat(bufferExtra, sizeof(bufferExtra), "#ifndef r_showLightMaps\n#define r_showLightMaps 1\n#endif\n");
		}

		if(r_showDeluxeMaps->integer)
		{
			Q_strcat(bufferExtra, sizeof(bufferExtra), "#ifndef r_showDeluxeMaps\n#define r_showDeluxeMaps 1\n#endif\n");
		}
#ifdef EXPERIMENTAL
		if(r_screenSpaceAmbientOcclusion->integer)
		{
			int             i;
			static vec3_t   jitter[32];
			static qboolean jitterInit = qfalse;

			if(!jitterInit)
			{
				for(i = 0; i < 32; i++)
				{
					float          *jit = &jitter[i][0];

					float           rad = crandom() * 1024.0f;	// FIXME radius;
					float           a = crandom() * M_PI * 2;
					float           b = crandom() * M_PI * 2;

					jit[0] = rad * sin(a) * cos(b);
					jit[1] = rad * sin(a) * sin(b);
					jit[2] = rad * cos(a);
				}

				jitterInit = qtrue;
			}

			// TODO
		}
#endif

		if(glConfig2.vboVertexSkinningAvailable)
		{
			Q_strcat(bufferExtra, sizeof(bufferExtra), "#ifndef r_VertexSkinning\n#define r_VertexSkinning 1\n#endif\n");

			Q_strcat(bufferExtra, sizeof(bufferExtra),
								 va("#ifndef MAX_GLSL_BONES\n#define MAX_GLSL_BONES %i\n#endif\n", glConfig2.maxVertexSkinningBones));
		}

		/*
		   if(glConfig2.drawBuffersAvailable && glConfig2.maxDrawBuffers >= 4)
		   {
		   //Q_strcat(bufferExtra, sizeof(bufferExtra), "#ifndef GL_ARB_draw_buffers\n#define GL_ARB_draw_buffers 1\n#endif\n");
		   Q_strcat(bufferExtra, sizeof(bufferExtra), "#extension GL_ARB_draw_buffers : enable\n");
		   }
		 */

		if(r_normalMapping->integer)
		{
			Q_strcat(bufferExtra, sizeof(bufferExtra), "#ifndef r_NormalMapping\n#define r_NormalMapping 1\n#endif\n");
		}

		if( /* TODO: check for shader model 3 hardware  && */ r_normalMapping->integer && r_parallaxMapping->integer)
		{
			Q_strcat(bufferExtra, sizeof(bufferExtra), "#ifndef r_ParallaxMapping\n#define r_ParallaxMapping 1\n#endif\n");
		}

		if(r_wrapAroundLighting->value)
		{
			Q_strcat(bufferExtra, sizeof(bufferExtra),
							 va("#ifndef r_WrapAroundLighting\n#define r_WrapAroundLighting %f\n#endif\n",
									 r_wrapAroundLighting->value));
		}

		if(r_halfLambertLighting->integer)
		{
			Q_strcat(bufferExtra, sizeof(bufferExtra), "#ifndef r_halfLambertLighting\n#define r_halfLambertLighting 1\n#endif\n");
		}

		/*
		   if(glConfig2.textureFloatAvailable)
		   {
		   Q_strcat(bufferExtra, sizeof(bufferExtra), "#ifndef GL_ARB_texture_float\n#define GL_ARB_texture_float 1\n#endif\n");
		   }
		 */


		// OK we added a lot of stuff but if we do something bad in the GLSL shaders then we want the proper line
		// so we have to reset the line counting
		Q_strcat(bufferExtra, sizeof(bufferExtra), "#line 0\n");

		sizeExtra = strlen(bufferExtra);
		sizeFinal = sizeExtra + mainSize + libsSize;

		//ri.Printf(PRINT_ALL, "GLSL extra: %s\n", bufferExtra);

		bufferFinal = (char *) ri.Hunk_AllocateTempMemory(sizeFinal);

		strcpy(bufferFinal, bufferExtra);

		if(libsSize > 0)
		{
			Q_strcat(bufferFinal, sizeFinal, libsBuffer);
		}

		Q_strcat(bufferFinal, sizeFinal, mainBuffer);

#if 0
		{
			static char     msgPart[1024];
			int             i;
			ri.Printf(PRINT_WARNING, "----------------------------------------------------------\n", filename);
			ri.Printf(PRINT_WARNING, "CONCATENATED shader '%s' ----------\n", filename);
			ri.Printf(PRINT_WARNING, " BEGIN ---------------------------------------------------\n", filename);

			for(i = 0; i < sizeFinal; i += 1024)
			{
				Q_strncpyz(msgPart, bufferFinal + i, sizeof(msgPart));
				ri.Printf(PRINT_ALL, "%s", msgPart);
			}

			ri.Printf(PRINT_WARNING, " END-- ---------------------------------------------------\n", filename);
		}
#endif

#if defined(USE_GLSL_OPTIMIZER)
		if(glConfig.driverType != GLDRV_OPENGL3 && optimize)
		{
			static char     msgPart[1024];
			int             length = 0;
			int             i;
			

			glslopt_shader_type glsloptShaderType;

			if(shaderType == GL_FRAGMENT_SHADER_ARB)
				glsloptShaderType = kGlslOptShaderFragment;
			else
				glsloptShaderType = kGlslOptShaderVertex;

			glslopt_shader* shaderOptimized = glslopt_optimize(s_glslOptimizer, 
				glsloptShaderType, bufferFinal, 0);

			if(glslopt_get_status(shaderOptimized))
			{
				const char* newSource = glslopt_get_output(shaderOptimized);

				ri.Printf(PRINT_WARNING, "----------------------------------------------------------\n", filename);
				ri.Printf(PRINT_WARNING, "OPTIMIZED shader '%s' ----------\n", filename);
				ri.Printf(PRINT_WARNING, " BEGIN ---------------------------------------------------\n", filename);

				length = strlen(newSource);
				for(i = 0; i < length; i += 1024)
				{
					Q_strncpyz(msgPart, newSource + i, sizeof(msgPart));
					ri.Printf(PRINT_ALL, "%s\n", msgPart);
				}

				ri.Printf(PRINT_WARNING, " END-- ---------------------------------------------------\n", filename);

				glShaderSourceARB(shader, 1, (const GLcharARB **)&newSource, &length);
			}
			else
			{
				const char* errorLog = glslopt_get_log(shaderOptimized);

				//ri.Printf(PRINT_WARNING, "Couldn't optimize '%s'", filename);

				length = strlen(errorLog);
				for(i = 0; i < length; i += 1024)
				{
					Q_strncpyz(msgPart, errorLog + i, sizeof(msgPart));
					ri.Printf(PRINT_ALL, "%s\n", msgPart);
				}

				ri.Error(ERR_FATAL, "Couldn't optimize %s", filename);
			}
			
			glslopt_shader_delete(shaderOptimized);
		}
		else
		{
			glShaderSourceARB(shader, 1, (const GLcharARB **)&bufferFinal, &sizeFinal);
		}
#else
		glShaderSourceARB(shader, 1, (const GLcharARB **)&bufferFinal, &sizeFinal);
#endif


		ri.Hunk_FreeTempMemory(bufferFinal);
	}

	// compile shader
	glCompileShaderARB(shader);

	GL_CheckErrors();

	// check if shader compiled
	glGetObjectParameterivARB(shader, GL_OBJECT_COMPILE_STATUS_ARB, &compiled);
	if(!compiled)
	{
		GLSL_PrintShaderSource(shader);
		GLSL_PrintInfoLog(shader, qfalse);
		ri.Error(ERR_DROP, "Couldn't compile %s", filename);
		ri.FS_FreeFile(mainBuffer);
		free(libsBuffer);
		return;
	}

	GLSL_PrintInfoLog(shader, qtrue);
	//ri.Printf(PRINT_ALL, "%s\n", GLSL_PrintShaderSource(shader));

	// attach shader to program
	glAttachObjectARB(program, shader);
	GL_CheckErrors();

	// delete shader, no longer needed
	glDeleteObjectARB(shader);
	GL_CheckErrors();

	ri.FS_FreeFile(mainBuffer);
	free(libsBuffer);
}

static void GLSL_LinkProgram(GLhandleARB program)
{
	GLint           linked;

	glLinkProgramARB(program);

	glGetObjectParameterivARB(program, GL_OBJECT_LINK_STATUS_ARB, &linked);
	if(!linked)
	{
		GLSL_PrintInfoLog(program, qfalse);
		ri.Error(ERR_DROP, "%s\nshaders failed to link");
	}
}

void GLSL_ValidateProgram(GLhandleARB program)
{
	GLint           validated;

	glValidateProgramARB(program);

	glGetObjectParameterivARB(program, GL_OBJECT_VALIDATE_STATUS_ARB, &validated);
	if(!validated)
	{
		GLSL_PrintInfoLog(program, qfalse);
		ri.Error(ERR_DROP, "%s\nshaders failed to validate");
	}
}

void GLSL_ShowProgramUniforms(GLhandleARB program)
{
	int             i, count, size;
	GLenum			type;
	char            uniformName[1000];

	// install the executables in the program object as part of current state.
	glUseProgramObjectARB(program);

	// check for GL Errors

	// query the number of active uniforms
	glGetObjectParameterivARB(program, GL_OBJECT_ACTIVE_UNIFORMS_ARB, &count);

	// Loop over each of the active uniforms, and set their value
	for(i = 0; i < count; i++)
	{
		glGetActiveUniformARB(program, i, sizeof(uniformName), NULL, &size, &type, uniformName);

		ri.Printf(PRINT_DEVELOPER, "active uniform: '%s'\n", uniformName);
	}

	glUseProgramObjectARB(0);
}

static void GLSL_BindAttribLocations(GLhandleARB program, int attribs)
{
	if(attribs & ATTR_POSITION)
		glBindAttribLocationARB(program, ATTR_INDEX_POSITION, "attr_Position");

	if(attribs & ATTR_TEXCOORD)
		glBindAttribLocationARB(program, ATTR_INDEX_TEXCOORD0, "attr_TexCoord0");

	if(attribs & ATTR_LIGHTCOORD)
		glBindAttribLocationARB(program, ATTR_INDEX_TEXCOORD1, "attr_TexCoord1");

//  if(attribs & ATTR_TEXCOORD2)
//      glBindAttribLocationARB(program, ATTR_INDEX_TEXCOORD2, "attr_TexCoord2");

//  if(attribs & ATTR_TEXCOORD3)
//      glBindAttribLocationARB(program, ATTR_INDEX_TEXCOORD3, "attr_TexCoord3");

	if(attribs & ATTR_TANGENT)
		glBindAttribLocationARB(program, ATTR_INDEX_TANGENT, "attr_Tangent");

	if(attribs & ATTR_BINORMAL)
		glBindAttribLocationARB(program, ATTR_INDEX_BINORMAL, "attr_Binormal");

	if(attribs & ATTR_NORMAL)
		glBindAttribLocationARB(program, ATTR_INDEX_NORMAL, "attr_Normal");

	if(attribs & ATTR_COLOR)
		glBindAttribLocationARB(program, ATTR_INDEX_COLOR, "attr_Color");

	if(attribs & ATTR_PAINTCOLOR)
		glBindAttribLocationARB(program, ATTR_INDEX_PAINTCOLOR, "attr_PaintColor");

	if(attribs & ATTR_LIGHTDIRECTION)
		glBindAttribLocationARB(program, ATTR_INDEX_LIGHTDIRECTION, "attr_LightDirection");

	if(glConfig2.vboVertexSkinningAvailable)
	{
		glBindAttribLocationARB(program, ATTR_INDEX_BONE_INDEXES, "attr_BoneIndexes");
		glBindAttribLocationARB(program, ATTR_INDEX_BONE_WEIGHTS, "attr_BoneWeights");
	}

	if(attribs & ATTR_POSITION2)
		glBindAttribLocationARB(program, ATTR_INDEX_POSITION2, "attr_Position2");

	if(attribs & ATTR_TANGENT2)
		glBindAttribLocationARB(program, ATTR_INDEX_TANGENT2, "attr_Tangent2");

	if(attribs & ATTR_BINORMAL2)
		glBindAttribLocationARB(program, ATTR_INDEX_BINORMAL2, "attr_Binormal2");

	if(attribs & ATTR_NORMAL2)
		glBindAttribLocationARB(program, ATTR_INDEX_NORMAL2, "attr_Normal2");
}

static void GLSL_InitGPUShader(shaderProgram_t * program, const char *name, int attribs, qboolean fragmentShader, qboolean optimize)
{
	ri.Printf(PRINT_DEVELOPER, "------- GPU shader -------\n");

	if(strlen(name) >= MAX_QPATH)
	{
		ri.Error(ERR_DROP, "GLSL_InitGPUShader: \"%s\" is too long\n", name);
	}

	Q_strncpyz(program->name, name, sizeof(program->name));

	program->program = glCreateProgramObjectARB();
	program->attribs = attribs;

	GLSL_LoadGPUShader(program->program, name, "", "", GL_VERTEX_SHADER_ARB, optimize);
	GLSL_LoadGPUShader(program->program, name, "", "", GL_FRAGMENT_SHADER_ARB, optimize);

	GLSL_BindAttribLocations(program->program, attribs);
	GLSL_LinkProgram(program->program);
}


static void GLSL_InitGPUShader2(shaderProgram_t * program,
								const char *vertexMainShader,
								const char *fragmentMainShader,
								const char *vertexLibShaders,
								const char *fragmentLibShaders,
								int attribs,
								qboolean optimize)
{
	ri.Printf(PRINT_DEVELOPER, "------- GPU shader -------\n");

	if(strlen(vertexMainShader) >= MAX_QPATH)
	{
		ri.Error(ERR_DROP, "GLSL_InitGPUShader2: \"%s\" is too long\n", vertexMainShader);
	}

	if(strlen(fragmentMainShader) >= MAX_QPATH)
	{
		ri.Error(ERR_DROP, "GLSL_InitGPUShader2: \"%s\" is too long\n", fragmentMainShader);
	}

	Q_strncpyz(program->name, fragmentMainShader, sizeof(program->name));

	program->program = glCreateProgramObjectARB();
	program->attribs = attribs;

	GLSL_LoadGPUShader(program->program, vertexMainShader, vertexLibShaders, "", GL_VERTEX_SHADER_ARB, optimize);
	GLSL_LoadGPUShader(program->program, fragmentMainShader, fragmentLibShaders, "", GL_FRAGMENT_SHADER_ARB, optimize);
	
	GLSL_BindAttribLocations(program->program, attribs);
	GLSL_LinkProgram(program->program);
}

void GLSL_InitGPUShader3(shaderProgram_t * program,
								const char *vertexMainShader,
								const char *fragmentMainShader,
								const char *vertexLibShaders,
								const char *fragmentLibShaders,
								const char *compileMacros,
								int attribs,
								qboolean optimize)
{
	int			len;

	ri.Printf(PRINT_DEVELOPER, "------- GPU shader -------\n");

	if(strlen(vertexMainShader) >= MAX_QPATH)
	{
		ri.Error(ERR_DROP, "GLSL_InitGPUShader3: \"%s\" is too long\n", vertexMainShader);
	}

	if(strlen(fragmentMainShader) >= MAX_QPATH)
	{
		ri.Error(ERR_DROP, "GLSL_InitGPUShader3: \"%s\" is too long\n", fragmentMainShader);
	}

	Q_strncpyz(program->name, fragmentMainShader, sizeof(program->name));

	len = strlen(compileMacros) + 1;
	program->compileMacros = (char *) ri.Hunk_Alloc(sizeof(char) * len, h_low);
	Q_strncpyz(program->compileMacros, compileMacros, len);

	program->program = glCreateProgramObjectARB();
	program->attribs = attribs;

	GLSL_LoadGPUShader(program->program, vertexMainShader, vertexLibShaders, compileMacros, GL_VERTEX_SHADER_ARB, optimize);
	GLSL_LoadGPUShader(program->program, fragmentMainShader, fragmentLibShaders, compileMacros, GL_FRAGMENT_SHADER_ARB, optimize);

	GLSL_BindAttribLocations(program->program, attribs);
	GLSL_LinkProgram(program->program);
}








void GLSL_InitGPUShaders(void)
{
//	int				i;
	int             startTime, endTime;
	static char     compileMacros[32000];
//	shaderProgram_t *shaderProgram;

	ri.Printf(PRINT_ALL, "------- GLSL_InitGPUShaders -------\n");

	// make sure the render thread is stopped
	R_SyncRenderThread();

	GL_CheckErrors();

#if defined(USE_GLSL_OPTIMIZER)
	s_glslOptimizer = glslopt_initialize();
#endif

	startTime = ri.Milliseconds();

	// single texture rendering
	GLSL_InitGPUShader(&tr.genericShader, "generic", ATTR_POSITION | ATTR_TEXCOORD | ATTR_NORMAL | ATTR_COLOR, qtrue, qtrue);

	GL_CheckErrors();

	tr.genericShader.u_ColorMap = glGetUniformLocationARB(tr.genericShader.program, "u_ColorMap");
	tr.genericShader.u_ColorTextureMatrix =
		glGetUniformLocationARB(tr.genericShader.program, "u_ColorTextureMatrix");
	tr.genericShader.u_ColorGen = glGetUniformLocationARB(tr.genericShader.program, "u_ColorGen");
	tr.genericShader.u_AlphaGen = glGetUniformLocationARB(tr.genericShader.program, "u_AlphaGen");
	tr.genericShader.u_Color = glGetUniformLocationARB(tr.genericShader.program, "u_Color");
	tr.genericShader.u_AlphaTest = glGetUniformLocationARB(tr.genericShader.program, "u_AlphaTest");
	tr.genericShader.u_ViewOrigin = glGetUniformLocationARB(tr.genericShader.program, "u_ViewOrigin");
	tr.genericShader.u_TCGen_Environment = glGetUniformLocationARB(tr.genericShader.program, "u_TCGen_Environment");
	tr.genericShader.u_DeformGen = glGetUniformLocationARB(tr.genericShader.program, "u_DeformGen");
	tr.genericShader.u_DeformWave = glGetUniformLocationARB(tr.genericShader.program, "u_DeformWave");
	tr.genericShader.u_DeformBulge = glGetUniformLocationARB(tr.genericShader.program, "u_DeformBulge");
	tr.genericShader.u_DeformSpread = glGetUniformLocationARB(tr.genericShader.program, "u_DeformSpread");
	tr.genericShader.u_Time = glGetUniformLocationARB(tr.genericShader.program, "u_Time");
	tr.genericShader.u_PortalClipping = glGetUniformLocationARB(tr.genericShader.program, "u_PortalClipping");
	tr.genericShader.u_PortalPlane = glGetUniformLocationARB(tr.genericShader.program, "u_PortalPlane");
	tr.genericShader.u_ModelMatrix = glGetUniformLocationARB(tr.genericShader.program, "u_ModelMatrix");
	/*
	   tr.genericShader.u_ModelViewMatrix =
	   glGetUniformLocationARB(tr.genericShader.program, "u_ModelViewMatrix");
	   tr.genericShader.u_ProjectionMatrix =
	   glGetUniformLocationARB(tr.genericShader.program, "u_ProjectionMatrix");
	 */
	tr.genericShader.u_ModelViewProjectionMatrix =
		glGetUniformLocationARB(tr.genericShader.program, "u_ModelViewProjectionMatrix");
	if(glConfig2.vboVertexSkinningAvailable)
	{
		tr.genericShader.u_VertexSkinning = glGetUniformLocationARB(tr.genericShader.program, "u_VertexSkinning");
		tr.genericShader.u_BoneMatrix = glGetUniformLocationARB(tr.genericShader.program, "u_BoneMatrix");
	}

	glUseProgramObjectARB(tr.genericShader.program);
	glUniform1iARB(tr.genericShader.u_ColorMap, 0);
	glUseProgramObjectARB(0);

	GLSL_ValidateProgram(tr.genericShader.program);
	GLSL_ShowProgramUniforms(tr.genericShader.program);
	GL_CheckErrors();

	// simple vertex color shading for entities
	gl_vertexLightingShader_DBS_entity = new GLShader_vertexLighting_DBS_entity();

	// simple vertex color shading for the world
	GLSL_InitGPUShader(&tr.vertexLightingShader_DBS_world,
					   "vertexLighting_DBS_world",
					   ATTR_POSITION | ATTR_TEXCOORD | ATTR_TANGENT | ATTR_BINORMAL | ATTR_NORMAL | ATTR_COLOR | ATTR_LIGHTDIRECTION, qtrue, qtrue);

	tr.vertexLightingShader_DBS_world.u_DiffuseMap =
		glGetUniformLocationARB(tr.vertexLightingShader_DBS_world.program, "u_DiffuseMap");
	tr.vertexLightingShader_DBS_world.u_NormalMap =
		glGetUniformLocationARB(tr.vertexLightingShader_DBS_world.program, "u_NormalMap");
	tr.vertexLightingShader_DBS_world.u_SpecularMap =
		glGetUniformLocationARB(tr.vertexLightingShader_DBS_world.program, "u_SpecularMap");
	tr.vertexLightingShader_DBS_world.u_DiffuseTextureMatrix =
		glGetUniformLocationARB(tr.vertexLightingShader_DBS_world.program, "u_DiffuseTextureMatrix");
	tr.vertexLightingShader_DBS_world.u_NormalTextureMatrix =
		glGetUniformLocationARB(tr.vertexLightingShader_DBS_world.program, "u_NormalTextureMatrix");
	tr.vertexLightingShader_DBS_world.u_SpecularTextureMatrix =
		glGetUniformLocationARB(tr.vertexLightingShader_DBS_world.program, "u_SpecularTextureMatrix");
	tr.vertexLightingShader_DBS_world.u_AlphaTest =
		glGetUniformLocationARB(tr.vertexLightingShader_DBS_world.program, "u_AlphaTest");
	tr.vertexLightingShader_DBS_world.u_DeformGen =
		glGetUniformLocationARB(tr.vertexLightingShader_DBS_world.program, "u_DeformGen");
	tr.vertexLightingShader_DBS_world.u_DeformWave =
		glGetUniformLocationARB(tr.vertexLightingShader_DBS_world.program, "u_DeformWave");
	tr.vertexLightingShader_DBS_world.u_DeformBulge =
		glGetUniformLocationARB(tr.vertexLightingShader_DBS_world.program, "u_DeformBulge");
	tr.vertexLightingShader_DBS_world.u_DeformSpread =
		glGetUniformLocationARB(tr.vertexLightingShader_DBS_world.program, "u_DeformSpread");
	tr.vertexLightingShader_DBS_world.u_ColorGen =
		glGetUniformLocationARB(tr.vertexLightingShader_DBS_world.program, "u_ColorGen");
	tr.vertexLightingShader_DBS_world.u_AlphaGen =
		glGetUniformLocationARB(tr.vertexLightingShader_DBS_world.program, "u_AlphaGen");
	tr.vertexLightingShader_DBS_world.u_Color =
		glGetUniformLocationARB(tr.vertexLightingShader_DBS_world.program, "u_Color");
	tr.vertexLightingShader_DBS_world.u_ViewOrigin =
		glGetUniformLocationARB(tr.vertexLightingShader_DBS_world.program, "u_ViewOrigin");
	tr.vertexLightingShader_DBS_world.u_ParallaxMapping =
		glGetUniformLocationARB(tr.vertexLightingShader_DBS_world.program, "u_ParallaxMapping");
	tr.vertexLightingShader_DBS_world.u_DepthScale =
		glGetUniformLocationARB(tr.vertexLightingShader_DBS_world.program, "u_DepthScale");
	tr.vertexLightingShader_DBS_world.u_PortalClipping =
		glGetUniformLocationARB(tr.vertexLightingShader_DBS_world.program, "u_PortalClipping");
	tr.vertexLightingShader_DBS_world.u_PortalPlane =
		glGetUniformLocationARB(tr.vertexLightingShader_DBS_world.program, "u_PortalPlane");
	tr.vertexLightingShader_DBS_world.u_ModelViewProjectionMatrix =
		glGetUniformLocationARB(tr.vertexLightingShader_DBS_world.program, "u_ModelViewProjectionMatrix");
	tr.vertexLightingShader_DBS_world.u_Time =
		glGetUniformLocationARB(tr.vertexLightingShader_DBS_world.program, "u_Time");
	tr.vertexLightingShader_DBS_world.u_LightWrapAround =
		glGetUniformLocationARB(tr.vertexLightingShader_DBS_world.program, "u_LightWrapAround");

	glUseProgramObjectARB(tr.vertexLightingShader_DBS_world.program);
	glUniform1iARB(tr.vertexLightingShader_DBS_world.u_DiffuseMap, 0);
	glUniform1iARB(tr.vertexLightingShader_DBS_world.u_NormalMap, 1);
	glUniform1iARB(tr.vertexLightingShader_DBS_world.u_SpecularMap, 2);
	glUseProgramObjectARB(0);

	GLSL_ValidateProgram(tr.vertexLightingShader_DBS_world.program);
	GLSL_ShowProgramUniforms(tr.vertexLightingShader_DBS_world.program);
	GL_CheckErrors();

	// standard light mapping
	gl_lightMappingShader = new GLShader_lightMapping();
	/*
	GLSL_InitGPUShader(&tr.lightMappingShader,
					   "lightMapping", ATTR_POSITION | ATTR_TEXCOORD | ATTR_LIGHTCOORD | ATTR_NORMAL, qtrue, qtrue);

	tr.lightMappingShader.u_ModelViewProjectionMatrix =
		glGetUniformLocationARB(tr.lightMappingShader.program, "u_ModelViewProjectionMatrix");

	tr.lightMappingShader.u_DiffuseMap = glGetUniformLocationARB(tr.lightMappingShader.program, "u_DiffuseMap");
	tr.lightMappingShader.u_LightMap = glGetUniformLocationARB(tr.lightMappingShader.program, "u_LightMap");
	tr.lightMappingShader.u_DiffuseTextureMatrix =
		glGetUniformLocationARB(tr.lightMappingShader.program, "u_DiffuseTextureMatrix");
	tr.lightMappingShader.u_AlphaTest = glGetUniformLocationARB(tr.lightMappingShader.program, "u_AlphaTest");
	tr.lightMappingShader.u_DeformGen = glGetUniformLocationARB(tr.lightMappingShader.program, "u_DeformGen");
	tr.lightMappingShader.u_DeformWave = glGetUniformLocationARB(tr.lightMappingShader.program, "u_DeformWave");
	tr.lightMappingShader.u_DeformBulge = glGetUniformLocationARB(tr.lightMappingShader.program, "u_DeformBulge");
	tr.lightMappingShader.u_DeformSpread = glGetUniformLocationARB(tr.lightMappingShader.program, "u_DeformSpread");
	tr.lightMappingShader.u_Time = glGetUniformLocationARB(tr.lightMappingShader.program, "u_Time");

	glUseProgramObjectARB(tr.lightMappingShader.program);
	glUniform1iARB(tr.lightMappingShader.u_DiffuseMap, 0);
	glUniform1iARB(tr.lightMappingShader.u_LightMap, 1);
	glUseProgramObjectARB(0);

	GLSL_ValidateProgram(tr.lightMappingShader.program);
	GLSL_ShowProgramUniforms(tr.lightMappingShader.program);
	GL_CheckErrors();
	*/

	// directional light mapping aka deluxe mapping
	if(r_normalMapping->integer)
	{
		GLSL_InitGPUShader(&tr.deluxeMappingShader,
						   "deluxeMapping",
						   ATTR_POSITION | ATTR_TEXCOORD | ATTR_LIGHTCOORD | ATTR_TANGENT | ATTR_BINORMAL | ATTR_NORMAL, qtrue, qtrue);

		tr.deluxeMappingShader.u_DiffuseMap = glGetUniformLocationARB(tr.deluxeMappingShader.program, "u_DiffuseMap");
		tr.deluxeMappingShader.u_NormalMap = glGetUniformLocationARB(tr.deluxeMappingShader.program, "u_NormalMap");
		tr.deluxeMappingShader.u_SpecularMap = glGetUniformLocationARB(tr.deluxeMappingShader.program, "u_SpecularMap");
		tr.deluxeMappingShader.u_LightMap = glGetUniformLocationARB(tr.deluxeMappingShader.program, "u_LightMap");
		tr.deluxeMappingShader.u_DeluxeMap = glGetUniformLocationARB(tr.deluxeMappingShader.program, "u_DeluxeMap");
		tr.deluxeMappingShader.u_DiffuseTextureMatrix =
			glGetUniformLocationARB(tr.deluxeMappingShader.program, "u_DiffuseTextureMatrix");
		tr.deluxeMappingShader.u_NormalTextureMatrix =
			glGetUniformLocationARB(tr.deluxeMappingShader.program, "u_NormalTextureMatrix");
		tr.deluxeMappingShader.u_SpecularTextureMatrix =
			glGetUniformLocationARB(tr.deluxeMappingShader.program, "u_SpecularTextureMatrix");
		tr.deluxeMappingShader.u_AlphaTest = glGetUniformLocationARB(tr.deluxeMappingShader.program, "u_AlphaTest");
		tr.deluxeMappingShader.u_ViewOrigin = glGetUniformLocationARB(tr.deluxeMappingShader.program, "u_ViewOrigin");
		tr.deluxeMappingShader.u_ParallaxMapping = glGetUniformLocationARB(tr.deluxeMappingShader.program, "u_ParallaxMapping");
		tr.deluxeMappingShader.u_DepthScale = glGetUniformLocationARB(tr.deluxeMappingShader.program, "u_DepthScale");
		tr.deluxeMappingShader.u_PortalClipping = glGetUniformLocationARB(tr.deluxeMappingShader.program, "u_PortalClipping");
		tr.deluxeMappingShader.u_PortalPlane = glGetUniformLocationARB(tr.deluxeMappingShader.program, "u_PortalPlane");
		tr.deluxeMappingShader.u_ModelMatrix = glGetUniformLocationARB(tr.deluxeMappingShader.program, "u_ModelMatrix");
		tr.deluxeMappingShader.u_ModelViewProjectionMatrix =
			glGetUniformLocationARB(tr.deluxeMappingShader.program, "u_ModelViewProjectionMatrix");
		tr.deluxeMappingShader.u_DeformGen = glGetUniformLocationARB(tr.deluxeMappingShader.program, "u_DeformGen");
		tr.deluxeMappingShader.u_DeformWave = glGetUniformLocationARB(tr.deluxeMappingShader.program, "u_DeformWave");
		tr.deluxeMappingShader.u_DeformBulge = glGetUniformLocationARB(tr.deluxeMappingShader.program, "u_DeformBulge");
		tr.deluxeMappingShader.u_DeformSpread = glGetUniformLocationARB(tr.deluxeMappingShader.program, "u_DeformSpread");
		tr.deluxeMappingShader.u_Time = glGetUniformLocationARB(tr.deluxeMappingShader.program, "u_Time");

		glUseProgramObjectARB(tr.deluxeMappingShader.program);
		glUniform1iARB(tr.deluxeMappingShader.u_DiffuseMap, 0);
		glUniform1iARB(tr.deluxeMappingShader.u_NormalMap, 1);
		glUniform1iARB(tr.deluxeMappingShader.u_SpecularMap, 2);
		glUniform1iARB(tr.deluxeMappingShader.u_LightMap, 3);
		glUniform1iARB(tr.deluxeMappingShader.u_DeluxeMap, 4);
		glUseProgramObjectARB(0);

		GLSL_ValidateProgram(tr.deluxeMappingShader.program);
		GLSL_ShowProgramUniforms(tr.deluxeMappingShader.program);
		GL_CheckErrors();
	}

	// geometric-buffer fill rendering with diffuse + bump + specular
	if(DS_STANDARD_ENABLED() || DS_PREPASS_LIGHTING_ENABLED())
	{
		GLSL_InitGPUShader(&tr.geometricFillShader_DBS, "geometricFill_DBS",
						   ATTR_POSITION | ATTR_TEXCOORD | ATTR_TANGENT | ATTR_BINORMAL | ATTR_NORMAL, qtrue, qtrue);

		tr.geometricFillShader_DBS.u_DiffuseMap = glGetUniformLocationARB(tr.geometricFillShader_DBS.program, "u_DiffuseMap");
		tr.geometricFillShader_DBS.u_NormalMap = glGetUniformLocationARB(tr.geometricFillShader_DBS.program, "u_NormalMap");
		tr.geometricFillShader_DBS.u_SpecularMap = glGetUniformLocationARB(tr.geometricFillShader_DBS.program, "u_SpecularMap");
		tr.geometricFillShader_DBS.u_DiffuseTextureMatrix =
			glGetUniformLocationARB(tr.geometricFillShader_DBS.program, "u_DiffuseTextureMatrix");
		tr.geometricFillShader_DBS.u_NormalTextureMatrix =
			glGetUniformLocationARB(tr.geometricFillShader_DBS.program, "u_NormalTextureMatrix");
		tr.geometricFillShader_DBS.u_SpecularTextureMatrix =
			glGetUniformLocationARB(tr.geometricFillShader_DBS.program, "u_SpecularTextureMatrix");
		tr.geometricFillShader_DBS.u_AlphaTest = glGetUniformLocationARB(tr.geometricFillShader_DBS.program, "u_AlphaTest");
		tr.geometricFillShader_DBS.u_ViewOrigin = glGetUniformLocationARB(tr.geometricFillShader_DBS.program, "u_ViewOrigin");
		tr.geometricFillShader_DBS.u_AmbientColor =
			glGetUniformLocationARB(tr.geometricFillShader_DBS.program, "u_AmbientColor");
		tr.geometricFillShader_DBS.u_ParallaxMapping = glGetUniformLocationARB(tr.geometricFillShader_DBS.program, "u_ParallaxMapping");
		tr.geometricFillShader_DBS.u_DepthScale = glGetUniformLocationARB(tr.geometricFillShader_DBS.program, "u_DepthScale");
		tr.geometricFillShader_DBS.u_ModelMatrix = glGetUniformLocationARB(tr.geometricFillShader_DBS.program, "u_ModelMatrix");
		tr.geometricFillShader_DBS.u_ModelViewMatrix =
			glGetUniformLocationARB(tr.geometricFillShader_DBS.program, "u_ModelViewMatrix");
		tr.geometricFillShader_DBS.u_ModelViewProjectionMatrix =
			glGetUniformLocationARB(tr.geometricFillShader_DBS.program, "u_ModelViewProjectionMatrix");
		if(glConfig2.vboVertexSkinningAvailable)
		{
			tr.geometricFillShader_DBS.u_VertexSkinning =
				glGetUniformLocationARB(tr.geometricFillShader_DBS.program, "u_VertexSkinning");
			tr.geometricFillShader_DBS.u_BoneMatrix =
				glGetUniformLocationARB(tr.geometricFillShader_DBS.program, "u_BoneMatrix");
		}

		tr.geometricFillShader_DBS.u_DeformGen = glGetUniformLocationARB(tr.geometricFillShader_DBS.program, "u_DeformGen");
		tr.geometricFillShader_DBS.u_DeformWave = glGetUniformLocationARB(tr.geometricFillShader_DBS.program, "u_DeformWave");
		tr.geometricFillShader_DBS.u_DeformBulge = glGetUniformLocationARB(tr.geometricFillShader_DBS.program, "u_DeformBulge");
		tr.geometricFillShader_DBS.u_DeformSpread = glGetUniformLocationARB(tr.geometricFillShader_DBS.program, "u_DeformSpread");
		tr.geometricFillShader_DBS.u_Time = glGetUniformLocationARB(tr.geometricFillShader_DBS.program, "u_Time");

		glUseProgramObjectARB(tr.geometricFillShader_DBS.program);
		glUniform1iARB(tr.geometricFillShader_DBS.u_DiffuseMap, 0);
		glUniform1iARB(tr.geometricFillShader_DBS.u_NormalMap, 1);
		glUniform1iARB(tr.geometricFillShader_DBS.u_SpecularMap, 2);
		glUseProgramObjectARB(0);

		GLSL_ValidateProgram(tr.geometricFillShader_DBS.program);
		GLSL_ShowProgramUniforms(tr.geometricFillShader_DBS.program);
		GL_CheckErrors();

		// deferred omni-directional lighting post process effect
		GLSL_InitGPUShader(&tr.deferredLightingShader_DBS_omni, "deferredLighting_DBS_omni", ATTR_POSITION, qtrue, qtrue);

		tr.deferredLightingShader_DBS_omni.u_DiffuseMap =
			glGetUniformLocationARB(tr.deferredLightingShader_DBS_omni.program, "u_DiffuseMap");
		tr.deferredLightingShader_DBS_omni.u_NormalMap =
			glGetUniformLocationARB(tr.deferredLightingShader_DBS_omni.program, "u_NormalMap");
		tr.deferredLightingShader_DBS_omni.u_SpecularMap =
			glGetUniformLocationARB(tr.deferredLightingShader_DBS_omni.program, "u_SpecularMap");
		tr.deferredLightingShader_DBS_omni.u_DepthMap =
			glGetUniformLocationARB(tr.deferredLightingShader_DBS_omni.program, "u_DepthMap");
		tr.deferredLightingShader_DBS_omni.u_AttenuationMapXY =
			glGetUniformLocationARB(tr.deferredLightingShader_DBS_omni.program, "u_AttenuationMapXY");
		tr.deferredLightingShader_DBS_omni.u_AttenuationMapZ =
			glGetUniformLocationARB(tr.deferredLightingShader_DBS_omni.program, "u_AttenuationMapZ");
		tr.deferredLightingShader_DBS_omni.u_ShadowMap =
			glGetUniformLocationARB(tr.deferredLightingShader_DBS_omni.program, "u_ShadowMap");
		tr.deferredLightingShader_DBS_omni.u_ViewOrigin =
			glGetUniformLocationARB(tr.deferredLightingShader_DBS_omni.program, "u_ViewOrigin");
		tr.deferredLightingShader_DBS_omni.u_LightOrigin =
			glGetUniformLocationARB(tr.deferredLightingShader_DBS_omni.program, "u_LightOrigin");
		tr.deferredLightingShader_DBS_omni.u_LightColor =
			glGetUniformLocationARB(tr.deferredLightingShader_DBS_omni.program, "u_LightColor");
		tr.deferredLightingShader_DBS_omni.u_LightRadius =
			glGetUniformLocationARB(tr.deferredLightingShader_DBS_omni.program, "u_LightRadius");
		tr.deferredLightingShader_DBS_omni.u_LightScale =
			glGetUniformLocationARB(tr.deferredLightingShader_DBS_omni.program, "u_LightScale");
		tr.deferredLightingShader_DBS_omni.u_LightAttenuationMatrix =
			glGetUniformLocationARB(tr.deferredLightingShader_DBS_omni.program, "u_LightAttenuationMatrix");
		tr.deferredLightingShader_DBS_omni.u_ShadowCompare =
			glGetUniformLocationARB(tr.deferredLightingShader_DBS_omni.program, "u_ShadowCompare");
		tr.deferredLightingShader_DBS_omni.u_PortalClipping =
			glGetUniformLocationARB(tr.deferredLightingShader_DBS_omni.program, "u_PortalClipping");
		tr.deferredLightingShader_DBS_omni.u_PortalPlane =
			glGetUniformLocationARB(tr.deferredLightingShader_DBS_omni.program, "u_PortalPlane");
		tr.deferredLightingShader_DBS_omni.u_ModelViewProjectionMatrix =
			glGetUniformLocationARB(tr.deferredLightingShader_DBS_omni.program, "u_ModelViewProjectionMatrix");
		tr.deferredLightingShader_DBS_omni.u_UnprojectMatrix =
			glGetUniformLocationARB(tr.deferredLightingShader_DBS_omni.program, "u_UnprojectMatrix");

		glUseProgramObjectARB(tr.deferredLightingShader_DBS_omni.program);
		glUniform1iARB(tr.deferredLightingShader_DBS_omni.u_DiffuseMap, 0);
		glUniform1iARB(tr.deferredLightingShader_DBS_omni.u_NormalMap, 1);
		glUniform1iARB(tr.deferredLightingShader_DBS_omni.u_SpecularMap, 2);
		glUniform1iARB(tr.deferredLightingShader_DBS_omni.u_DepthMap, 3);
		glUniform1iARB(tr.deferredLightingShader_DBS_omni.u_AttenuationMapXY, 4);
		glUniform1iARB(tr.deferredLightingShader_DBS_omni.u_AttenuationMapZ, 5);
		glUniform1iARB(tr.deferredLightingShader_DBS_omni.u_ShadowMap, 6);
		glUseProgramObjectARB(0);

		GLSL_ValidateProgram(tr.deferredLightingShader_DBS_omni.program);
		GLSL_ShowProgramUniforms(tr.deferredLightingShader_DBS_omni.program);
		GL_CheckErrors();

		// deferred projective lighting post process effect
		GLSL_InitGPUShader(&tr.deferredLightingShader_DBS_proj, "deferredLighting_DBS_proj", ATTR_POSITION, qtrue, qtrue);

		tr.deferredLightingShader_DBS_proj.u_DiffuseMap =
			glGetUniformLocationARB(tr.deferredLightingShader_DBS_proj.program, "u_DiffuseMap");
		tr.deferredLightingShader_DBS_proj.u_NormalMap =
			glGetUniformLocationARB(tr.deferredLightingShader_DBS_proj.program, "u_NormalMap");
		tr.deferredLightingShader_DBS_proj.u_SpecularMap =
			glGetUniformLocationARB(tr.deferredLightingShader_DBS_proj.program, "u_SpecularMap");
		tr.deferredLightingShader_DBS_proj.u_DepthMap =
			glGetUniformLocationARB(tr.deferredLightingShader_DBS_proj.program, "u_DepthMap");
		tr.deferredLightingShader_DBS_proj.u_AttenuationMapXY =
			glGetUniformLocationARB(tr.deferredLightingShader_DBS_proj.program, "u_AttenuationMapXY");
		tr.deferredLightingShader_DBS_proj.u_AttenuationMapZ =
			glGetUniformLocationARB(tr.deferredLightingShader_DBS_proj.program, "u_AttenuationMapZ");
		tr.deferredLightingShader_DBS_proj.u_ShadowMap =
			glGetUniformLocationARB(tr.deferredLightingShader_DBS_proj.program, "u_ShadowMap");
		tr.deferredLightingShader_DBS_proj.u_ViewOrigin =
			glGetUniformLocationARB(tr.deferredLightingShader_DBS_proj.program, "u_ViewOrigin");
		tr.deferredLightingShader_DBS_proj.u_LightOrigin =
			glGetUniformLocationARB(tr.deferredLightingShader_DBS_proj.program, "u_LightOrigin");
		tr.deferredLightingShader_DBS_proj.u_LightColor =
			glGetUniformLocationARB(tr.deferredLightingShader_DBS_proj.program, "u_LightColor");
		tr.deferredLightingShader_DBS_proj.u_LightRadius =
			glGetUniformLocationARB(tr.deferredLightingShader_DBS_proj.program, "u_LightRadius");
		tr.deferredLightingShader_DBS_proj.u_LightScale =
			glGetUniformLocationARB(tr.deferredLightingShader_DBS_proj.program, "u_LightScale");
		tr.deferredLightingShader_DBS_proj.u_LightAttenuationMatrix =
			glGetUniformLocationARB(tr.deferredLightingShader_DBS_proj.program, "u_LightAttenuationMatrix");
		tr.deferredLightingShader_DBS_proj.u_ShadowMatrix =
			glGetUniformLocationARB(tr.deferredLightingShader_DBS_proj.program, "u_ShadowMatrix");
		tr.deferredLightingShader_DBS_proj.u_ShadowCompare =
			glGetUniformLocationARB(tr.deferredLightingShader_DBS_proj.program, "u_ShadowCompare");
		tr.deferredLightingShader_DBS_proj.u_PortalClipping =
			glGetUniformLocationARB(tr.deferredLightingShader_DBS_proj.program, "u_PortalClipping");
		tr.deferredLightingShader_DBS_proj.u_PortalPlane =
			glGetUniformLocationARB(tr.deferredLightingShader_DBS_proj.program, "u_PortalPlane");
		tr.deferredLightingShader_DBS_proj.u_ModelViewProjectionMatrix =
			glGetUniformLocationARB(tr.deferredLightingShader_DBS_proj.program, "u_ModelViewProjectionMatrix");
		tr.deferredLightingShader_DBS_proj.u_UnprojectMatrix =
			glGetUniformLocationARB(tr.deferredLightingShader_DBS_proj.program, "u_UnprojectMatrix");

		glUseProgramObjectARB(tr.deferredLightingShader_DBS_proj.program);
		glUniform1iARB(tr.deferredLightingShader_DBS_proj.u_DiffuseMap, 0);
		glUniform1iARB(tr.deferredLightingShader_DBS_proj.u_NormalMap, 1);
		glUniform1iARB(tr.deferredLightingShader_DBS_proj.u_SpecularMap, 2);
		glUniform1iARB(tr.deferredLightingShader_DBS_proj.u_DepthMap, 3);
		glUniform1iARB(tr.deferredLightingShader_DBS_proj.u_AttenuationMapXY, 4);
		glUniform1iARB(tr.deferredLightingShader_DBS_proj.u_AttenuationMapZ, 5);
		glUniform1iARB(tr.deferredLightingShader_DBS_proj.u_ShadowMap, 6);
		glUseProgramObjectARB(0);

		GLSL_ValidateProgram(tr.deferredLightingShader_DBS_proj.program);
		GLSL_ShowProgramUniforms(tr.deferredLightingShader_DBS_proj.program);
		GL_CheckErrors();

		// deferred projective lighting post process effect
		GLSL_InitGPUShader(&tr.deferredLightingShader_DBS_directional, "deferredLighting_DBS_directional", ATTR_POSITION, qtrue, qtrue);

		tr.deferredLightingShader_DBS_directional.u_DiffuseMap =
			glGetUniformLocationARB(tr.deferredLightingShader_DBS_directional.program, "u_DiffuseMap");
		tr.deferredLightingShader_DBS_directional.u_NormalMap =
			glGetUniformLocationARB(tr.deferredLightingShader_DBS_directional.program, "u_NormalMap");
		tr.deferredLightingShader_DBS_directional.u_SpecularMap =
			glGetUniformLocationARB(tr.deferredLightingShader_DBS_directional.program, "u_SpecularMap");
		tr.deferredLightingShader_DBS_directional.u_DepthMap =
			glGetUniformLocationARB(tr.deferredLightingShader_DBS_directional.program, "u_DepthMap");
		tr.deferredLightingShader_DBS_directional.u_AttenuationMapXY =
			glGetUniformLocationARB(tr.deferredLightingShader_DBS_directional.program, "u_AttenuationMapXY");
		tr.deferredLightingShader_DBS_directional.u_AttenuationMapZ =
			glGetUniformLocationARB(tr.deferredLightingShader_DBS_directional.program, "u_AttenuationMapZ");
		tr.deferredLightingShader_DBS_directional.u_ShadowMap0 =
			glGetUniformLocationARB(tr.deferredLightingShader_DBS_directional.program, "u_ShadowMap0");
		tr.deferredLightingShader_DBS_directional.u_ShadowMap1 =
			glGetUniformLocationARB(tr.deferredLightingShader_DBS_directional.program, "u_ShadowMap1");
		tr.deferredLightingShader_DBS_directional.u_ShadowMap2 =
			glGetUniformLocationARB(tr.deferredLightingShader_DBS_directional.program, "u_ShadowMap2");
		tr.deferredLightingShader_DBS_directional.u_ShadowMap3 =
			glGetUniformLocationARB(tr.deferredLightingShader_DBS_directional.program, "u_ShadowMap3");
		tr.deferredLightingShader_DBS_directional.u_ShadowMap4 =
			glGetUniformLocationARB(tr.deferredLightingShader_DBS_directional.program, "u_ShadowMap4");
		tr.deferredLightingShader_DBS_directional.u_ViewOrigin =
			glGetUniformLocationARB(tr.deferredLightingShader_DBS_directional.program, "u_ViewOrigin");
		tr.deferredLightingShader_DBS_directional.u_LightDir =
			glGetUniformLocationARB(tr.deferredLightingShader_DBS_directional.program, "u_LightDir");
		tr.deferredLightingShader_DBS_directional.u_LightColor =
			glGetUniformLocationARB(tr.deferredLightingShader_DBS_directional.program, "u_LightColor");
		tr.deferredLightingShader_DBS_directional.u_LightRadius =
			glGetUniformLocationARB(tr.deferredLightingShader_DBS_directional.program, "u_LightRadius");
		tr.deferredLightingShader_DBS_directional.u_LightScale =
			glGetUniformLocationARB(tr.deferredLightingShader_DBS_directional.program, "u_LightScale");
		tr.deferredLightingShader_DBS_directional.u_LightAttenuationMatrix =
			glGetUniformLocationARB(tr.deferredLightingShader_DBS_directional.program, "u_LightAttenuationMatrix");
		tr.deferredLightingShader_DBS_directional.u_ShadowMatrix =
			glGetUniformLocationARB(tr.deferredLightingShader_DBS_directional.program, "u_ShadowMatrix");
		tr.deferredLightingShader_DBS_directional.u_ShadowCompare =
			glGetUniformLocationARB(tr.deferredLightingShader_DBS_directional.program, "u_ShadowCompare");
		tr.deferredLightingShader_DBS_directional.u_ShadowParallelSplitDistances =
			glGetUniformLocationARB(tr.deferredLightingShader_DBS_directional.program, "u_ShadowParallelSplitDistances");
		tr.deferredLightingShader_DBS_directional.u_PortalClipping =
			glGetUniformLocationARB(tr.deferredLightingShader_DBS_directional.program, "u_PortalClipping");
		tr.deferredLightingShader_DBS_directional.u_PortalPlane =
			glGetUniformLocationARB(tr.deferredLightingShader_DBS_directional.program, "u_PortalPlane");
		tr.deferredLightingShader_DBS_directional.u_ModelViewProjectionMatrix =
			glGetUniformLocationARB(tr.deferredLightingShader_DBS_directional.program, "u_ModelViewProjectionMatrix");
		tr.deferredLightingShader_DBS_directional.u_UnprojectMatrix =
			glGetUniformLocationARB(tr.deferredLightingShader_DBS_directional.program, "u_UnprojectMatrix");
		tr.deferredLightingShader_DBS_directional.u_ViewMatrix =
			glGetUniformLocationARB(tr.deferredLightingShader_DBS_directional.program, "u_ViewMatrix");

		glUseProgramObjectARB(tr.deferredLightingShader_DBS_directional.program);
		glUniform1iARB(tr.deferredLightingShader_DBS_directional.u_DiffuseMap, 0);
		glUniform1iARB(tr.deferredLightingShader_DBS_directional.u_NormalMap, 1);
		glUniform1iARB(tr.deferredLightingShader_DBS_directional.u_SpecularMap, 2);
		glUniform1iARB(tr.deferredLightingShader_DBS_directional.u_DepthMap, 3);
		glUniform1iARB(tr.deferredLightingShader_DBS_directional.u_AttenuationMapXY, 4);
		glUniform1iARB(tr.deferredLightingShader_DBS_directional.u_AttenuationMapZ, 5);
		glUniform1iARB(tr.deferredLightingShader_DBS_directional.u_ShadowMap0, 6);
		glUniform1iARB(tr.deferredLightingShader_DBS_directional.u_ShadowMap1, 7);
		glUniform1iARB(tr.deferredLightingShader_DBS_directional.u_ShadowMap2, 8);
		glUniform1iARB(tr.deferredLightingShader_DBS_directional.u_ShadowMap3, 9);
		glUniform1iARB(tr.deferredLightingShader_DBS_directional.u_ShadowMap4, 10);
		glUseProgramObjectARB(0);

		GLSL_ValidateProgram(tr.deferredLightingShader_DBS_directional.program);
		GLSL_ShowProgramUniforms(tr.deferredLightingShader_DBS_directional.program);
		GL_CheckErrors();
	}

	// black depth fill rendering with textures
	GLSL_InitGPUShader(&tr.depthFillShader, "depthFill", ATTR_POSITION | ATTR_NORMAL | ATTR_TEXCOORD | ATTR_COLOR, qtrue, qtrue);

	tr.depthFillShader.u_ColorMap = glGetUniformLocationARB(tr.depthFillShader.program, "u_ColorMap");
	tr.depthFillShader.u_ColorTextureMatrix = glGetUniformLocationARB(tr.depthFillShader.program, "u_ColorTextureMatrix");
	tr.depthFillShader.u_AlphaTest = glGetUniformLocationARB(tr.depthFillShader.program, "u_AlphaTest");
	tr.depthFillShader.u_AmbientColor = glGetUniformLocationARB(tr.depthFillShader.program, "u_AmbientColor");
	tr.depthFillShader.u_ModelViewProjectionMatrix =
		glGetUniformLocationARB(tr.depthFillShader.program, "u_ModelViewProjectionMatrix");
	if(glConfig2.vboVertexSkinningAvailable)
	{
		tr.depthFillShader.u_VertexSkinning = glGetUniformLocationARB(tr.depthFillShader.program, "u_VertexSkinning");
		tr.depthFillShader.u_BoneMatrix = glGetUniformLocationARB(tr.depthFillShader.program, "u_BoneMatrix");
	}
	tr.depthFillShader.u_DeformGen = glGetUniformLocationARB(tr.depthFillShader.program, "u_DeformGen");
	tr.depthFillShader.u_DeformWave = glGetUniformLocationARB(tr.depthFillShader.program, "u_DeformWave");
	tr.depthFillShader.u_DeformBulge = glGetUniformLocationARB(tr.depthFillShader.program, "u_DeformBulge");
	tr.depthFillShader.u_DeformSpread = glGetUniformLocationARB(tr.depthFillShader.program, "u_DeformSpread");
	tr.depthFillShader.u_Time = glGetUniformLocationARB(tr.depthFillShader.program, "u_Time");

	glUseProgramObjectARB(tr.depthFillShader.program);
	glUniform1iARB(tr.depthFillShader.u_ColorMap, 0);
	glUseProgramObjectARB(0);

	GLSL_ValidateProgram(tr.depthFillShader.program);
	GLSL_ShowProgramUniforms(tr.depthFillShader.program);
	GL_CheckErrors();

	// colored depth test rendering for occlusion testing
	GLSL_InitGPUShader(&tr.depthTestShader, "depthTest", ATTR_POSITION | ATTR_TEXCOORD, qtrue, qtrue);

	tr.depthTestShader.u_ColorMap = glGetUniformLocationARB(tr.depthTestShader.program, "u_ColorMap");
	tr.depthTestShader.u_CurrentMap = glGetUniformLocationARB(tr.depthTestShader.program, "u_CurrentMap");
	tr.depthTestShader.u_ColorTextureMatrix = glGetUniformLocationARB(tr.depthTestShader.program, "u_ColorTextureMatrix");
	tr.depthTestShader.u_ModelViewProjectionMatrix =
		glGetUniformLocationARB(tr.depthTestShader.program, "u_ModelViewProjectionMatrix");

	glUseProgramObjectARB(tr.depthTestShader.program);
	glUniform1iARB(tr.depthTestShader.u_ColorMap, 0);
	glUniform1iARB(tr.depthTestShader.u_CurrentMap, 1);
	glUseProgramObjectARB(0);

	GLSL_ValidateProgram(tr.depthTestShader.program);
	GLSL_ShowProgramUniforms(tr.depthTestShader.program);
	GL_CheckErrors();

	// depth to color encoding
	GLSL_InitGPUShader(&tr.depthToColorShader, "depthToColor", ATTR_POSITION, qtrue, qtrue);

	tr.depthToColorShader.u_ModelViewProjectionMatrix =
		glGetUniformLocationARB(tr.depthToColorShader.program, "u_ModelViewProjectionMatrix");
	if(glConfig2.vboVertexSkinningAvailable)
	{
		tr.depthToColorShader.u_VertexSkinning = glGetUniformLocationARB(tr.depthToColorShader.program, "u_VertexSkinning");
		tr.depthToColorShader.u_BoneMatrix = glGetUniformLocationARB(tr.depthToColorShader.program, "u_BoneMatrix");
	}

	glUseProgramObjectARB(tr.depthToColorShader.program);
	//glUniform1iARB(tr.depthToColorShader.u_ColorMap, 0);
	glUseProgramObjectARB(0);

	GLSL_ValidateProgram(tr.depthToColorShader.program);
	GLSL_ShowProgramUniforms(tr.depthToColorShader.program);
	GL_CheckErrors();

	// shadow volume extrusion
	GLSL_InitGPUShader(&tr.shadowExtrudeShader, "shadowExtrude", ATTR_POSITION, qtrue, qtrue);

	tr.shadowExtrudeShader.u_LightOrigin = glGetUniformLocationARB(tr.shadowExtrudeShader.program, "u_LightOrigin");
	tr.shadowExtrudeShader.u_ModelViewProjectionMatrix =
		glGetUniformLocationARB(tr.shadowExtrudeShader.program, "u_ModelViewProjectionMatrix");

	GLSL_ValidateProgram(tr.shadowExtrudeShader.program);
	GLSL_ShowProgramUniforms(tr.shadowExtrudeShader.program);
	GL_CheckErrors();

	// shadowmap distance compression
	GLSL_InitGPUShader(&tr.shadowFillShader, "shadowFill", ATTR_POSITION | ATTR_NORMAL | ATTR_TEXCOORD, qtrue, qtrue);

	tr.shadowFillShader.u_ColorMap = glGetUniformLocationARB(tr.shadowFillShader.program, "u_ColorMap");
	tr.shadowFillShader.u_ColorTextureMatrix = glGetUniformLocationARB(tr.shadowFillShader.program, "u_ColorTextureMatrix");
	tr.shadowFillShader.u_AlphaTest = glGetUniformLocationARB(tr.shadowFillShader.program, "u_AlphaTest");
	tr.shadowFillShader.u_LightOrigin = glGetUniformLocationARB(tr.shadowFillShader.program, "u_LightOrigin");
	tr.shadowFillShader.u_LightRadius = glGetUniformLocationARB(tr.shadowFillShader.program, "u_LightRadius");
	tr.shadowFillShader.u_LightParallel = glGetUniformLocationARB(tr.shadowFillShader.program, "u_LightParallel");
	tr.shadowFillShader.u_ModelMatrix = glGetUniformLocationARB(tr.shadowFillShader.program, "u_ModelMatrix");
	tr.shadowFillShader.u_ModelViewProjectionMatrix =
		glGetUniformLocationARB(tr.shadowFillShader.program, "u_ModelViewProjectionMatrix");
	if(glConfig2.vboVertexSkinningAvailable)
	{
		tr.shadowFillShader.u_VertexSkinning = glGetUniformLocationARB(tr.shadowFillShader.program, "u_VertexSkinning");
		tr.shadowFillShader.u_BoneMatrix = glGetUniformLocationARB(tr.shadowFillShader.program, "u_BoneMatrix");
	}
	tr.shadowFillShader.u_DeformGen = glGetUniformLocationARB(tr.shadowFillShader.program, "u_DeformGen");
	tr.shadowFillShader.u_DeformWave = glGetUniformLocationARB(tr.shadowFillShader.program, "u_DeformWave");
	tr.shadowFillShader.u_DeformBulge = glGetUniformLocationARB(tr.shadowFillShader.program, "u_DeformBulge");
	tr.shadowFillShader.u_DeformSpread = glGetUniformLocationARB(tr.shadowFillShader.program, "u_DeformSpread");
	tr.shadowFillShader.u_Time = glGetUniformLocationARB(tr.shadowFillShader.program, "u_Time");

	glUseProgramObjectARB(tr.shadowFillShader.program);
	glUniform1iARB(tr.shadowFillShader.u_ColorMap, 0);
	glUseProgramObjectARB(0);

	GLSL_ValidateProgram(tr.shadowFillShader.program);
	GLSL_ShowProgramUniforms(tr.shadowFillShader.program);
	GL_CheckErrors();

	// omni-directional specular bump mapping ( Doom3 style )
	GLSL_InitGPUShader(&tr.forwardLightingShader_DBS_omni,
					   "forwardLighting_DBS_omni",
					   ATTR_POSITION | ATTR_TEXCOORD | ATTR_TANGENT | ATTR_BINORMAL | ATTR_NORMAL | ATTR_COLOR, qtrue, qtrue);

	tr.forwardLightingShader_DBS_omni.u_DiffuseMap =
		glGetUniformLocationARB(tr.forwardLightingShader_DBS_omni.program, "u_DiffuseMap");
	tr.forwardLightingShader_DBS_omni.u_NormalMap =
		glGetUniformLocationARB(tr.forwardLightingShader_DBS_omni.program, "u_NormalMap");
	tr.forwardLightingShader_DBS_omni.u_SpecularMap =
		glGetUniformLocationARB(tr.forwardLightingShader_DBS_omni.program, "u_SpecularMap");
	tr.forwardLightingShader_DBS_omni.u_AttenuationMapXY =
		glGetUniformLocationARB(tr.forwardLightingShader_DBS_omni.program, "u_AttenuationMapXY");
	tr.forwardLightingShader_DBS_omni.u_AttenuationMapZ =
		glGetUniformLocationARB(tr.forwardLightingShader_DBS_omni.program, "u_AttenuationMapZ");
	if(r_shadows->integer >= SHADOWING_VSM16)
	{
		tr.forwardLightingShader_DBS_omni.u_ShadowMap =
			glGetUniformLocationARB(tr.forwardLightingShader_DBS_omni.program, "u_ShadowMap");
	}
	tr.forwardLightingShader_DBS_omni.u_DiffuseTextureMatrix =
		glGetUniformLocationARB(tr.forwardLightingShader_DBS_omni.program, "u_DiffuseTextureMatrix");
	tr.forwardLightingShader_DBS_omni.u_NormalTextureMatrix =
		glGetUniformLocationARB(tr.forwardLightingShader_DBS_omni.program, "u_NormalTextureMatrix");
	tr.forwardLightingShader_DBS_omni.u_SpecularTextureMatrix =
		glGetUniformLocationARB(tr.forwardLightingShader_DBS_omni.program, "u_SpecularTextureMatrix");
	tr.forwardLightingShader_DBS_omni.u_ViewOrigin =
		glGetUniformLocationARB(tr.forwardLightingShader_DBS_omni.program, "u_ViewOrigin");
//	tr.forwardLightingShader_DBS_omni.u_InverseVertexColor =
//		glGetUniformLocationARB(tr.forwardLightingShader_DBS_omni.program, "u_InverseVertexColor");
	tr.forwardLightingShader_DBS_omni.u_LightOrigin =
		glGetUniformLocationARB(tr.forwardLightingShader_DBS_omni.program, "u_LightOrigin");
	tr.forwardLightingShader_DBS_omni.u_LightColor =
		glGetUniformLocationARB(tr.forwardLightingShader_DBS_omni.program, "u_LightColor");
	tr.forwardLightingShader_DBS_omni.u_LightRadius =
		glGetUniformLocationARB(tr.forwardLightingShader_DBS_omni.program, "u_LightRadius");
	tr.forwardLightingShader_DBS_omni.u_LightScale =
		glGetUniformLocationARB(tr.forwardLightingShader_DBS_omni.program, "u_LightScale");
	tr.forwardLightingShader_DBS_omni.u_LightWrapAround =
		glGetUniformLocationARB(tr.forwardLightingShader_DBS_omni.program, "u_LightWrapAround");
	tr.forwardLightingShader_DBS_omni.u_LightAttenuationMatrix =
		glGetUniformLocationARB(tr.forwardLightingShader_DBS_omni.program, "u_LightAttenuationMatrix");
	tr.forwardLightingShader_DBS_omni.u_ShadowCompare =
		glGetUniformLocationARB(tr.forwardLightingShader_DBS_omni.program, "u_ShadowCompare");
	tr.forwardLightingShader_DBS_omni.u_ShadowTexelSize =
		glGetUniformLocationARB(tr.forwardLightingShader_DBS_omni.program, "u_ShadowTexelSize");
	tr.forwardLightingShader_DBS_omni.u_ShadowBlur =
		glGetUniformLocationARB(tr.forwardLightingShader_DBS_omni.program, "u_ShadowBlur");
	tr.forwardLightingShader_DBS_omni.u_PortalClipping =
		glGetUniformLocationARB(tr.forwardLightingShader_DBS_omni.program, "u_PortalClipping");
	tr.forwardLightingShader_DBS_omni.u_PortalPlane =
		glGetUniformLocationARB(tr.forwardLightingShader_DBS_omni.program, "u_PortalPlane");
	tr.forwardLightingShader_DBS_omni.u_ModelMatrix =
		glGetUniformLocationARB(tr.forwardLightingShader_DBS_omni.program, "u_ModelMatrix");
	tr.forwardLightingShader_DBS_omni.u_ModelViewProjectionMatrix =
		glGetUniformLocationARB(tr.forwardLightingShader_DBS_omni.program, "u_ModelViewProjectionMatrix");
	if(glConfig2.vboVertexSkinningAvailable)
	{
		tr.forwardLightingShader_DBS_omni.u_VertexSkinning =
			glGetUniformLocationARB(tr.forwardLightingShader_DBS_omni.program, "u_VertexSkinning");
		tr.forwardLightingShader_DBS_omni.u_BoneMatrix =
			glGetUniformLocationARB(tr.forwardLightingShader_DBS_omni.program, "u_BoneMatrix");
	}
	tr.forwardLightingShader_DBS_omni.u_DeformGen =
		glGetUniformLocationARB(tr.forwardLightingShader_DBS_omni.program, "u_DeformGen");
	tr.forwardLightingShader_DBS_omni.u_DeformWave =
		glGetUniformLocationARB(tr.forwardLightingShader_DBS_omni.program, "u_DeformWave");
	tr.forwardLightingShader_DBS_omni.u_DeformBulge =
		glGetUniformLocationARB(tr.forwardLightingShader_DBS_omni.program, "u_DeformBulge");
	tr.forwardLightingShader_DBS_omni.u_DeformSpread =
		glGetUniformLocationARB(tr.forwardLightingShader_DBS_omni.program, "u_DeformSpread");
	tr.forwardLightingShader_DBS_omni.u_Time =
		glGetUniformLocationARB(tr.forwardLightingShader_DBS_omni.program, "u_Time");

	glUseProgramObjectARB(tr.forwardLightingShader_DBS_omni.program);
	glUniform1iARB(tr.forwardLightingShader_DBS_omni.u_DiffuseMap, 0);
	glUniform1iARB(tr.forwardLightingShader_DBS_omni.u_NormalMap, 1);
	glUniform1iARB(tr.forwardLightingShader_DBS_omni.u_SpecularMap, 2);
	glUniform1iARB(tr.forwardLightingShader_DBS_omni.u_AttenuationMapXY, 3);
	glUniform1iARB(tr.forwardLightingShader_DBS_omni.u_AttenuationMapZ, 4);
	if(r_shadows->integer >= SHADOWING_VSM16)
	{
		glUniform1iARB(tr.forwardLightingShader_DBS_omni.u_ShadowMap, 5);
	}
	glUseProgramObjectARB(0);

	GLSL_ValidateProgram(tr.forwardLightingShader_DBS_omni.program);
	GLSL_ShowProgramUniforms(tr.forwardLightingShader_DBS_omni.program);
	GL_CheckErrors();

	// projective lighting ( Doom3 style )
	GLSL_InitGPUShader(&tr.forwardLightingShader_DBS_proj, "forwardLighting_DBS_proj",
					   ATTR_POSITION | ATTR_TEXCOORD | ATTR_TANGENT | ATTR_BINORMAL | ATTR_NORMAL | ATTR_COLOR, qtrue, qtrue);

	tr.forwardLightingShader_DBS_proj.u_DiffuseMap =
		glGetUniformLocationARB(tr.forwardLightingShader_DBS_proj.program, "u_DiffuseMap");
	tr.forwardLightingShader_DBS_proj.u_NormalMap =
		glGetUniformLocationARB(tr.forwardLightingShader_DBS_proj.program, "u_NormalMap");
	tr.forwardLightingShader_DBS_proj.u_SpecularMap =
		glGetUniformLocationARB(tr.forwardLightingShader_DBS_proj.program, "u_SpecularMap");
	tr.forwardLightingShader_DBS_proj.u_AttenuationMapXY =
		glGetUniformLocationARB(tr.forwardLightingShader_DBS_proj.program, "u_AttenuationMapXY");
	tr.forwardLightingShader_DBS_proj.u_AttenuationMapZ =
		glGetUniformLocationARB(tr.forwardLightingShader_DBS_proj.program, "u_AttenuationMapZ");
	if(r_shadows->integer >= SHADOWING_VSM16)
	{
		tr.forwardLightingShader_DBS_proj.u_ShadowMap =
			glGetUniformLocationARB(tr.forwardLightingShader_DBS_proj.program, "u_ShadowMap");
	}
	tr.forwardLightingShader_DBS_proj.u_DiffuseTextureMatrix =
		glGetUniformLocationARB(tr.forwardLightingShader_DBS_proj.program, "u_DiffuseTextureMatrix");
	tr.forwardLightingShader_DBS_proj.u_NormalTextureMatrix =
		glGetUniformLocationARB(tr.forwardLightingShader_DBS_proj.program, "u_NormalTextureMatrix");
	tr.forwardLightingShader_DBS_proj.u_SpecularTextureMatrix =
		glGetUniformLocationARB(tr.forwardLightingShader_DBS_proj.program, "u_SpecularTextureMatrix");
	tr.forwardLightingShader_DBS_proj.u_ViewOrigin =
		glGetUniformLocationARB(tr.forwardLightingShader_DBS_proj.program, "u_ViewOrigin");
//	tr.forwardLightingShader_DBS_proj.u_InverseVertexColor =
//		glGetUniformLocationARB(tr.forwardLightingShader_DBS_proj.program, "u_InverseVertexColor");
	tr.forwardLightingShader_DBS_proj.u_LightOrigin =
		glGetUniformLocationARB(tr.forwardLightingShader_DBS_proj.program, "u_LightOrigin");
	tr.forwardLightingShader_DBS_proj.u_LightColor =
		glGetUniformLocationARB(tr.forwardLightingShader_DBS_proj.program, "u_LightColor");
	tr.forwardLightingShader_DBS_proj.u_LightRadius =
		glGetUniformLocationARB(tr.forwardLightingShader_DBS_proj.program, "u_LightRadius");
	tr.forwardLightingShader_DBS_proj.u_LightScale =
		glGetUniformLocationARB(tr.forwardLightingShader_DBS_proj.program, "u_LightScale");
	tr.forwardLightingShader_DBS_proj.u_LightWrapAround =
		glGetUniformLocationARB(tr.forwardLightingShader_DBS_proj.program, "u_LightWrapAround");
	tr.forwardLightingShader_DBS_proj.u_LightAttenuationMatrix =
		glGetUniformLocationARB(tr.forwardLightingShader_DBS_proj.program, "u_LightAttenuationMatrix");
	tr.forwardLightingShader_DBS_proj.u_ShadowMatrix =
		glGetUniformLocationARB(tr.forwardLightingShader_DBS_proj.program, "u_ShadowMatrix");
	tr.forwardLightingShader_DBS_proj.u_ShadowCompare =
		glGetUniformLocationARB(tr.forwardLightingShader_DBS_proj.program, "u_ShadowCompare");
	tr.forwardLightingShader_DBS_proj.u_ShadowTexelSize =
		glGetUniformLocationARB(tr.forwardLightingShader_DBS_proj.program, "u_ShadowTexelSize");
	tr.forwardLightingShader_DBS_proj.u_ShadowBlur =
		glGetUniformLocationARB(tr.forwardLightingShader_DBS_proj.program, "u_ShadowBlur");
	tr.forwardLightingShader_DBS_proj.u_PortalClipping =
		glGetUniformLocationARB(tr.forwardLightingShader_DBS_proj.program, "u_PortalClipping");
	tr.forwardLightingShader_DBS_proj.u_PortalPlane =
		glGetUniformLocationARB(tr.forwardLightingShader_DBS_proj.program, "u_PortalPlane");
	tr.forwardLightingShader_DBS_proj.u_ModelMatrix =
		glGetUniformLocationARB(tr.forwardLightingShader_DBS_proj.program, "u_ModelMatrix");
	tr.forwardLightingShader_DBS_proj.u_ModelViewProjectionMatrix =
		glGetUniformLocationARB(tr.forwardLightingShader_DBS_proj.program, "u_ModelViewProjectionMatrix");
	if(glConfig2.vboVertexSkinningAvailable)
	{
		tr.forwardLightingShader_DBS_proj.u_VertexSkinning =
			glGetUniformLocationARB(tr.forwardLightingShader_DBS_proj.program, "u_VertexSkinning");
		tr.forwardLightingShader_DBS_proj.u_BoneMatrix =
			glGetUniformLocationARB(tr.forwardLightingShader_DBS_proj.program, "u_BoneMatrix");
	}
	tr.forwardLightingShader_DBS_proj.u_DeformGen =
		glGetUniformLocationARB(tr.forwardLightingShader_DBS_proj.program, "u_DeformGen");
	tr.forwardLightingShader_DBS_proj.u_DeformWave =
		glGetUniformLocationARB(tr.forwardLightingShader_DBS_proj.program, "u_DeformWave");
	tr.forwardLightingShader_DBS_proj.u_DeformBulge =
		glGetUniformLocationARB(tr.forwardLightingShader_DBS_proj.program, "u_DeformBulge");
	tr.forwardLightingShader_DBS_proj.u_DeformSpread =
		glGetUniformLocationARB(tr.forwardLightingShader_DBS_proj.program, "u_DeformSpread");
	tr.forwardLightingShader_DBS_proj.u_Time =
		glGetUniformLocationARB(tr.forwardLightingShader_DBS_proj.program, "u_Time");

	glUseProgramObjectARB(tr.forwardLightingShader_DBS_proj.program);
	glUniform1iARB(tr.forwardLightingShader_DBS_proj.u_DiffuseMap, 0);
	glUniform1iARB(tr.forwardLightingShader_DBS_proj.u_NormalMap, 1);
	glUniform1iARB(tr.forwardLightingShader_DBS_proj.u_SpecularMap, 2);
	glUniform1iARB(tr.forwardLightingShader_DBS_proj.u_AttenuationMapXY, 3);
	glUniform1iARB(tr.forwardLightingShader_DBS_proj.u_AttenuationMapZ, 4);
	if(r_shadows->integer >= SHADOWING_VSM16)
	{
		glUniform1iARB(tr.forwardLightingShader_DBS_proj.u_ShadowMap, 5);
	}
	glUseProgramObjectARB(0);

	GLSL_ValidateProgram(tr.forwardLightingShader_DBS_proj.program);
	GLSL_ShowProgramUniforms(tr.forwardLightingShader_DBS_proj.program);
	GL_CheckErrors();



	// directional sun lighting ( Doom3 style )
	GLSL_InitGPUShader(&tr.forwardLightingShader_DBS_directional, "forwardLighting_DBS_directional",
					   ATTR_POSITION | ATTR_TEXCOORD | ATTR_TANGENT | ATTR_BINORMAL | ATTR_NORMAL | ATTR_COLOR, qtrue, qtrue);

	tr.forwardLightingShader_DBS_directional.u_DiffuseMap =
		glGetUniformLocationARB(tr.forwardLightingShader_DBS_directional.program, "u_DiffuseMap");
	tr.forwardLightingShader_DBS_directional.u_NormalMap =
		glGetUniformLocationARB(tr.forwardLightingShader_DBS_directional.program, "u_NormalMap");
	tr.forwardLightingShader_DBS_directional.u_SpecularMap =
		glGetUniformLocationARB(tr.forwardLightingShader_DBS_directional.program, "u_SpecularMap");
	tr.forwardLightingShader_DBS_directional.u_AttenuationMapXY =
		glGetUniformLocationARB(tr.forwardLightingShader_DBS_directional.program, "u_AttenuationMapXY");
	tr.forwardLightingShader_DBS_directional.u_AttenuationMapZ =
		glGetUniformLocationARB(tr.forwardLightingShader_DBS_directional.program, "u_AttenuationMapZ");
	if(r_shadows->integer >= SHADOWING_VSM16)
	{
		tr.forwardLightingShader_DBS_directional.u_ShadowMap0 =
			glGetUniformLocationARB(tr.forwardLightingShader_DBS_directional.program, "u_ShadowMap0");
		tr.forwardLightingShader_DBS_directional.u_ShadowMap1 =
			glGetUniformLocationARB(tr.forwardLightingShader_DBS_directional.program, "u_ShadowMap1");
		tr.forwardLightingShader_DBS_directional.u_ShadowMap2 =
			glGetUniformLocationARB(tr.forwardLightingShader_DBS_directional.program, "u_ShadowMap2");
		tr.forwardLightingShader_DBS_directional.u_ShadowMap3 =
			glGetUniformLocationARB(tr.forwardLightingShader_DBS_directional.program, "u_ShadowMap3");
		tr.forwardLightingShader_DBS_directional.u_ShadowMap4 =
			glGetUniformLocationARB(tr.forwardLightingShader_DBS_directional.program, "u_ShadowMap4");
	}
	tr.forwardLightingShader_DBS_directional.u_DiffuseTextureMatrix =
		glGetUniformLocationARB(tr.forwardLightingShader_DBS_directional.program, "u_DiffuseTextureMatrix");
	tr.forwardLightingShader_DBS_directional.u_NormalTextureMatrix =
		glGetUniformLocationARB(tr.forwardLightingShader_DBS_directional.program, "u_NormalTextureMatrix");
	tr.forwardLightingShader_DBS_directional.u_SpecularTextureMatrix =
		glGetUniformLocationARB(tr.forwardLightingShader_DBS_directional.program, "u_SpecularTextureMatrix");
	tr.forwardLightingShader_DBS_directional.u_ViewOrigin =
		glGetUniformLocationARB(tr.forwardLightingShader_DBS_directional.program, "u_ViewOrigin");
//	tr.forwardLightingShader_DBS_directional.u_InverseVertexColor =
//		glGetUniformLocationARB(tr.forwardLightingShader_DBS_directional.program, "u_InverseVertexColor");
	tr.forwardLightingShader_DBS_directional.u_LightDir =
		glGetUniformLocationARB(tr.forwardLightingShader_DBS_directional.program, "u_LightDir");
	tr.forwardLightingShader_DBS_directional.u_LightColor =
		glGetUniformLocationARB(tr.forwardLightingShader_DBS_directional.program, "u_LightColor");
	tr.forwardLightingShader_DBS_directional.u_LightRadius =
		glGetUniformLocationARB(tr.forwardLightingShader_DBS_directional.program, "u_LightRadius");
	tr.forwardLightingShader_DBS_directional.u_LightScale =
		glGetUniformLocationARB(tr.forwardLightingShader_DBS_directional.program, "u_LightScale");
	tr.forwardLightingShader_DBS_directional.u_LightWrapAround =
		glGetUniformLocationARB(tr.forwardLightingShader_DBS_directional.program, "u_LightWrapAround");
	tr.forwardLightingShader_DBS_directional.u_LightAttenuationMatrix =
		glGetUniformLocationARB(tr.forwardLightingShader_DBS_directional.program, "u_LightAttenuationMatrix");
	tr.forwardLightingShader_DBS_directional.u_ShadowMatrix =
		glGetUniformLocationARB(tr.forwardLightingShader_DBS_directional.program, "u_ShadowMatrix");
	tr.forwardLightingShader_DBS_directional.u_ShadowCompare =
		glGetUniformLocationARB(tr.forwardLightingShader_DBS_directional.program, "u_ShadowCompare");
	tr.forwardLightingShader_DBS_directional.u_ShadowParallelSplitDistances =
		glGetUniformLocationARB(tr.forwardLightingShader_DBS_directional.program, "u_ShadowParallelSplitDistances");
	tr.forwardLightingShader_DBS_directional.u_ShadowTexelSize =
		glGetUniformLocationARB(tr.forwardLightingShader_DBS_directional.program, "u_ShadowTexelSize");
	tr.forwardLightingShader_DBS_directional.u_ShadowBlur =
		glGetUniformLocationARB(tr.forwardLightingShader_DBS_directional.program, "u_ShadowBlur");
	tr.forwardLightingShader_DBS_directional.u_PortalClipping =
		glGetUniformLocationARB(tr.forwardLightingShader_DBS_directional.program, "u_PortalClipping");
	tr.forwardLightingShader_DBS_directional.u_PortalPlane =
		glGetUniformLocationARB(tr.forwardLightingShader_DBS_directional.program, "u_PortalPlane");
	tr.forwardLightingShader_DBS_directional.u_ModelMatrix =
		glGetUniformLocationARB(tr.forwardLightingShader_DBS_directional.program, "u_ModelMatrix");
	tr.forwardLightingShader_DBS_directional.u_ViewMatrix =
		glGetUniformLocationARB(tr.forwardLightingShader_DBS_directional.program, "u_ViewMatrix");
	tr.forwardLightingShader_DBS_directional.u_ModelViewProjectionMatrix =
		glGetUniformLocationARB(tr.forwardLightingShader_DBS_directional.program, "u_ModelViewProjectionMatrix");
	if(glConfig2.vboVertexSkinningAvailable)
	{
		tr.forwardLightingShader_DBS_directional.u_VertexSkinning =
			glGetUniformLocationARB(tr.forwardLightingShader_DBS_directional.program, "u_VertexSkinning");
		tr.forwardLightingShader_DBS_directional.u_BoneMatrix =
			glGetUniformLocationARB(tr.forwardLightingShader_DBS_directional.program, "u_BoneMatrix");
	}
	tr.forwardLightingShader_DBS_directional.u_DeformGen =
		glGetUniformLocationARB(tr.forwardLightingShader_DBS_directional.program, "u_DeformGen");
	tr.forwardLightingShader_DBS_directional.u_DeformWave =
		glGetUniformLocationARB(tr.forwardLightingShader_DBS_directional.program, "u_DeformWave");
	tr.forwardLightingShader_DBS_directional.u_DeformBulge =
		glGetUniformLocationARB(tr.forwardLightingShader_DBS_directional.program, "u_DeformBulge");
	tr.forwardLightingShader_DBS_directional.u_DeformSpread =
		glGetUniformLocationARB(tr.forwardLightingShader_DBS_directional.program, "u_DeformSpread");
	tr.forwardLightingShader_DBS_directional.u_Time =
		glGetUniformLocationARB(tr.forwardLightingShader_DBS_directional.program, "u_Time");

	glUseProgramObjectARB(tr.forwardLightingShader_DBS_directional.program);
	glUniform1iARB(tr.forwardLightingShader_DBS_directional.u_DiffuseMap, 0);
	glUniform1iARB(tr.forwardLightingShader_DBS_directional.u_NormalMap, 1);
	glUniform1iARB(tr.forwardLightingShader_DBS_directional.u_SpecularMap, 2);
	glUniform1iARB(tr.forwardLightingShader_DBS_directional.u_AttenuationMapXY, 3);
	glUniform1iARB(tr.forwardLightingShader_DBS_directional.u_AttenuationMapZ, 4);
	if(r_shadows->integer >= SHADOWING_VSM16)
	{
		glUniform1iARB(tr.forwardLightingShader_DBS_directional.u_ShadowMap0, 5);
		glUniform1iARB(tr.forwardLightingShader_DBS_directional.u_ShadowMap1, 6);
		glUniform1iARB(tr.forwardLightingShader_DBS_directional.u_ShadowMap2, 7);
		glUniform1iARB(tr.forwardLightingShader_DBS_directional.u_ShadowMap3, 8);
		glUniform1iARB(tr.forwardLightingShader_DBS_directional.u_ShadowMap4, 9);
	}
	glUseProgramObjectARB(0);

	GLSL_ValidateProgram(tr.forwardLightingShader_DBS_directional.program);
	GLSL_ShowProgramUniforms(tr.forwardLightingShader_DBS_directional.program);
	GL_CheckErrors();


	// forward shading using the light buffer as in pre pass deferred lighting
	GLSL_InitGPUShader(&tr.forwardLightingShader_DBS_post, "forwardLighting_DBS_post",
							   ATTR_POSITION | ATTR_TEXCOORD | ATTR_TANGENT | ATTR_BINORMAL | ATTR_NORMAL, qtrue, qtrue);

	tr.forwardLightingShader_DBS_post.u_DiffuseMap = glGetUniformLocationARB(tr.forwardLightingShader_DBS_post.program, "u_DiffuseMap");
	tr.forwardLightingShader_DBS_post.u_NormalMap = glGetUniformLocationARB(tr.forwardLightingShader_DBS_post.program, "u_NormalMap");
	tr.forwardLightingShader_DBS_post.u_SpecularMap = glGetUniformLocationARB(tr.forwardLightingShader_DBS_post.program, "u_SpecularMap");
	tr.forwardLightingShader_DBS_post.u_LightMap = glGetUniformLocationARB(tr.forwardLightingShader_DBS_post.program, "u_LightMap");
	tr.forwardLightingShader_DBS_post.u_DiffuseTextureMatrix =
		glGetUniformLocationARB(tr.forwardLightingShader_DBS_post.program, "u_DiffuseTextureMatrix");
	tr.forwardLightingShader_DBS_post.u_NormalTextureMatrix =
		glGetUniformLocationARB(tr.forwardLightingShader_DBS_post.program, "u_NormalTextureMatrix");
	tr.forwardLightingShader_DBS_post.u_SpecularTextureMatrix =
		glGetUniformLocationARB(tr.forwardLightingShader_DBS_post.program, "u_SpecularTextureMatrix");
	tr.forwardLightingShader_DBS_post.u_AlphaTest = glGetUniformLocationARB(tr.forwardLightingShader_DBS_post.program, "u_AlphaTest");
	tr.forwardLightingShader_DBS_post.u_ViewOrigin = glGetUniformLocationARB(tr.forwardLightingShader_DBS_post.program, "u_ViewOrigin");
	tr.forwardLightingShader_DBS_post.u_AmbientColor =
		glGetUniformLocationARB(tr.forwardLightingShader_DBS_post.program, "u_AmbientColor");
	tr.forwardLightingShader_DBS_post.u_ParallaxMapping = glGetUniformLocationARB(tr.forwardLightingShader_DBS_post.program, "u_ParallaxMapping");
	tr.forwardLightingShader_DBS_post.u_DepthScale = glGetUniformLocationARB(tr.forwardLightingShader_DBS_post.program, "u_DepthScale");
	tr.forwardLightingShader_DBS_post.u_ModelMatrix = glGetUniformLocationARB(tr.forwardLightingShader_DBS_post.program, "u_ModelMatrix");
	tr.forwardLightingShader_DBS_post.u_ModelViewMatrix =
		glGetUniformLocationARB(tr.forwardLightingShader_DBS_post.program, "u_ModelViewMatrix");
	tr.forwardLightingShader_DBS_post.u_ModelViewProjectionMatrix =
		glGetUniformLocationARB(tr.forwardLightingShader_DBS_post.program, "u_ModelViewProjectionMatrix");
	if(glConfig2.vboVertexSkinningAvailable)
	{
		tr.forwardLightingShader_DBS_post.u_VertexSkinning =
			glGetUniformLocationARB(tr.forwardLightingShader_DBS_post.program, "u_VertexSkinning");
		tr.forwardLightingShader_DBS_post.u_BoneMatrix =
			glGetUniformLocationARB(tr.forwardLightingShader_DBS_post.program, "u_BoneMatrix");
	}
	tr.forwardLightingShader_DBS_post.u_DeformGen =
		glGetUniformLocationARB(tr.forwardLightingShader_DBS_post.program, "u_DeformGen");
	tr.forwardLightingShader_DBS_post.u_DeformWave =
		glGetUniformLocationARB(tr.forwardLightingShader_DBS_post.program, "u_DeformWave");
	tr.forwardLightingShader_DBS_post.u_DeformBulge =
		glGetUniformLocationARB(tr.forwardLightingShader_DBS_post.program, "u_DeformBulge");
	tr.forwardLightingShader_DBS_post.u_DeformSpread =
		glGetUniformLocationARB(tr.forwardLightingShader_DBS_post.program, "u_DeformSpread");
	tr.forwardLightingShader_DBS_post.u_Time =
		glGetUniformLocationARB(tr.forwardLightingShader_DBS_post.program, "u_Time");

	glUseProgramObjectARB(tr.forwardLightingShader_DBS_post.program);
	glUniform1iARB(tr.forwardLightingShader_DBS_post.u_LightMap, 0);
	glUniform1iARB(tr.forwardLightingShader_DBS_post.u_DiffuseMap, 1);
	glUniform1iARB(tr.forwardLightingShader_DBS_post.u_NormalMap, 2);
	glUniform1iARB(tr.forwardLightingShader_DBS_post.u_SpecularMap, 3);
	glUseProgramObjectARB(0);

	GLSL_ValidateProgram(tr.forwardLightingShader_DBS_post.program);
	GLSL_ShowProgramUniforms(tr.forwardLightingShader_DBS_post.program);
	GL_CheckErrors();

#ifdef VOLUMETRIC_LIGHTING
	// volumetric lighting
	GLSL_InitGPUShader(&tr.lightVolumeShader_omni, "lightVolume_omni", ATTR_POSITION, qtrue);

	tr.lightVolumeShader_omni.u_DepthMap =
		glGetUniformLocationARB(tr.lightVolumeShader_omni.program, "u_DepthMap");
	tr.lightVolumeShader_omni.u_AttenuationMapXY =
		glGetUniformLocationARB(tr.lightVolumeShader_omni.program, "u_AttenuationMapXY");
	tr.lightVolumeShader_omni.u_AttenuationMapZ =
		glGetUniformLocationARB(tr.lightVolumeShader_omni.program, "u_AttenuationMapZ");
	tr.lightVolumeShader_omni.u_ShadowMap = glGetUniformLocationARB(tr.lightVolumeShader_omni.program, "u_ShadowMap");
	tr.lightVolumeShader_omni.u_ViewOrigin = glGetUniformLocationARB(tr.lightVolumeShader_omni.program, "u_ViewOrigin");
	tr.lightVolumeShader_omni.u_LightOrigin = glGetUniformLocationARB(tr.lightVolumeShader_omni.program, "u_LightOrigin");
	tr.lightVolumeShader_omni.u_LightColor = glGetUniformLocationARB(tr.lightVolumeShader_omni.program, "u_LightColor");
	tr.lightVolumeShader_omni.u_LightRadius = glGetUniformLocationARB(tr.lightVolumeShader_omni.program, "u_LightRadius");
	tr.lightVolumeShader_omni.u_LightScale = glGetUniformLocationARB(tr.lightVolumeShader_omni.program, "u_LightScale");
	tr.lightVolumeShader_omni.u_LightAttenuationMatrix =
		glGetUniformLocationARB(tr.lightVolumeShader_omni.program, "u_LightAttenuationMatrix");
	tr.lightVolumeShader_omni.u_ShadowCompare = glGetUniformLocationARB(tr.lightVolumeShader_omni.program, "u_ShadowCompare");
	tr.lightVolumeShader_omni.u_ModelViewProjectionMatrix =
		glGetUniformLocationARB(tr.lightVolumeShader_omni.program, "u_ModelViewProjectionMatrix");
	tr.lightVolumeShader_omni.u_UnprojectMatrix = glGetUniformLocationARB(tr.lightVolumeShader_omni.program, "u_UnprojectMatrix");

	glUseProgramObjectARB(tr.lightVolumeShader_omni.program);
	glUniform1iARB(tr.lightVolumeShader_omni.u_DepthMap, 0);
	glUniform1iARB(tr.lightVolumeShader_omni.u_AttenuationMapXY, 1);
	glUniform1iARB(tr.lightVolumeShader_omni.u_AttenuationMapZ, 2);
	glUniform1iARB(tr.lightVolumeShader_omni.u_ShadowMap, 3);
	glUseProgramObjectARB(0);

	GLSL_ValidateProgram(tr.lightVolumeShader_omni.program);
	GLSL_ShowProgramUniforms(tr.lightVolumeShader_omni.program);
	GL_CheckErrors();
#endif

	// UT3 style player shadowing
	GLSL_InitGPUShader(&tr.deferredShadowingShader_proj, "deferredShadowing_proj", ATTR_POSITION, qtrue, qtrue);

	tr.deferredShadowingShader_proj.u_DepthMap =
		glGetUniformLocationARB(tr.deferredShadowingShader_proj.program, "u_DepthMap");
	tr.deferredShadowingShader_proj.u_AttenuationMapXY =
		glGetUniformLocationARB(tr.deferredShadowingShader_proj.program, "u_AttenuationMapXY");
	tr.deferredShadowingShader_proj.u_AttenuationMapZ =
		glGetUniformLocationARB(tr.deferredShadowingShader_proj.program, "u_AttenuationMapZ");
	tr.deferredShadowingShader_proj.u_ShadowMap =
		glGetUniformLocationARB(tr.deferredShadowingShader_proj.program, "u_ShadowMap");
	tr.deferredShadowingShader_proj.u_LightOrigin =
		glGetUniformLocationARB(tr.deferredShadowingShader_proj.program, "u_LightOrigin");
	tr.deferredShadowingShader_proj.u_LightColor =
		glGetUniformLocationARB(tr.deferredShadowingShader_proj.program, "u_LightColor");
	tr.deferredShadowingShader_proj.u_LightRadius =
		glGetUniformLocationARB(tr.deferredShadowingShader_proj.program, "u_LightRadius");
	tr.deferredShadowingShader_proj.u_LightAttenuationMatrix =
		glGetUniformLocationARB(tr.deferredShadowingShader_proj.program, "u_LightAttenuationMatrix");
	tr.deferredShadowingShader_proj.u_ShadowMatrix =
		glGetUniformLocationARB(tr.deferredShadowingShader_proj.program, "u_ShadowMatrix");
	tr.deferredShadowingShader_proj.u_ShadowCompare =
		glGetUniformLocationARB(tr.deferredShadowingShader_proj.program, "u_ShadowCompare");
	tr.deferredShadowingShader_proj.u_PortalClipping =
		glGetUniformLocationARB(tr.deferredShadowingShader_proj.program, "u_PortalClipping");
	tr.deferredShadowingShader_proj.u_PortalPlane =
		glGetUniformLocationARB(tr.deferredShadowingShader_proj.program, "u_PortalPlane");
	tr.deferredShadowingShader_proj.u_ModelViewProjectionMatrix =
		glGetUniformLocationARB(tr.deferredShadowingShader_proj.program, "u_ModelViewProjectionMatrix");
	tr.deferredShadowingShader_proj.u_UnprojectMatrix =
		glGetUniformLocationARB(tr.deferredShadowingShader_proj.program, "u_UnprojectMatrix");

	glUseProgramObjectARB(tr.deferredShadowingShader_proj.program);
	glUniform1iARB(tr.deferredShadowingShader_proj.u_DepthMap, 0);
	glUniform1iARB(tr.deferredShadowingShader_proj.u_AttenuationMapXY, 1);
	glUniform1iARB(tr.deferredShadowingShader_proj.u_AttenuationMapZ, 2);
	glUniform1iARB(tr.deferredShadowingShader_proj.u_ShadowMap, 3);
	glUseProgramObjectARB(0);

	GLSL_ValidateProgram(tr.deferredShadowingShader_proj.program);
	GLSL_ShowProgramUniforms(tr.deferredShadowingShader_proj.program);
	GL_CheckErrors();

	// cubemap reflection for abitrary polygons
	GLSL_InitGPUShader(&tr.reflectionShader_C, "reflection_C", ATTR_POSITION | ATTR_NORMAL, qtrue, qtrue);

	tr.reflectionShader_C.u_ColorMap = glGetUniformLocationARB(tr.reflectionShader_C.program, "u_ColorMap");
	tr.reflectionShader_C.u_ViewOrigin = glGetUniformLocationARB(tr.reflectionShader_C.program, "u_ViewOrigin");
	tr.reflectionShader_C.u_ModelMatrix = glGetUniformLocationARB(tr.reflectionShader_C.program, "u_ModelMatrix");
	tr.reflectionShader_C.u_ModelViewProjectionMatrix =
		glGetUniformLocationARB(tr.reflectionShader_C.program, "u_ModelViewProjectionMatrix");
	if(glConfig2.vboVertexSkinningAvailable)
	{
		tr.reflectionShader_C.u_VertexSkinning = glGetUniformLocationARB(tr.reflectionShader_C.program, "u_VertexSkinning");
		tr.reflectionShader_C.u_BoneMatrix = glGetUniformLocationARB(tr.reflectionShader_C.program, "u_BoneMatrix");
	}

	glUseProgramObjectARB(tr.reflectionShader_C.program);
	glUniform1iARB(tr.reflectionShader_C.u_ColorMap, 0);
	glUseProgramObjectARB(0);

	GLSL_ValidateProgram(tr.reflectionShader_C.program);
	GLSL_ShowProgramUniforms(tr.reflectionShader_C.program);
	GL_CheckErrors();

	// bumped cubemap reflection for abitrary polygons ( EMBM )
	GLSL_InitGPUShader(&tr.reflectionShader_CB,
					   "reflection_CB", ATTR_POSITION | ATTR_TEXCOORD | ATTR_TANGENT | ATTR_BINORMAL | ATTR_NORMAL, qtrue, qtrue);

	tr.reflectionShader_CB.u_ColorMap = glGetUniformLocationARB(tr.reflectionShader_CB.program, "u_ColorMap");
	tr.reflectionShader_CB.u_NormalMap = glGetUniformLocationARB(tr.reflectionShader_CB.program, "u_NormalMap");
	tr.reflectionShader_CB.u_NormalTextureMatrix =
		glGetUniformLocationARB(tr.reflectionShader_CB.program, "u_NormalTextureMatrix");
	tr.reflectionShader_CB.u_ViewOrigin = glGetUniformLocationARB(tr.reflectionShader_CB.program, "u_ViewOrigin");
	tr.reflectionShader_CB.u_ModelMatrix = glGetUniformLocationARB(tr.reflectionShader_CB.program, "u_ModelMatrix");
	tr.reflectionShader_CB.u_ModelViewProjectionMatrix =
		glGetUniformLocationARB(tr.reflectionShader_CB.program, "u_ModelViewProjectionMatrix");
	if(glConfig2.vboVertexSkinningAvailable)
	{
		tr.reflectionShader_CB.u_VertexSkinning = glGetUniformLocationARB(tr.reflectionShader_CB.program, "u_VertexSkinning");
		tr.reflectionShader_CB.u_BoneMatrix = glGetUniformLocationARB(tr.reflectionShader_CB.program, "u_BoneMatrix");
	}

	glUseProgramObjectARB(tr.reflectionShader_CB.program);
	glUniform1iARB(tr.reflectionShader_CB.u_ColorMap, 0);
	glUniform1iARB(tr.reflectionShader_CB.u_NormalMap, 1);
	glUseProgramObjectARB(0);

	GLSL_ValidateProgram(tr.reflectionShader_CB.program);
	GLSL_ShowProgramUniforms(tr.reflectionShader_CB.program);
	GL_CheckErrors();

	// cubemap refraction for abitrary polygons
	GLSL_InitGPUShader(&tr.refractionShader_C, "refraction_C", ATTR_POSITION | ATTR_NORMAL, qtrue, qtrue);

	tr.refractionShader_C.u_ColorMap = glGetUniformLocationARB(tr.refractionShader_C.program, "u_ColorMap");
	tr.refractionShader_C.u_ViewOrigin = glGetUniformLocationARB(tr.refractionShader_C.program, "u_ViewOrigin");
	tr.refractionShader_C.u_RefractionIndex = glGetUniformLocationARB(tr.refractionShader_C.program, "u_RefractionIndex");
	tr.refractionShader_C.u_FresnelPower = glGetUniformLocationARB(tr.refractionShader_C.program, "u_FresnelPower");
	tr.refractionShader_C.u_FresnelScale = glGetUniformLocationARB(tr.refractionShader_C.program, "u_FresnelScale");
	tr.refractionShader_C.u_FresnelBias = glGetUniformLocationARB(tr.refractionShader_C.program, "u_FresnelBias");
	tr.refractionShader_C.u_ModelMatrix = glGetUniformLocationARB(tr.refractionShader_C.program, "u_ModelMatrix");
	tr.refractionShader_C.u_ModelViewProjectionMatrix =
		glGetUniformLocationARB(tr.refractionShader_C.program, "u_ModelViewProjectionMatrix");
	if(glConfig2.vboVertexSkinningAvailable)
	{
		tr.refractionShader_C.u_VertexSkinning = glGetUniformLocationARB(tr.refractionShader_C.program, "u_VertexSkinning");
		tr.refractionShader_C.u_BoneMatrix = glGetUniformLocationARB(tr.refractionShader_C.program, "u_BoneMatrix");
	}

	glUseProgramObjectARB(tr.refractionShader_C.program);
	glUniform1iARB(tr.refractionShader_C.u_ColorMap, 0);
	glUseProgramObjectARB(0);

	GLSL_ValidateProgram(tr.refractionShader_C.program);
	GLSL_ShowProgramUniforms(tr.refractionShader_C.program);
	GL_CheckErrors();

	// cubemap dispersion for abitrary polygons
	GLSL_InitGPUShader(&tr.dispersionShader_C, "dispersion_C", ATTR_POSITION | ATTR_NORMAL, qtrue, qtrue);

	tr.dispersionShader_C.u_ColorMap = glGetUniformLocationARB(tr.dispersionShader_C.program, "u_ColorMap");
	tr.dispersionShader_C.u_ViewOrigin = glGetUniformLocationARB(tr.dispersionShader_C.program, "u_ViewOrigin");
	tr.dispersionShader_C.u_EtaRatio = glGetUniformLocationARB(tr.dispersionShader_C.program, "u_EtaRatio");
	tr.dispersionShader_C.u_FresnelPower = glGetUniformLocationARB(tr.dispersionShader_C.program, "u_FresnelPower");
	tr.dispersionShader_C.u_FresnelScale = glGetUniformLocationARB(tr.dispersionShader_C.program, "u_FresnelScale");
	tr.dispersionShader_C.u_FresnelBias = glGetUniformLocationARB(tr.dispersionShader_C.program, "u_FresnelBias");
	tr.dispersionShader_C.u_ModelMatrix = glGetUniformLocationARB(tr.dispersionShader_C.program, "u_ModelMatrix");
	tr.dispersionShader_C.u_ModelViewProjectionMatrix =
		glGetUniformLocationARB(tr.dispersionShader_C.program, "u_ModelViewProjectionMatrix");
	if(glConfig2.vboVertexSkinningAvailable)
	{
		tr.dispersionShader_C.u_VertexSkinning = glGetUniformLocationARB(tr.dispersionShader_C.program, "u_VertexSkinning");
		tr.dispersionShader_C.u_BoneMatrix = glGetUniformLocationARB(tr.dispersionShader_C.program, "u_BoneMatrix");
	}

	glUseProgramObjectARB(tr.dispersionShader_C.program);
	glUniform1iARB(tr.dispersionShader_C.u_ColorMap, 0);
	glUseProgramObjectARB(0);

	GLSL_ValidateProgram(tr.dispersionShader_C.program);
	GLSL_ShowProgramUniforms(tr.dispersionShader_C.program);
	GL_CheckErrors();

	// skybox drawing for abitrary polygons
	GLSL_InitGPUShader(&tr.skyBoxShader, "skybox", ATTR_POSITION, qtrue, qtrue);

	tr.skyBoxShader.u_ColorMap = glGetUniformLocationARB(tr.skyBoxShader.program, "u_ColorMap");
	tr.skyBoxShader.u_ViewOrigin = glGetUniformLocationARB(tr.skyBoxShader.program, "u_ViewOrigin");
	tr.skyBoxShader.u_ModelMatrix = glGetUniformLocationARB(tr.skyBoxShader.program, "u_ModelMatrix");
	tr.skyBoxShader.u_ModelViewProjectionMatrix =
		glGetUniformLocationARB(tr.skyBoxShader.program, "u_ModelViewProjectionMatrix");

	glUseProgramObjectARB(tr.skyBoxShader.program);
	glUniform1iARB(tr.skyBoxShader.u_ColorMap, 0);
	glUseProgramObjectARB(0);

	GLSL_ValidateProgram(tr.skyBoxShader.program);
	GLSL_ShowProgramUniforms(tr.skyBoxShader.program);
	GL_CheckErrors();

	// heatHaze post process effect
	GLSL_InitGPUShader(&tr.heatHazeShader, "heatHaze", ATTR_POSITION | ATTR_TEXCOORD, qtrue, qtrue);

	tr.heatHazeShader.u_DeformMagnitude = glGetUniformLocationARB(tr.heatHazeShader.program, "u_DeformMagnitude");
	tr.heatHazeShader.u_NormalMap = glGetUniformLocationARB(tr.heatHazeShader.program, "u_NormalMap");
	tr.heatHazeShader.u_CurrentMap = glGetUniformLocationARB(tr.heatHazeShader.program, "u_CurrentMap");
	tr.heatHazeShader.u_NormalTextureMatrix = glGetUniformLocationARB(tr.heatHazeShader.program, "u_NormalTextureMatrix");
	if(r_heatHazeFix->integer && glConfig2.framebufferBlitAvailable && glConfig.hardwareType != GLHW_ATI && glConfig.hardwareType != GLHW_ATI_DX10 && glConfig.driverType != GLDRV_MESA)
	{
		tr.heatHazeShader.u_ContrastMap = glGetUniformLocationARB(tr.heatHazeShader.program, "u_ContrastMap");
	}
	tr.heatHazeShader.u_AlphaTest = glGetUniformLocationARB(tr.heatHazeShader.program, "u_AlphaTest");

	tr.heatHazeShader.u_ModelViewMatrixTranspose =
		glGetUniformLocationARB(tr.heatHazeShader.program, "u_ModelViewMatrixTranspose");
	tr.heatHazeShader.u_ProjectionMatrixTranspose =
		glGetUniformLocationARB(tr.heatHazeShader.program, "u_ProjectionMatrixTranspose");
	tr.heatHazeShader.u_ModelViewProjectionMatrix =
		glGetUniformLocationARB(tr.heatHazeShader.program, "u_ModelViewProjectionMatrix");

	if(glConfig2.vboVertexSkinningAvailable)
	{
		tr.heatHazeShader.u_VertexSkinning = glGetUniformLocationARB(tr.heatHazeShader.program, "u_VertexSkinning");
		tr.heatHazeShader.u_BoneMatrix = glGetUniformLocationARB(tr.heatHazeShader.program, "u_BoneMatrix");
	}

	glUseProgramObjectARB(tr.heatHazeShader.program);
	glUniform1iARB(tr.heatHazeShader.u_NormalMap, 0);
	glUniform1iARB(tr.heatHazeShader.u_CurrentMap, 1);
	if(r_heatHazeFix->integer && glConfig2.framebufferBlitAvailable && glConfig.hardwareType != GLHW_ATI && glConfig.hardwareType != GLHW_ATI_DX10 && glConfig.driverType != GLDRV_MESA)
	{
		glUniform1iARB(tr.heatHazeShader.u_ContrastMap, 2);
	}
	glUseProgramObjectARB(0);

	GLSL_ValidateProgram(tr.heatHazeShader.program);
	GLSL_ShowProgramUniforms(tr.heatHazeShader.program);
	GL_CheckErrors();

	// bloom post process effect
	GLSL_InitGPUShader(&tr.bloomShader, "bloom", ATTR_POSITION, qtrue, qfalse);

	tr.bloomShader.u_ColorMap = glGetUniformLocationARB(tr.bloomShader.program, "u_ColorMap");
	tr.bloomShader.u_ContrastMap = glGetUniformLocationARB(tr.bloomShader.program, "u_ContrastMap");
	tr.bloomShader.u_BlurMagnitude = glGetUniformLocationARB(tr.bloomShader.program, "u_BlurMagnitude");
	tr.bloomShader.u_ModelViewProjectionMatrix = glGetUniformLocationARB(tr.bloomShader.program, "u_ModelViewProjectionMatrix");

	glUseProgramObjectARB(tr.bloomShader.program);
	glUniform1iARB(tr.bloomShader.u_ColorMap, 0);
	glUniform1iARB(tr.bloomShader.u_ContrastMap, 1);
	glUseProgramObjectARB(0);

	GLSL_ValidateProgram(tr.bloomShader.program);
	GLSL_ShowProgramUniforms(tr.bloomShader.program);
	GL_CheckErrors();

	// contrast post process effect
	GLSL_InitGPUShader(&tr.contrastShader, "contrast", ATTR_POSITION, qtrue, qfalse);

	tr.contrastShader.u_ColorMap = glGetUniformLocationARB(tr.contrastShader.program, "u_ColorMap");
	if(r_hdrRendering->integer && glConfig2.framebufferObjectAvailable && glConfig2.textureFloatAvailable)
	{
		tr.contrastShader.u_HDRKey = glGetUniformLocationARB(tr.contrastShader.program, "u_HDRKey");
		tr.contrastShader.u_HDRAverageLuminance = glGetUniformLocationARB(tr.contrastShader.program, "u_HDRAverageLuminance");
		tr.contrastShader.u_HDRMaxLuminance = glGetUniformLocationARB(tr.contrastShader.program, "u_HDRMaxLuminance");
	}
	tr.contrastShader.u_ModelViewProjectionMatrix =
		glGetUniformLocationARB(tr.contrastShader.program, "u_ModelViewProjectionMatrix");

	glUseProgramObjectARB(tr.contrastShader.program);
	glUniform1iARB(tr.contrastShader.u_ColorMap, 0);
	glUseProgramObjectARB(0);

	GLSL_ValidateProgram(tr.contrastShader.program);
	GLSL_ShowProgramUniforms(tr.contrastShader.program);
	GL_CheckErrors();

	// blurX post process effect
	GLSL_InitGPUShader(&tr.blurXShader, "blurX", ATTR_POSITION, qtrue, qfalse);

	tr.blurXShader.u_ColorMap = glGetUniformLocationARB(tr.blurXShader.program, "u_ColorMap");
	tr.blurXShader.u_ModelViewProjectionMatrix = glGetUniformLocationARB(tr.blurXShader.program, "u_ModelViewProjectionMatrix");

	glUseProgramObjectARB(tr.blurXShader.program);
	glUniform1iARB(tr.blurXShader.u_ColorMap, 0);
	glUseProgramObjectARB(0);

	GLSL_ValidateProgram(tr.blurXShader.program);
	GLSL_ShowProgramUniforms(tr.blurXShader.program);
	GL_CheckErrors();

	// blurY post process effect
	GLSL_InitGPUShader(&tr.blurYShader, "blurY", ATTR_POSITION, qtrue, qfalse);

	tr.blurYShader.u_ColorMap = glGetUniformLocationARB(tr.blurYShader.program, "u_ColorMap");
	tr.blurYShader.u_ModelViewProjectionMatrix = glGetUniformLocationARB(tr.blurYShader.program, "u_ModelViewProjectionMatrix");

	glUseProgramObjectARB(tr.blurYShader.program);
	glUniform1iARB(tr.blurYShader.u_ColorMap, 0);
	glUseProgramObjectARB(0);

	GLSL_ValidateProgram(tr.blurYShader.program);
	GLSL_ShowProgramUniforms(tr.blurYShader.program);
	GL_CheckErrors();

	// rotoscope post process effect
	GLSL_InitGPUShader(&tr.rotoscopeShader, "rotoscope", ATTR_POSITION | ATTR_TEXCOORD, qtrue, qtrue);

	tr.rotoscopeShader.u_ColorMap = glGetUniformLocationARB(tr.rotoscopeShader.program, "u_ColorMap");
	tr.rotoscopeShader.u_BlurMagnitude = glGetUniformLocationARB(tr.rotoscopeShader.program, "u_BlurMagnitude");
	tr.rotoscopeShader.u_ModelViewProjectionMatrix = glGetUniformLocationARB(tr.rotoscopeShader.program, "u_ModelViewProjectionMatrix");

	glUseProgramObjectARB(tr.rotoscopeShader.program);
	glUniform1iARB(tr.rotoscopeShader.u_ColorMap, 0);
	glUseProgramObjectARB(0);

	GLSL_ValidateProgram(tr.rotoscopeShader.program);
	GLSL_ShowProgramUniforms(tr.rotoscopeShader.program);
	GL_CheckErrors();

	// camera post process effect
	GLSL_InitGPUShader(&tr.cameraEffectsShader, "cameraEffects", ATTR_POSITION | ATTR_TEXCOORD, qtrue, qtrue);

	tr.cameraEffectsShader.u_CurrentMap = glGetUniformLocationARB(tr.cameraEffectsShader.program, "u_CurrentMap");
	tr.cameraEffectsShader.u_GrainMap = glGetUniformLocationARB(tr.cameraEffectsShader.program, "u_GrainMap");
	tr.cameraEffectsShader.u_VignetteMap = glGetUniformLocationARB(tr.cameraEffectsShader.program, "u_VignetteMap");
	//tr.cameraEffectsShader.u_BlurMagnitude = glGetUniformLocationARB(tr.cameraEffectsShader.program, "u_BlurMagnitude");
	tr.cameraEffectsShader.u_ModelViewProjectionMatrix = glGetUniformLocationARB(tr.cameraEffectsShader.program, "u_ModelViewProjectionMatrix");
	tr.cameraEffectsShader.u_ColorTextureMatrix = glGetUniformLocationARB(tr.cameraEffectsShader.program, "u_ColorTextureMatrix");

	glUseProgramObjectARB(tr.cameraEffectsShader.program);
	glUniform1iARB(tr.cameraEffectsShader.u_CurrentMap, 0);
	glUniform1iARB(tr.cameraEffectsShader.u_GrainMap, 1);
	glUniform1iARB(tr.cameraEffectsShader.u_VignetteMap, 2);
	glUseProgramObjectARB(0);

	GLSL_ValidateProgram(tr.cameraEffectsShader.program);
	GLSL_ShowProgramUniforms(tr.cameraEffectsShader.program);
	GL_CheckErrors();

	// screen post process effect
	GLSL_InitGPUShader(&tr.screenShader, "screen", ATTR_POSITION | ATTR_COLOR, qtrue, qtrue);

	tr.screenShader.u_CurrentMap = glGetUniformLocationARB(tr.screenShader.program, "u_CurrentMap");
	tr.screenShader.u_ModelViewProjectionMatrix =
		glGetUniformLocationARB(tr.screenShader.program, "u_ModelViewProjectionMatrix");

	glUseProgramObjectARB(tr.screenShader.program);
	glUniform1iARB(tr.screenShader.u_CurrentMap, 0);
	glUseProgramObjectARB(0);

	GLSL_ValidateProgram(tr.screenShader.program);
	GLSL_ShowProgramUniforms(tr.screenShader.program);
	GL_CheckErrors();

	// portal process effect
	GLSL_InitGPUShader(&tr.portalShader, "portal", ATTR_POSITION | ATTR_COLOR, qtrue, qtrue);

	tr.portalShader.u_CurrentMap = glGetUniformLocationARB(tr.portalShader.program, "u_CurrentMap");
	tr.portalShader.u_PortalRange = glGetUniformLocationARB(tr.portalShader.program, "u_PortalRange");
	tr.portalShader.u_ModelViewMatrix = glGetUniformLocationARB(tr.portalShader.program, "u_ModelViewMatrix");
	tr.portalShader.u_ModelViewProjectionMatrix =
		glGetUniformLocationARB(tr.portalShader.program, "u_ModelViewProjectionMatrix");

	glUseProgramObjectARB(tr.portalShader.program);
	glUniform1iARB(tr.portalShader.u_CurrentMap, 0);
	glUseProgramObjectARB(0);

	GLSL_ValidateProgram(tr.portalShader.program);
	GLSL_ShowProgramUniforms(tr.portalShader.program);
	GL_CheckErrors();

	// liquid post process effect
	GLSL_InitGPUShader(&tr.liquidShader, "liquid",
			ATTR_POSITION | ATTR_TEXCOORD | ATTR_TANGENT | ATTR_BINORMAL | ATTR_NORMAL | ATTR_COLOR | ATTR_LIGHTDIRECTION, qtrue, qtrue);

	tr.liquidShader.u_CurrentMap = glGetUniformLocationARB(tr.liquidShader.program, "u_CurrentMap");
	tr.liquidShader.u_PortalMap = glGetUniformLocationARB(tr.liquidShader.program, "u_PortalMap");
	tr.liquidShader.u_DepthMap = glGetUniformLocationARB(tr.liquidShader.program, "u_DepthMap");
	tr.liquidShader.u_NormalMap = glGetUniformLocationARB(tr.liquidShader.program, "u_NormalMap");
	tr.liquidShader.u_NormalTextureMatrix = glGetUniformLocationARB(tr.liquidShader.program, "u_NormalTextureMatrix");
	tr.liquidShader.u_ViewOrigin = glGetUniformLocationARB(tr.liquidShader.program, "u_ViewOrigin");
	tr.liquidShader.u_RefractionIndex = glGetUniformLocationARB(tr.liquidShader.program, "u_RefractionIndex");
	tr.liquidShader.u_FresnelPower = glGetUniformLocationARB(tr.liquidShader.program, "u_FresnelPower");
	tr.liquidShader.u_FresnelScale = glGetUniformLocationARB(tr.liquidShader.program, "u_FresnelScale");
	tr.liquidShader.u_FresnelBias = glGetUniformLocationARB(tr.liquidShader.program, "u_FresnelBias");
	tr.liquidShader.u_NormalScale = glGetUniformLocationARB(tr.liquidShader.program, "u_NormalScale");
	tr.liquidShader.u_FogDensity = glGetUniformLocationARB(tr.liquidShader.program, "u_FogDensity");
	tr.liquidShader.u_FogColor = glGetUniformLocationARB(tr.liquidShader.program, "u_FogColor");
	tr.liquidShader.u_ModelMatrix = glGetUniformLocationARB(tr.liquidShader.program, "u_ModelMatrix");
	tr.liquidShader.u_ModelViewProjectionMatrix =
		glGetUniformLocationARB(tr.liquidShader.program, "u_ModelViewProjectionMatrix");
	tr.liquidShader.u_UnprojectMatrix = glGetUniformLocationARB(tr.liquidShader.program, "u_UnprojectMatrix");

	glUseProgramObjectARB(tr.liquidShader.program);
	glUniform1iARB(tr.liquidShader.u_CurrentMap, 0);
	glUniform1iARB(tr.liquidShader.u_PortalMap, 1);
	glUniform1iARB(tr.liquidShader.u_DepthMap, 2);
	glUniform1iARB(tr.liquidShader.u_NormalMap, 3);
	glUseProgramObjectARB(0);

	GLSL_ValidateProgram(tr.liquidShader.program);
	GLSL_ShowProgramUniforms(tr.liquidShader.program);
	GL_CheckErrors();

	// uniform fog post process effect
	GLSL_InitGPUShader(&tr.uniformFogShader, "uniformFog", ATTR_POSITION, qtrue, qtrue);

	tr.uniformFogShader.u_DepthMap = glGetUniformLocationARB(tr.uniformFogShader.program, "u_DepthMap");
	tr.uniformFogShader.u_ViewOrigin = glGetUniformLocationARB(tr.uniformFogShader.program, "u_ViewOrigin");
	tr.uniformFogShader.u_FogDensity = glGetUniformLocationARB(tr.uniformFogShader.program, "u_FogDensity");
	tr.uniformFogShader.u_FogColor = glGetUniformLocationARB(tr.uniformFogShader.program, "u_FogColor");
	tr.uniformFogShader.u_UnprojectMatrix = glGetUniformLocationARB(tr.uniformFogShader.program, "u_UnprojectMatrix");
	tr.uniformFogShader.u_ModelViewProjectionMatrix =
		glGetUniformLocationARB(tr.uniformFogShader.program, "u_ModelViewProjectionMatrix");

	glUseProgramObjectARB(tr.uniformFogShader.program);
	glUniform1iARB(tr.uniformFogShader.u_DepthMap, 0);
	glUseProgramObjectARB(0);

	GLSL_ValidateProgram(tr.uniformFogShader.program);
	GLSL_ShowProgramUniforms(tr.uniformFogShader.program);
	GL_CheckErrors();

	// volumetric fog post process effect
	GLSL_InitGPUShader(&tr.volumetricFogShader, "volumetricFog", ATTR_POSITION, qtrue, qtrue);

	tr.volumetricFogShader.u_DepthMap = glGetUniformLocationARB(tr.volumetricFogShader.program, "u_DepthMap");
	tr.volumetricFogShader.u_DepthMapBack = glGetUniformLocationARB(tr.volumetricFogShader.program, "u_DepthMapBack");
	tr.volumetricFogShader.u_DepthMapFront = glGetUniformLocationARB(tr.volumetricFogShader.program, "u_DepthMapFront");
	tr.volumetricFogShader.u_ViewOrigin = glGetUniformLocationARB(tr.volumetricFogShader.program, "u_ViewOrigin");
	tr.volumetricFogShader.u_FogDensity = glGetUniformLocationARB(tr.volumetricFogShader.program, "u_FogDensity");
	tr.volumetricFogShader.u_FogColor = glGetUniformLocationARB(tr.volumetricFogShader.program, "u_FogColor");
	tr.volumetricFogShader.u_UnprojectMatrix = glGetUniformLocationARB(tr.volumetricFogShader.program, "u_UnprojectMatrix");
	tr.volumetricFogShader.u_ModelViewProjectionMatrix =
		glGetUniformLocationARB(tr.volumetricFogShader.program, "u_ModelViewProjectionMatrix");

	glUseProgramObjectARB(tr.volumetricFogShader.program);
	glUniform1iARB(tr.volumetricFogShader.u_DepthMap, 0);
	glUniform1iARB(tr.volumetricFogShader.u_DepthMapBack, 1);
	glUniform1iARB(tr.volumetricFogShader.u_DepthMapFront, 2);
	glUseProgramObjectARB(0);

	GLSL_ValidateProgram(tr.volumetricFogShader.program);
	GLSL_ShowProgramUniforms(tr.volumetricFogShader.program);
	GL_CheckErrors();

#ifdef EXPERIMENTAL
	// screen space ambien occlusion post process effect
	GLSL_InitGPUShader(&tr.screenSpaceAmbientOcclusionShader, "screenSpaceAmbientOcclusion", ATTR_POSITION, qtrue, qtrue);

	tr.screenSpaceAmbientOcclusionShader.u_CurrentMap =
		glGetUniformLocationARB(tr.screenSpaceAmbientOcclusionShader.program, "u_CurrentMap");
	tr.screenSpaceAmbientOcclusionShader.u_DepthMap =
		glGetUniformLocationARB(tr.screenSpaceAmbientOcclusionShader.program, "u_DepthMap");
	tr.screenSpaceAmbientOcclusionShader.u_ModelViewProjectionMatrix =
		glGetUniformLocationARB(tr.screenSpaceAmbientOcclusionShader.program, "u_ModelViewProjectionMatrix");
	//tr.screenSpaceAmbientOcclusionShader.u_ViewOrigin = glGetUniformLocationARB(tr.screenSpaceAmbientOcclusionShader.program, "u_ViewOrigin");
	//tr.screenSpaceAmbientOcclusionShader.u_SSAOJitter = glGetUniformLocationARB(tr.screenSpaceAmbientOcclusionShader.program, "u_SSAOJitter");
	//tr.screenSpaceAmbientOcclusionShader.u_SSAORadius = glGetUniformLocationARB(tr.screenSpaceAmbientOcclusionShader.program, "u_SSAORadius");
	//tr.screenSpaceAmbientOcclusionShader.u_UnprojectMatrix = glGetUniformLocationARB(tr.screenSpaceAmbientOcclusionShader.program, "u_UnprojectMatrix");
	//tr.screenSpaceAmbientOcclusionShader.u_ProjectMatrix = glGetUniformLocationARB(tr.screenSpaceAmbientOcclusionShader.program, "u_ProjectMatrix");

	glUseProgramObjectARB(tr.screenSpaceAmbientOcclusionShader.program);
	glUniform1iARB(tr.screenSpaceAmbientOcclusionShader.u_CurrentMap, 0);
	glUniform1iARB(tr.screenSpaceAmbientOcclusionShader.u_DepthMap, 1);
	glUseProgramObjectARB(0);

	GLSL_ValidateProgram(tr.screenSpaceAmbientOcclusionShader.program);
	GLSL_ShowProgramUniforms(tr.screenSpaceAmbientOcclusionShader.program);
	GL_CheckErrors();
#endif
#ifdef EXPERIMENTAL
	// depth of field post process effect
	GLSL_InitGPUShader(&tr.depthOfFieldShader, "depthOfField", ATTR_POSITION, qtrue, qtrue);

	tr.depthOfFieldShader.u_CurrentMap = glGetUniformLocationARB(tr.depthOfFieldShader.program, "u_CurrentMap");
	tr.depthOfFieldShader.u_DepthMap = glGetUniformLocationARB(tr.depthOfFieldShader.program, "u_DepthMap");
	tr.depthOfFieldShader.u_ModelViewProjectionMatrix =
		glGetUniformLocationARB(tr.depthOfFieldShader.program, "u_ModelViewProjectionMatrix");

	glUseProgramObjectARB(tr.depthOfFieldShader.program);
	glUniform1iARB(tr.depthOfFieldShader.u_CurrentMap, 0);
	glUniform1iARB(tr.depthOfFieldShader.u_DepthMap, 1);
	glUseProgramObjectARB(0);

	GLSL_ValidateProgram(tr.depthOfFieldShader.program);
	GLSL_ShowProgramUniforms(tr.depthOfFieldShader.program);
	GL_CheckErrors();
#endif

	// HDR tone mapping post process effect
	GLSL_InitGPUShader(&tr.toneMappingShader, "toneMapping", ATTR_POSITION, qtrue, qfalse);

	tr.toneMappingShader.u_CurrentMap = glGetUniformLocationARB(tr.toneMappingShader.program, "u_CurrentMap");
	tr.toneMappingShader.u_HDRKey = glGetUniformLocationARB(tr.toneMappingShader.program, "u_HDRKey");
	tr.toneMappingShader.u_HDRAverageLuminance = glGetUniformLocationARB(tr.toneMappingShader.program, "u_HDRAverageLuminance");
	tr.toneMappingShader.u_HDRMaxLuminance = glGetUniformLocationARB(tr.toneMappingShader.program, "u_HDRMaxLuminance");
	tr.toneMappingShader.u_ModelViewProjectionMatrix =
		glGetUniformLocationARB(tr.toneMappingShader.program, "u_ModelViewProjectionMatrix");

	glUseProgramObjectARB(tr.toneMappingShader.program);
	glUniform1iARB(tr.toneMappingShader.u_CurrentMap, 0);
	glUseProgramObjectARB(0);

	GLSL_ValidateProgram(tr.toneMappingShader.program);
	GLSL_ShowProgramUniforms(tr.toneMappingShader.program);
	GL_CheckErrors();

	// debugUtils
	GLSL_InitGPUShader(&tr.debugShadowMapShader, "debugShadowMap", ATTR_POSITION | ATTR_TEXCOORD, qtrue, qtrue);

	tr.debugShadowMapShader.u_ShadowMap = glGetUniformLocationARB(tr.debugShadowMapShader.program, "u_ShadowMap");
	tr.debugShadowMapShader.u_ModelViewProjectionMatrix =
		glGetUniformLocationARB(tr.debugShadowMapShader.program, "u_ModelViewProjectionMatrix");

	glUseProgramObjectARB(tr.debugShadowMapShader.program);
	glUniform1iARB(tr.debugShadowMapShader.u_ShadowMap, 0);
	glUseProgramObjectARB(0);

	GLSL_ValidateProgram(tr.debugShadowMapShader.program);
	GLSL_ShowProgramUniforms(tr.debugShadowMapShader.program);
	GL_CheckErrors();

	endTime = ri.Milliseconds();

#if defined(USE_GLSL_OPTIMIZER)
	glslopt_cleanup(s_glslOptimizer);
#endif

	ri.Printf(PRINT_ALL, "GLSL shaders load time = %5.2f seconds\n", (endTime - startTime) / 1000.0);
}

void GLSL_ShutdownGPUShaders(void)
{
//	int				i;

	ri.Printf(PRINT_ALL, "------- GLSL_ShutdownGPUShaders -------\n");

	if(tr.genericShader.program)
	{
		glDeleteObjectARB(tr.genericShader.program);
		Com_Memset(&tr.genericShader, 0, sizeof(shaderProgram_t));
	}

	if(gl_vertexLightingShader_DBS_entity)
	{
		delete gl_vertexLightingShader_DBS_entity;
		gl_vertexLightingShader_DBS_entity = NULL;
	}

	if(tr.vertexLightingShader_DBS_world.program)
	{
		glDeleteObjectARB(tr.vertexLightingShader_DBS_world.program);
		Com_Memset(&tr.vertexLightingShader_DBS_world, 0, sizeof(shaderProgram_t));
	}

	if(gl_lightMappingShader)
	{
		delete gl_lightMappingShader;
		gl_lightMappingShader = NULL;
	}

	if(tr.deluxeMappingShader.program)
	{
		glDeleteObjectARB(tr.deluxeMappingShader.program);
		Com_Memset(&tr.deluxeMappingShader, 0, sizeof(shaderProgram_t));
	}

	if(tr.geometricFillShader_DBS.program)
	{
		glDeleteObjectARB(tr.geometricFillShader_DBS.program);
		Com_Memset(&tr.geometricFillShader_DBS, 0, sizeof(shaderProgram_t));
	}

	if(tr.deferredLightingShader_DBS_omni.program)
	{
		glDeleteObjectARB(tr.deferredLightingShader_DBS_omni.program);
		Com_Memset(&tr.deferredLightingShader_DBS_omni, 0, sizeof(shaderProgram_t));
	}

	if(tr.deferredLightingShader_DBS_proj.program)
	{
		glDeleteObjectARB(tr.deferredLightingShader_DBS_proj.program);
		Com_Memset(&tr.deferredLightingShader_DBS_proj, 0, sizeof(shaderProgram_t));
	}

	if(tr.deferredLightingShader_DBS_directional.program)
	{
		glDeleteObjectARB(tr.deferredLightingShader_DBS_directional.program);
		Com_Memset(&tr.deferredLightingShader_DBS_directional, 0, sizeof(shaderProgram_t));
	}

	if(tr.depthFillShader.program)
	{
		glDeleteObjectARB(tr.depthFillShader.program);
		Com_Memset(&tr.depthFillShader, 0, sizeof(shaderProgram_t));
	}

	if(tr.depthTestShader.program)
	{
		glDeleteObjectARB(tr.depthTestShader.program);
		Com_Memset(&tr.depthTestShader, 0, sizeof(shaderProgram_t));
	}

	if(tr.depthToColorShader.program)
	{
		glDeleteObjectARB(tr.depthToColorShader.program);
		Com_Memset(&tr.depthToColorShader, 0, sizeof(shaderProgram_t));
	}

	if(tr.shadowExtrudeShader.program)
	{
		glDeleteObjectARB(tr.shadowExtrudeShader.program);
		Com_Memset(&tr.shadowExtrudeShader, 0, sizeof(shaderProgram_t));
	}

	if(tr.shadowFillShader.program)
	{
		glDeleteObjectARB(tr.shadowFillShader.program);
		Com_Memset(&tr.shadowFillShader, 0, sizeof(shaderProgram_t));
	}

	if(tr.forwardLightingShader_DBS_omni.program)
	{
		glDeleteObjectARB(tr.forwardLightingShader_DBS_omni.program);
		Com_Memset(&tr.forwardLightingShader_DBS_omni, 0, sizeof(shaderProgram_t));
	}

	if(tr.forwardLightingShader_DBS_proj.program)
	{
		glDeleteObjectARB(tr.forwardLightingShader_DBS_proj.program);
		Com_Memset(&tr.forwardLightingShader_DBS_proj, 0, sizeof(shaderProgram_t));
	}

	if(tr.forwardLightingShader_DBS_directional.program)
	{
		glDeleteObjectARB(tr.forwardLightingShader_DBS_directional.program);
		Com_Memset(&tr.forwardLightingShader_DBS_directional, 0, sizeof(shaderProgram_t));
	}

	if(tr.forwardLightingShader_DBS_post.program)
	{
		glDeleteObjectARB(tr.forwardLightingShader_DBS_post.program);
		Com_Memset(&tr.forwardLightingShader_DBS_post, 0, sizeof(shaderProgram_t));
	}

#ifdef VOLUMETRIC_LIGHTING
	if(tr.lightVolumeShader_omni.program)
	{
		glDeleteObjectARB(tr.lightVolumeShader_omni.program);
		Com_Memset(&tr.lightVolumeShader_omni, 0, sizeof(shaderProgram_t));
	}
#endif

	if(tr.deferredShadowingShader_proj.program)
	{
		glDeleteObjectARB(tr.deferredShadowingShader_proj.program);
		Com_Memset(&tr.deferredShadowingShader_proj, 0, sizeof(shaderProgram_t));
	}

	if(tr.reflectionShader_C.program)
	{
		glDeleteObjectARB(tr.reflectionShader_C.program);
		Com_Memset(&tr.reflectionShader_C, 0, sizeof(shaderProgram_t));
	}

	if(tr.reflectionShader_CB.program)
	{
		glDeleteObjectARB(tr.reflectionShader_CB.program);
		Com_Memset(&tr.reflectionShader_CB, 0, sizeof(shaderProgram_t));
	}

	if(tr.refractionShader_C.program)
	{
		glDeleteObjectARB(tr.refractionShader_C.program);
		Com_Memset(&tr.refractionShader_C, 0, sizeof(shaderProgram_t));
	}

	if(tr.dispersionShader_C.program)
	{
		glDeleteObjectARB(tr.dispersionShader_C.program);
		Com_Memset(&tr.dispersionShader_C, 0, sizeof(shaderProgram_t));
	}

	if(tr.skyBoxShader.program)
	{
		glDeleteObjectARB(tr.skyBoxShader.program);
		Com_Memset(&tr.skyBoxShader, 0, sizeof(shaderProgram_t));
	}

	if(tr.heatHazeShader.program)
	{
		glDeleteObjectARB(tr.heatHazeShader.program);
		Com_Memset(&tr.heatHazeShader, 0, sizeof(shaderProgram_t));
	}

	if(tr.bloomShader.program)
	{
		glDeleteObjectARB(tr.bloomShader.program);
		Com_Memset(&tr.bloomShader, 0, sizeof(shaderProgram_t));
	}

	if(tr.contrastShader.program)
	{
		glDeleteObjectARB(tr.contrastShader.program);
		Com_Memset(&tr.contrastShader, 0, sizeof(shaderProgram_t));
	}

	if(tr.blurXShader.program)
	{
		glDeleteObjectARB(tr.blurXShader.program);
		Com_Memset(&tr.blurXShader, 0, sizeof(shaderProgram_t));
	}

	if(tr.blurYShader.program)
	{
		glDeleteObjectARB(tr.blurYShader.program);
		Com_Memset(&tr.blurYShader, 0, sizeof(shaderProgram_t));
	}

	if(tr.rotoscopeShader.program)
	{
		glDeleteObjectARB(tr.rotoscopeShader.program);
		Com_Memset(&tr.rotoscopeShader, 0, sizeof(shaderProgram_t));
	}

	if(tr.cameraEffectsShader.program)
	{
		glDeleteObjectARB(tr.cameraEffectsShader.program);
		Com_Memset(&tr.cameraEffectsShader, 0, sizeof(shaderProgram_t));
	}

	if(tr.screenShader.program)
	{
		glDeleteObjectARB(tr.screenShader.program);
		Com_Memset(&tr.screenShader, 0, sizeof(shaderProgram_t));
	}

	if(tr.portalShader.program)
	{
		glDeleteObjectARB(tr.portalShader.program);
		Com_Memset(&tr.portalShader, 0, sizeof(shaderProgram_t));
	}

	if(tr.liquidShader.program)
	{
		glDeleteObjectARB(tr.liquidShader.program);
		Com_Memset(&tr.liquidShader, 0, sizeof(shaderProgram_t));
	}

	if(tr.uniformFogShader.program)
	{
		glDeleteObjectARB(tr.uniformFogShader.program);
		Com_Memset(&tr.uniformFogShader, 0, sizeof(shaderProgram_t));
	}

	if(tr.volumetricFogShader.program)
	{
		glDeleteObjectARB(tr.volumetricFogShader.program);
		Com_Memset(&tr.volumetricFogShader, 0, sizeof(shaderProgram_t));
	}
#ifdef EXPERIMENTAL
	if(tr.screenSpaceAmbientOcclusionShader.program)
	{
		glDeleteObjectARB(tr.screenSpaceAmbientOcclusionShader.program);
		Com_Memset(&tr.screenSpaceAmbientOcclusionShader, 0, sizeof(shaderProgram_t));
	}
#endif
#ifdef EXPERIMENTAL
	if(tr.depthOfFieldShader.program)
	{
		glDeleteObjectARB(tr.depthOfFieldShader.program);
		Com_Memset(&tr.depthOfFieldShader, 0, sizeof(shaderProgram_t));
	}
#endif
	if(tr.toneMappingShader.program)
	{
		glDeleteObjectARB(tr.toneMappingShader.program);
		Com_Memset(&tr.toneMappingShader, 0, sizeof(shaderProgram_t));
	}

	if(tr.debugShadowMapShader.program)
	{
		glDeleteObjectARB(tr.debugShadowMapShader.program);
		Com_Memset(&tr.debugShadowMapShader, 0, sizeof(shaderProgram_t));
	}

	glState.currentProgram = 0;
	glUseProgramObjectARB(0);
}

/*
static void MyMultiDrawElements(GLenum mode, const GLsizei *count, GLenum type, const void* *indices, GLsizei primcount)
{
	int			i;

	for (i = 0; i < primcount; i++)
	{
		if (count[i] > 0)
			glDrawElements(mode, count[i], type, indices[i]);
	}
}
*/

/*
==================
Tess_DrawElements
==================
*/
void Tess_DrawElements()
{
	int			i;

	if((tess.numIndexes == 0 || tess.numVertexes == 0) && tess.multiDrawPrimitives == 0)
	{
		return;
	}

	// move tess data through the GPU, finally
	if(glState.currentVBO && glState.currentIBO)
	{
		if(tess.multiDrawPrimitives)
		{
			glMultiDrawElements(GL_TRIANGLES, tess.multiDrawCounts, GL_INDEX_TYPE, (const GLvoid**) tess.multiDrawIndexes, tess.multiDrawPrimitives);

			backEnd.pc.c_multiDrawElements++;
			backEnd.pc.c_multiDrawPrimitives += tess.multiDrawPrimitives;

			for(i = 0; i < tess.multiDrawPrimitives; i++)
			{
				//backEnd.pc.c_vboVertexes += tess.numVertexes;
				backEnd.pc.c_multiVboIndexes += tess.multiDrawCounts[i];
				backEnd.pc.c_indexes += tess.multiDrawCounts[i];
			}
		}
		else
		{
			glDrawElements(GL_TRIANGLES, tess.numIndexes, GL_INDEX_TYPE, BUFFER_OFFSET(0));

			backEnd.pc.c_drawElements++;

			backEnd.pc.c_vboVertexes += tess.numVertexes;
			backEnd.pc.c_vboIndexes += tess.numIndexes;

			backEnd.pc.c_indexes += tess.numIndexes;
			backEnd.pc.c_vertexes += tess.numVertexes;
		}
	}
	else
	{
		glDrawElements(GL_TRIANGLES, tess.numIndexes, GL_INDEX_TYPE, tess.indexes);

		backEnd.pc.c_drawElements++;

		backEnd.pc.c_indexes += tess.numIndexes;
		backEnd.pc.c_vertexes += tess.numVertexes;
	}

	// update performance counters
	
	
}


/*
=============================================================

SURFACE SHADERS

=============================================================
*/

shaderCommands_t tess;





/*
=================
BindLightMap
=================
*/
static void BindLightMap()
{
	image_t        *lightmap;

	if(tess.lightmapNum >= 0 && tess.lightmapNum < tr.lightmaps.currentElements)
	{
#if defined(COMPAT_Q3A)
		lightmap = tr.fatLightmap;
#else
		lightmap = (image_t *) Com_GrowListElement(&tr.lightmaps, tess.lightmapNum);
#endif
	}
	else
	{
		lightmap = NULL;
	}

	if(!tr.lightmaps.currentElements || !lightmap)
	{
		GL_Bind(tr.whiteImage);
		return;
	}

	GL_Bind(lightmap);
}

/*
=================
BindDeluxeMap
=================
*/
static void BindDeluxeMap()
{
	image_t        *deluxemap;

	if(tess.lightmapNum >= 0 && tess.lightmapNum < tr.deluxemaps.currentElements)
	{
		deluxemap = (image_t *) Com_GrowListElement(&tr.deluxemaps, tess.lightmapNum);
	}
	else
	{
		deluxemap = NULL;
	}

	if(!tr.deluxemaps.currentElements || !deluxemap)
	{
		GL_Bind(tr.flatImage);
		return;
	}

	GL_Bind(deluxemap);
}


/*
================
DrawTris

Draws triangle outlines for debugging
================
*/
static void DrawTris()
{
	GLimp_LogComment("--- DrawTris ---\n");

	GL_BindProgram(&tr.genericShader);
	GL_State(GLS_POLYMODE_LINE | GLS_DEPTHMASK_TRUE);
	GL_VertexAttribsState(tr.genericShader.attribs);

	if(r_showBatches->integer || r_showLightBatches->integer)
	{
		GLSL_SetUniform_Color(&tr.genericShader, g_color_table[backEnd.pc.c_batches % 8]);
	}
	else if(glState.currentVBO == tess.vbo)
	{
		GLSL_SetUniform_Color(&tr.genericShader, colorRed);
	}
	else if(glState.currentVBO)
	{
		GLSL_SetUniform_Color(&tr.genericShader, colorBlue);
	}
	else
	{
		GLSL_SetUniform_Color(&tr.genericShader, colorWhite);
	}

	GLSL_SetUniform_TCGen_Environment(&tr.genericShader,  qfalse);
	GLSL_SetUniform_ColorGen(&tr.genericShader, CGEN_CONST);
	GLSL_SetUniform_AlphaGen(&tr.genericShader, AGEN_CONST);

	GLSL_SetUniform_ModelMatrix(&tr.genericShader, backEnd.orientation.transformMatrix);
	GLSL_SetUniform_ModelViewProjectionMatrix(&tr.genericShader, glState.modelViewProjectionMatrix[glState.stackIndex]);

	if(glConfig2.vboVertexSkinningAvailable)
	{
		GLSL_SetUniform_VertexSkinning(&tr.genericShader, tess.vboVertexSkinning);

		if(tess.vboVertexSkinning)
			glUniformMatrix4fvARB(tr.genericShader.u_BoneMatrix, MAX_BONES, GL_FALSE, &tess.boneMatrices[0][0]);
	}

	// u_DeformGen
	if(tess.surfaceShader->numDeforms)
	{
		deformStage_t  *ds;

		// only support the first one
		ds = &tess.surfaceShader->deforms[0];

		switch (ds->deformation)
		{
			case DEFORM_WAVE:
				GLSL_SetUniform_DeformGen(&tr.genericShader, (deformGen_t) ds->deformationWave.func);
				GLSL_SetUniform_DeformWave(&tr.genericShader, &ds->deformationWave);
				GLSL_SetUniform_DeformSpread(&tr.genericShader, ds->deformationSpread);
				GLSL_SetUniform_Time(&tr.genericShader, backEnd.refdef.floatTime);
				break;

			case DEFORM_BULGE:
				GLSL_SetUniform_DeformGen(&tr.genericShader, DGEN_BULGE);
				GLSL_SetUniform_DeformBulge(&tr.genericShader, ds);
				GLSL_SetUniform_Time(&tr.genericShader, backEnd.refdef.floatTime);
				break;

			default:
				GLSL_SetUniform_DeformGen(&tr.genericShader, DGEN_NONE);
				break;
		}
	}
	else
	{
		GLSL_SetUniform_DeformGen(&tr.genericShader, DGEN_NONE);
	}

	GLSL_SetUniform_AlphaTest(&tr.genericShader, 0);

	// bind u_ColorMap
	GL_SelectTexture(0);
	GL_Bind(tr.whiteImage);
	GLSL_SetUniform_ColorTextureMatrix(&tr.genericShader, tess.svars.texMatrices[TB_COLORMAP]);

	glDepthRange(0, 0);

	Tess_DrawElements();

	glDepthRange(0, 1);
}





/*
==============
Tess_Begin

We must set some things up before beginning any tesselation,
because a surface may be forced to perform a Tess_End due
to overflow.
==============
*/
// *INDENT-OFF*
void Tess_Begin(	 void (*stageIteratorFunc)(),
					 shader_t * surfaceShader, shader_t * lightShader,
					 qboolean skipTangentSpaces,
					 qboolean shadowVolume,
					 int lightmapNum)
{
	shader_t       *state;

	tess.numIndexes = 0;
	tess.numVertexes = 0;

	tess.multiDrawPrimitives = 0;
	
	// materials are optional
	if(surfaceShader != NULL)
	{
		state = (surfaceShader->remappedShader) ? surfaceShader->remappedShader : surfaceShader;
	
		tess.surfaceShader = state;
		tess.surfaceStages = state->stages;
		tess.numSurfaceStages = state->numStages;
	}
	else
	{
		state = NULL;

		tess.numSurfaceStages = 0;
		tess.surfaceShader = NULL;
		tess.surfaceStages = NULL;
	}

	tess.lightShader = lightShader;

	tess.stageIteratorFunc = stageIteratorFunc;
	tess.stageIteratorFunc2 = NULL;

	if(!tess.stageIteratorFunc)
	{
		tess.stageIteratorFunc = Tess_StageIteratorGeneric;
	}

	if(tess.stageIteratorFunc == Tess_StageIteratorGeneric)
	{
		if(state->isSky)
		{
			tess.stageIteratorFunc = Tess_StageIteratorSky;
			tess.stageIteratorFunc2 = Tess_StageIteratorGeneric;
		}
	}

	if(tess.stageIteratorFunc == Tess_StageIteratorGBuffer)
	{
		if(state && state->isSky)
		{
			tess.stageIteratorFunc = Tess_StageIteratorSky;
			tess.stageIteratorFunc2 = Tess_StageIteratorGBuffer;
		}
	}


	tess.skipTangentSpaces = skipTangentSpaces;
	tess.shadowVolume = shadowVolume;
	tess.lightmapNum = lightmapNum;

	if(r_logFile->integer)
	{
		// don't just call LogComment, or we will get
		// a call to va() every frame!
		GLimp_LogComment(va("--- Tess_Begin( surfaceShader = %s, lightShader = %s, skipTangentSpaces = %i, shadowVolume = %i, lightmap = %i ) ---\n", tess.surfaceShader->name, tess.lightShader ? tess.lightShader->name : NULL, tess.skipTangentSpaces, tess.shadowVolume, tess.lightmapNum));
	}
}
// *INDENT-ON*

static void Render_genericSingle(int stage)
{
	shaderStage_t  *pStage;
	uint32_t 		attribBits = ATTR_POSITION | ATTR_TEXCOORD;

	GLimp_LogComment("--- Render_genericSingle ---\n");

	pStage = tess.surfaceStages[stage];

	GL_State(pStage->stateBits);
	GL_BindProgram(&tr.genericShader);

	// set uniforms
	GLSL_SetUniform_TCGen_Environment(&tr.genericShader, pStage->tcGen_Environment);
	if(pStage->tcGen_Environment)
	{
		// calculate the environment texcoords in object space
		GLSL_SetUniform_ViewOrigin(&tr.genericShader, backEnd.orientation.viewOrigin);

		attribBits |= ATTR_NORMAL;
	}

	// u_ColorGen
	switch (pStage->rgbGen)
	{
		case CGEN_VERTEX:
		case CGEN_ONE_MINUS_VERTEX:
			GLSL_SetUniform_ColorGen(&tr.genericShader, pStage->rgbGen);
			attribBits |= ATTR_COLOR;
			break;

		default:
			GLSL_SetUniform_ColorGen(&tr.genericShader, CGEN_CONST);
			break;
	}

	// u_AlphaGen
	switch (pStage->alphaGen)
	{
		case AGEN_VERTEX:
		case AGEN_ONE_MINUS_VERTEX:
			GLSL_SetUniform_AlphaGen(&tr.genericShader, pStage->alphaGen);
			attribBits |= ATTR_COLOR;
			break;

		default:
			GLSL_SetUniform_AlphaGen(&tr.genericShader, AGEN_CONST);
			break;
	}

	// u_Color
	GLSL_SetUniform_Color(&tr.genericShader, tess.svars.color);

	GLSL_SetUniform_ModelMatrix(&tr.genericShader, backEnd.orientation.transformMatrix);
	GLSL_SetUniform_ModelViewProjectionMatrix(&tr.genericShader, glState.modelViewProjectionMatrix[glState.stackIndex]);

	if(glConfig2.vboVertexSkinningAvailable)
	{
		GLSL_SetUniform_VertexSkinning(&tr.genericShader, tess.vboVertexSkinning);

		if(tess.vboVertexSkinning)
			glUniformMatrix4fvARB(tr.genericShader.u_BoneMatrix, MAX_BONES, GL_FALSE, &tess.boneMatrices[0][0]);
	}

	// u_DeformGen
	if(tess.surfaceShader->numDeforms)
	{
		deformStage_t  *ds;

		// only support the first one
		ds = &tess.surfaceShader->deforms[0];

		switch (ds->deformation)
		{
			case DEFORM_WAVE:
				GLSL_SetUniform_DeformGen(&tr.genericShader, (deformGen_t) ds->deformationWave.func);
				GLSL_SetUniform_DeformWave(&tr.genericShader, &ds->deformationWave);
				GLSL_SetUniform_DeformSpread(&tr.genericShader, ds->deformationSpread);
				GLSL_SetUniform_Time(&tr.genericShader, backEnd.refdef.floatTime);
				attribBits |= ATTR_NORMAL;
				break;

			case DEFORM_BULGE:
				GLSL_SetUniform_DeformGen(&tr.genericShader, DGEN_BULGE);
				GLSL_SetUniform_DeformBulge(&tr.genericShader, ds);
				GLSL_SetUniform_Time(&tr.genericShader, backEnd.refdef.floatTime);
				attribBits |= ATTR_NORMAL;
				break;

			default:
				GLSL_SetUniform_DeformGen(&tr.genericShader, DGEN_NONE);
				break;
		}
	}
	else
	{
		GLSL_SetUniform_DeformGen(&tr.genericShader, DGEN_NONE);
	}

	GLSL_SetUniform_AlphaTest(&tr.genericShader, pStage->stateBits);

	GLSL_SetUniform_PortalClipping(&tr.genericShader, backEnd.viewParms.isPortal);
	if(backEnd.viewParms.isPortal)
	{
		float           plane[4];

		// clipping plane in world space
		plane[0] = backEnd.viewParms.portalPlane.normal[0];
		plane[1] = backEnd.viewParms.portalPlane.normal[1];
		plane[2] = backEnd.viewParms.portalPlane.normal[2];
		plane[3] = backEnd.viewParms.portalPlane.dist;

		GLSL_SetUniform_PortalPlane(&tr.genericShader, plane);
	}

	// bind u_ColorMap
	GL_SelectTexture(0);
	BindAnimatedImage(&pStage->bundle[TB_COLORMAP]);
	GLSL_SetUniform_ColorTextureMatrix(&tr.genericShader, tess.svars.texMatrices[TB_COLORMAP]);

	GL_VertexAttribsState(attribBits);

	Tess_DrawElements();

	GL_CheckErrors();
}

static void Render_vertexLighting_DBS_entity(int stage)
{
	vec3_t          viewOrigin;
	vec3_t          ambientColor;
	vec3_t          lightDir;
	vec4_t          lightColor;
	uint32_t 		attribBits = ATTR_POSITION | ATTR_TEXCOORD | ATTR_NORMAL;
	uint32_t		stateBits;
	shaderStage_t  *pStage = tess.surfaceStages[stage];

	GLimp_LogComment("--- Render_vertexLighting_DBS_entity ---\n");

	stateBits = pStage->stateBits;

	if(DS_PREPASS_LIGHTING_ENABLED())
	{
		stateBits &= ~(GLS_SRCBLEND_BITS | GLS_DSTBLEND_BITS | GLS_ATEST_BITS);
		stateBits |= (GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE);
	}

	GL_State(stateBits);

	// choose right shader program ----------------------------------
	if(backEnd.viewParms.isPortal)
	{
		gl_vertexLightingShader_DBS_entity->EnablePortalClipping();
	}
	else
	{
		gl_vertexLightingShader_DBS_entity->DisablePortalClipping();
	}

	if(pStage->stateBits & GLS_ATEST_BITS)
	{
		gl_vertexLightingShader_DBS_entity->EnableAlphaTesting();
	}
	else
	{
		gl_vertexLightingShader_DBS_entity->DisableAlphaTesting();
	}

	if(glConfig2.vboVertexSkinningAvailable && tess.vboVertexSkinning)
	{
		gl_vertexLightingShader_DBS_entity->EnableVertexSkinning();
	}
	else
	{
		gl_vertexLightingShader_DBS_entity->DisableVertexSkinning();
	}

	if(glState.vertexAttribsInterpolation > 0)
	{
		gl_vertexLightingShader_DBS_entity->EnableVertexAnimation();
	}
	else
	{
		gl_vertexLightingShader_DBS_entity->DisableVertexAnimation();
	}

	if(tess.surfaceShader->numDeforms)
	{
		gl_vertexLightingShader_DBS_entity->EnableDeformVertexes();
	}
	else
	{
		gl_vertexLightingShader_DBS_entity->DisableDeformVertexes();
	}

	if(r_parallaxMapping->integer && tess.surfaceShader->parallax)
	{
		gl_vertexLightingShader_DBS_entity->EnableParallaxMapping();
	}
	else
	{
		gl_vertexLightingShader_DBS_entity->DisableParallaxMapping();
	}

	gl_vertexLightingShader_DBS_entity->BindProgram();
	
	// end choose right shader program ------------------------------

	// now we are ready to set the shader program uniforms


	if(glConfig2.vboVertexSkinningAvailable && tess.vboVertexSkinning)
	{
		gl_vertexLightingShader_DBS_entity->SetUniform_BoneMatrix(MAX_BONES, tess.boneMatrices);

		attribBits |= (ATTR_BONE_INDEXES | ATTR_BONE_WEIGHTS);
	}

	// set uniforms
	VectorCopy(backEnd.viewParms.orientation.origin, viewOrigin);	// in world space
	VectorCopy(backEnd.currentEntity->ambientLight, ambientColor);
	//ClampColor(ambientColor);
	VectorCopy(backEnd.currentEntity->directedLight, lightColor);
	//ClampColor(directedLight);

	// lightDir = L vector which means surface to light
	VectorCopy(backEnd.currentEntity->lightDir, lightDir);

	// u_AlphaTest
	gl_vertexLightingShader_DBS_entity->SetUniform_AlphaTest(pStage->stateBits);

	gl_vertexLightingShader_DBS_entity->SetUniform_AmbientColor(ambientColor);
	gl_vertexLightingShader_DBS_entity->SetUniform_ViewOrigin(viewOrigin);
	gl_vertexLightingShader_DBS_entity->SetUniform_LightDir(lightDir);
	gl_vertexLightingShader_DBS_entity->SetUniform_LightColor(lightColor);

	gl_vertexLightingShader_DBS_entity->SetUniform_ModelMatrix(backEnd.orientation.transformMatrix);
	gl_vertexLightingShader_DBS_entity->SetUniform_ModelViewProjectionMatrix(glState.modelViewProjectionMatrix[glState.stackIndex]);

	// u_VertexInterpolation
	if(glState.vertexAttribsInterpolation > 0)
	{
		gl_vertexLightingShader_DBS_entity->SetUniform_VertexInterpolation(glState.vertexAttribsInterpolation);

		attribBits |= (ATTR_POSITION2 | ATTR_NORMAL2);

		if(r_normalMapping->integer)
		{
			attribBits |=  (ATTR_TANGENT2 | ATTR_BINORMAL2);
		}
	}

	// u_DeformGen
	if(tess.surfaceShader->numDeforms)
	{
		deformStage_t  *ds;

		// only support the first one
		ds = &tess.surfaceShader->deforms[0];

		gl_vertexLightingShader_DBS_entity->SetDeformStageUniforms(ds);
	}

	if(r_parallaxMapping->integer && tess.surfaceShader->parallax)
	{
		float           depthScale;

		depthScale = RB_EvalExpression(&pStage->depthScaleExp, r_parallaxDepthScale->value);
		gl_vertexLightingShader_DBS_entity->SetUniform_DepthScale(depthScale);
	}

	if(backEnd.viewParms.isPortal)
	{
		float           plane[4];

		// clipping plane in world space
		plane[0] = backEnd.viewParms.portalPlane.normal[0];
		plane[1] = backEnd.viewParms.portalPlane.normal[1];
		plane[2] = backEnd.viewParms.portalPlane.normal[2];
		plane[3] = backEnd.viewParms.portalPlane.dist;

		gl_vertexLightingShader_DBS_entity->SetUniform_PortalPlane(plane);
	}

	// bind u_DiffuseMap
	GL_SelectTexture(0);
	GL_Bind(pStage->bundle[TB_DIFFUSEMAP].image[0]);
	GLSL_SetUniform_DiffuseTextureMatrix(gl_vertexLightingShader_DBS_entity->GetProgram(), tess.svars.texMatrices[TB_DIFFUSEMAP]);

	if(r_normalMapping->integer)
	{
		attribBits |= ATTR_TANGENT | ATTR_BINORMAL;

		// bind u_NormalMap
		GL_SelectTexture(1);
		if(pStage->bundle[TB_NORMALMAP].image[0])
		{
			GL_Bind(pStage->bundle[TB_NORMALMAP].image[0]);
		}
		else
		{
			GL_Bind(tr.flatImage);
		}
		GLSL_SetUniform_NormalTextureMatrix(gl_vertexLightingShader_DBS_entity->GetProgram(), tess.svars.texMatrices[TB_NORMALMAP]);

		// bind u_SpecularMap
		GL_SelectTexture(2);
		if(pStage->bundle[TB_SPECULARMAP].image[0])
		{
			GL_Bind(pStage->bundle[TB_SPECULARMAP].image[0]);
		}
		else
		{
			GL_Bind(tr.blackImage);
		}
		GLSL_SetUniform_SpecularTextureMatrix(gl_vertexLightingShader_DBS_entity->GetProgram(), tess.svars.texMatrices[TB_SPECULARMAP]);
	}

	GL_VertexAttribsState(attribBits);

	Tess_DrawElements();

	GL_CheckErrors();
}

static void Render_vertexLighting_DBS_world(int stage)
{
	vec3_t          viewOrigin;
	uint32_t		stateBits;
	shaderStage_t  *pStage = tess.surfaceStages[stage];

	GLimp_LogComment("--- Render_vertexLighting_DBS_world ---\n");

	stateBits = pStage->stateBits;

	if(DS_PREPASS_LIGHTING_ENABLED())
	{
		stateBits &= ~(GLS_SRCBLEND_BITS | GLS_DSTBLEND_BITS | GLS_ATEST_BITS);
		stateBits |= (GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE);
	}

	GL_State(stateBits);

	// enable shader, set arrays
	GL_BindProgram(&tr.vertexLightingShader_DBS_world);
	GL_VertexAttribsState(tr.vertexLightingShader_DBS_world.attribs);

	// set uniforms
	VectorCopy(backEnd.orientation.viewOrigin, viewOrigin);

	GL_CheckErrors();

	// u_DeformGen
	if(tess.surfaceShader->numDeforms)
	{
		deformStage_t  *ds;

		// only support the first one
		ds = &tess.surfaceShader->deforms[0];

		switch (ds->deformation)
		{
			case DEFORM_WAVE:
				GLSL_SetUniform_DeformGen(&tr.vertexLightingShader_DBS_world, (deformGen_t) ds->deformationWave.func);
				GLSL_SetUniform_DeformWave(&tr.vertexLightingShader_DBS_world, &ds->deformationWave);
				GLSL_SetUniform_DeformSpread(&tr.vertexLightingShader_DBS_world, ds->deformationSpread);
				GLSL_SetUniform_Time(&tr.vertexLightingShader_DBS_world, backEnd.refdef.floatTime);
				break;

			case DEFORM_BULGE:
				GLSL_SetUniform_DeformGen(&tr.vertexLightingShader_DBS_world, DGEN_BULGE);
				GLSL_SetUniform_DeformBulge(&tr.vertexLightingShader_DBS_world, ds);
				GLSL_SetUniform_Time(&tr.vertexLightingShader_DBS_world, backEnd.refdef.floatTime);
				break;

			default:
				GLSL_SetUniform_DeformGen(&tr.vertexLightingShader_DBS_world, DGEN_NONE);
				break;
		}
	}
	else
	{
		GLSL_SetUniform_DeformGen(&tr.vertexLightingShader_DBS_world, DGEN_NONE);
	}

#if defined(COMPAT_ET)
	// u_ColorGen
	switch (pStage->rgbGen)
	{
		case CGEN_VERTEX:
		case CGEN_ONE_MINUS_VERTEX:
			GLSL_SetUniform_ColorGen(&tr.vertexLightingShader_DBS_world, pStage->rgbGen);
			break;

		default:
			GLSL_SetUniform_ColorGen(&tr.vertexLightingShader_DBS_world, CGEN_CONST);
			break;
	}

	// u_AlphaGen
	switch (pStage->alphaGen)
	{
		case AGEN_VERTEX:
		case AGEN_ONE_MINUS_VERTEX:
			GLSL_SetUniform_AlphaGen(&tr.vertexLightingShader_DBS_world, pStage->alphaGen);
			break;

		default:
			GLSL_SetUniform_AlphaGen(&tr.vertexLightingShader_DBS_world, AGEN_CONST);
			break;
	}

#else
	// u_ColorGen
	switch (pStage->rgbGen)
	{
		//case CGEN_IDENTITY_LIGHTING:
		//case CGEN_IDENTITY:
		//case CGEN_ENTITY:
		case CGEN_WAVEFORM:
		case CGEN_CONST:
		case CGEN_CUSTOM_RGB:
		case CGEN_CUSTOM_RGBs:
			GLSL_SetUniform_ColorGen(&tr.vertexLightingShader_DBS_world, CGEN_CONST);
			break;

		case CGEN_ONE_MINUS_VERTEX:
			GLSL_SetUniform_ColorGen(&tr.vertexLightingShader_DBS_world, CGEN_ONE_MINUS_VERTEX);
			break;

		default:
			GLSL_SetUniform_ColorGen(&tr.vertexLightingShader_DBS_world, CGEN_VERTEX);
			break;
	}

	// u_AlphaGen
	switch (pStage->alphaGen)
	{
		//case AGEN_IDENTITY:
		//case AGEN_ENTITY:
		//case AGEN_ONE_MINUS_ENTITY:
		case AGEN_WAVEFORM:
		case AGEN_CONST:
		case AGEN_CUSTOM:
			GLSL_SetUniform_AlphaGen(&tr.vertexLightingShader_DBS_world, AGEN_CONST);
			break;

		case AGEN_ONE_MINUS_VERTEX:
			GLSL_SetUniform_AlphaGen(&tr.vertexLightingShader_DBS_world, AGEN_ONE_MINUS_VERTEX);
			break;

		default:
			GLSL_SetUniform_AlphaGen(&tr.vertexLightingShader_DBS_world, AGEN_VERTEX);
			break;
	}
#endif

	// u_Color
	GLSL_SetUniform_Color(&tr.vertexLightingShader_DBS_world, tess.svars.color);

	GLSL_SetUniform_LightWrapAround(&tr.vertexLightingShader_DBS_world, RB_EvalExpression(&pStage->wrapAroundLightingExp, 0));

//	glUniform1iARB(tr.vertexLightingShader_DBS_world.u_InverseVertexColor, pStage->inverseVertexColor);
	GLSL_SetUniform_ViewOrigin(&tr.vertexLightingShader_DBS_world, viewOrigin);
	GLSL_SetUniform_ModelViewProjectionMatrix(&tr.vertexLightingShader_DBS_world, glState.modelViewProjectionMatrix[glState.stackIndex]);

	GLSL_SetUniform_AlphaTest(&tr.vertexLightingShader_DBS_world, pStage->stateBits);

	if(r_parallaxMapping->integer)
	{
		float           depthScale;

		GLSL_SetUniform_ParallaxMapping(&tr.vertexLightingShader_DBS_world, tess.surfaceShader->parallax);

		depthScale = RB_EvalExpression(&pStage->depthScaleExp, r_parallaxDepthScale->value);
		GLSL_SetUniform_DepthScale(&tr.vertexLightingShader_DBS_world, depthScale);
	}

	GLSL_SetUniform_PortalClipping(&tr.vertexLightingShader_DBS_world, backEnd.viewParms.isPortal);
	if(backEnd.viewParms.isPortal)
	{
		float           plane[4];

		// clipping plane in world space
		plane[0] = backEnd.viewParms.portalPlane.normal[0];
		plane[1] = backEnd.viewParms.portalPlane.normal[1];
		plane[2] = backEnd.viewParms.portalPlane.normal[2];
		plane[3] = backEnd.viewParms.portalPlane.dist;

		GLSL_SetUniform_PortalPlane(&tr.vertexLightingShader_DBS_world, plane);
	}

	// bind u_DiffuseMap
	GL_SelectTexture(0);
	GL_Bind(pStage->bundle[TB_DIFFUSEMAP].image[0]);
	GLSL_SetUniform_DiffuseTextureMatrix(&tr.vertexLightingShader_DBS_world, tess.svars.texMatrices[TB_DIFFUSEMAP]);

	if(r_normalMapping->integer)
	{
		// bind u_NormalMap
		GL_SelectTexture(1);
		if(pStage->bundle[TB_NORMALMAP].image[0])
		{
			GL_Bind(pStage->bundle[TB_NORMALMAP].image[0]);
		}
		else
		{
			GL_Bind(tr.flatImage);
		}
		GLSL_SetUniform_NormalTextureMatrix(&tr.vertexLightingShader_DBS_world, tess.svars.texMatrices[TB_NORMALMAP]);

		// bind u_SpecularMap
		GL_SelectTexture(2);
		if(pStage->bundle[TB_SPECULARMAP].image[0])
		{
			GL_Bind(pStage->bundle[TB_SPECULARMAP].image[0]);
		}
		else
		{
			GL_Bind(tr.blackImage);
		}
		GLSL_SetUniform_SpecularTextureMatrix(&tr.vertexLightingShader_DBS_world, tess.svars.texMatrices[TB_SPECULARMAP]);
	}

	Tess_DrawElements();

	GL_CheckErrors();
}

static void Render_lightMapping(int stage, qboolean asColorMap)
{
	shaderStage_t  *pStage;
	uint32_t	    stateBits;

	GLimp_LogComment("--- Render_lightMapping ---\n");

	pStage = tess.surfaceStages[stage];

	stateBits = pStage->stateBits;

	if(DS_PREPASS_LIGHTING_ENABLED())
	{
		stateBits &= ~(GLS_SRCBLEND_BITS | GLS_DSTBLEND_BITS | GLS_ATEST_BITS);
		stateBits |= (GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE);
	}

	if(r_showLightMaps->integer)
	{
		stateBits &= ~(GLS_SRCBLEND_BITS | GLS_DSTBLEND_BITS | GLS_ATEST_BITS);
	}

	GL_State(stateBits);

	
	// choose right shader program ----------------------------------
	//if(backEnd.viewParms.isPortal)
	//{
	//	gl_lightMappingShader->EnablePortalClipping();
	//}
	//else
	//{
		gl_lightMappingShader->DisablePortalClipping();
	//}

	if(pStage->stateBits & GLS_ATEST_BITS)
	{
		gl_lightMappingShader->EnableAlphaTesting();
	}
	else
	{
		gl_lightMappingShader->DisableAlphaTesting();
	}

	if(tess.surfaceShader->numDeforms)
	{
		gl_lightMappingShader->EnableDeformVertexes();
	}
	else
	{
		gl_lightMappingShader->DisableDeformVertexes();
	}

	gl_lightMappingShader->BindProgram();
	
	// end choose right shader program ------------------------------

	// now we are ready to set the shader program uniforms

	GL_VertexAttribsState(gl_lightMappingShader->GetProgram()->attribs);

	// u_DeformGen
	if(tess.surfaceShader->numDeforms)
	{
		deformStage_t  *ds;

		// only support the first one
		ds = &tess.surfaceShader->deforms[0];

		gl_lightMappingShader->SetDeformStageUniforms(ds);
	}

	gl_lightMappingShader->SetUniform_ModelViewProjectionMatrix(glState.modelViewProjectionMatrix[glState.stackIndex]);
	gl_lightMappingShader->SetUniform_AlphaTest(pStage->stateBits);

	// bind u_DiffuseMap
	GL_SelectTexture(0);
	if(asColorMap)
	{
		GL_Bind(tr.whiteImage);
	}
	else
	{
		GL_Bind(pStage->bundle[TB_DIFFUSEMAP].image[0]);
		gl_lightMappingShader->SetUniform_DiffuseTextureMatrix(tess.svars.texMatrices[TB_DIFFUSEMAP]);
	}

	// bind u_LightMap
	GL_SelectTexture(1);
	BindLightMap();

	Tess_DrawElements();

	GL_CheckErrors();
}

static void Render_deluxeMapping(int stage)
{
	vec3_t          viewOrigin;
	shaderStage_t  *pStage;
	uint32_t     	stateBits;

	GLimp_LogComment("--- Render_deluxeMapping ---\n");

	pStage = tess.surfaceStages[stage];

	stateBits = pStage->stateBits;

	if(DS_PREPASS_LIGHTING_ENABLED())
	{
		stateBits &= ~(GLS_SRCBLEND_BITS | GLS_DSTBLEND_BITS | GLS_ATEST_BITS);
		stateBits |= (GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE);
	}

	GL_State(stateBits);

	// enable shader, set arrays
	GL_BindProgram(&tr.deluxeMappingShader);
	GL_VertexAttribsState(tr.deluxeMappingShader.attribs);

	// set uniforms
	VectorCopy(backEnd.viewParms.orientation.origin, viewOrigin);	// in world space

	GLSL_SetUniform_ViewOrigin(&tr.deluxeMappingShader, viewOrigin);

	GLSL_SetUniform_ModelMatrix(&tr.deluxeMappingShader, backEnd.orientation.transformMatrix);
	GLSL_SetUniform_ModelViewProjectionMatrix(&tr.deluxeMappingShader, glState.modelViewProjectionMatrix[glState.stackIndex]);

	GLSL_SetUniform_AlphaTest(&tr.deluxeMappingShader, pStage->stateBits);

	// u_DeformGen
	if(tess.surfaceShader->numDeforms)
	{
		deformStage_t  *ds;

		// only support the first one
		ds = &tess.surfaceShader->deforms[0];

		switch (ds->deformation)
		{
			case DEFORM_WAVE:
				GLSL_SetUniform_DeformGen(&tr.deluxeMappingShader, (deformGen_t) ds->deformationWave.func);
				GLSL_SetUniform_DeformWave(&tr.deluxeMappingShader, &ds->deformationWave);
				GLSL_SetUniform_DeformSpread(&tr.deluxeMappingShader, ds->deformationSpread);
				GLSL_SetUniform_Time(&tr.deluxeMappingShader, backEnd.refdef.floatTime);
				break;

			case DEFORM_BULGE:
				GLSL_SetUniform_DeformGen(&tr.deluxeMappingShader, DGEN_BULGE);
				GLSL_SetUniform_DeformBulge(&tr.deluxeMappingShader, ds);
				GLSL_SetUniform_Time(&tr.deluxeMappingShader, backEnd.refdef.floatTime);
				break;

			default:
				GLSL_SetUniform_DeformGen(&tr.deluxeMappingShader, DGEN_NONE);
				break;
		}
	}
	else
	{
		GLSL_SetUniform_DeformGen(&tr.deluxeMappingShader, DGEN_NONE);
	}

	if(r_parallaxMapping->integer)
	{
		float           depthScale;

		GLSL_SetUniform_ParallaxMapping(&tr.deluxeMappingShader, tess.surfaceShader->parallax);

		depthScale = RB_EvalExpression(&pStage->depthScaleExp, r_parallaxDepthScale->value);
		GLSL_SetUniform_DepthScale(&tr.deluxeMappingShader, depthScale);
	}

	GLSL_SetUniform_PortalClipping(&tr.deluxeMappingShader, backEnd.viewParms.isPortal);
	if(backEnd.viewParms.isPortal)
	{
		float           plane[4];

		// clipping plane in world space
		plane[0] = backEnd.viewParms.portalPlane.normal[0];
		plane[1] = backEnd.viewParms.portalPlane.normal[1];
		plane[2] = backEnd.viewParms.portalPlane.normal[2];
		plane[3] = backEnd.viewParms.portalPlane.dist;

		GLSL_SetUniform_PortalPlane(&tr.deluxeMappingShader, plane);
	}

	// bind u_DiffuseMap
	GL_SelectTexture(0);
	GL_Bind(pStage->bundle[TB_DIFFUSEMAP].image[0]);
	GLSL_SetUniform_DiffuseTextureMatrix(&tr.deluxeMappingShader, tess.svars.texMatrices[TB_DIFFUSEMAP]);

	// bind u_NormalMap
	GL_SelectTexture(1);
	if(pStage->bundle[TB_NORMALMAP].image[0])
	{
		GL_Bind(pStage->bundle[TB_NORMALMAP].image[0]);
	}
	else
	{
		GL_Bind(tr.flatImage);
	}
	GLSL_SetUniform_NormalTextureMatrix(&tr.deluxeMappingShader, tess.svars.texMatrices[TB_NORMALMAP]);

	// bind u_SpecularMap
	GL_SelectTexture(2);
	if(pStage->bundle[TB_SPECULARMAP].image[0])
	{
		GL_Bind(pStage->bundle[TB_SPECULARMAP].image[0]);
	}
	else
	{
		GL_Bind(tr.blackImage);
	}
	GLSL_SetUniform_SpecularTextureMatrix(&tr.deluxeMappingShader, tess.svars.texMatrices[TB_SPECULARMAP]);

	// bind u_LightMap
	GL_SelectTexture(3);
	BindLightMap();

	// bind u_DeluxeMap
	GL_SelectTexture(4);
	BindDeluxeMap();

	Tess_DrawElements();

	GL_CheckErrors();
}

static void Render_forwardLighting_DBS_post(int stage, qboolean cmap2black)
{
	shaderStage_t  *pStage;
	uint32_t        stateBits;
	vec3_t          viewOrigin;
	vec4_t          ambientColor;

	GLimp_LogComment("--- Render_forwardLighting_DBS_post ---\n");

	pStage = tess.surfaceStages[stage];

	// remove blend mode
	stateBits = pStage->stateBits;
	stateBits &= ~(GLS_SRCBLEND_BITS | GLS_DSTBLEND_BITS | GLS_ATEST_BITS);
	stateBits |= (GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE);

	GL_State(stateBits);

	// enable shader, set arrays
	GL_BindProgram(&tr.forwardLightingShader_DBS_post);
	GL_VertexAttribsState(tr.forwardLightingShader_DBS_post.attribs);

	// set uniforms
	VectorCopy(backEnd.viewParms.orientation.origin, viewOrigin);	// in world space

#if 0
	if(r_precomputedLighting->integer)
	{
		VectorCopy(backEnd.currentEntity->ambientLight, ambientColor);
		ClampColor(ambientColor);
	}
	else if(r_forceAmbient->integer)
	{
		ambientColor[0] = r_forceAmbient->value;
		ambientColor[1] = r_forceAmbient->value;
		ambientColor[2] = r_forceAmbient->value;
	}
	else
#endif
	{
		VectorClear(ambientColor);
	}

	GLSL_SetUniform_AlphaTest(&tr.forwardLightingShader_DBS_post, pStage->stateBits);
	GLSL_SetUniform_ViewOrigin(&tr.forwardLightingShader_DBS_post, viewOrigin);
	GLSL_SetUniform_AmbientColor(&tr.forwardLightingShader_DBS_post, ambientColor);

	GLSL_SetUniform_ModelMatrix(&tr.forwardLightingShader_DBS_post, backEnd.orientation.transformMatrix);
	GLSL_SetUniform_ModelViewMatrix(&tr.forwardLightingShader_DBS_post, glState.modelViewMatrix[glState.stackIndex]);
	GLSL_SetUniform_ModelViewProjectionMatrix(&tr.forwardLightingShader_DBS_post, glState.modelViewProjectionMatrix[glState.stackIndex]);

	if(glConfig2.vboVertexSkinningAvailable)
	{
		GLSL_SetUniform_VertexSkinning(&tr.forwardLightingShader_DBS_post, tess.vboVertexSkinning);

		if(tess.vboVertexSkinning)
			glUniformMatrix4fvARB(tr.forwardLightingShader_DBS_post.u_BoneMatrix, MAX_BONES, GL_FALSE, &tess.boneMatrices[0][0]);
	}

	// u_DeformGen
	if(tess.surfaceShader->numDeforms)
	{
		deformStage_t  *ds;

		// only support the first one
		ds = &tess.surfaceShader->deforms[0];

		switch (ds->deformation)
		{
			case DEFORM_WAVE:
				GLSL_SetUniform_DeformGen(&tr.forwardLightingShader_DBS_post, (deformGen_t) ds->deformationWave.func);
				GLSL_SetUniform_DeformWave(&tr.forwardLightingShader_DBS_post, &ds->deformationWave);
				GLSL_SetUniform_DeformSpread(&tr.forwardLightingShader_DBS_post, ds->deformationSpread);
				GLSL_SetUniform_Time(&tr.forwardLightingShader_DBS_post, backEnd.refdef.floatTime);
				break;

			case DEFORM_BULGE:
				GLSL_SetUniform_DeformGen(&tr.forwardLightingShader_DBS_post, DGEN_BULGE);
				GLSL_SetUniform_DeformBulge(&tr.forwardLightingShader_DBS_post, ds);
				GLSL_SetUniform_Time(&tr.forwardLightingShader_DBS_post, backEnd.refdef.floatTime);
				break;

			default:
				GLSL_SetUniform_DeformGen(&tr.forwardLightingShader_DBS_post, DGEN_NONE);
				break;
		}
	}
	else
	{
		GLSL_SetUniform_DeformGen(&tr.forwardLightingShader_DBS_post, DGEN_NONE);
	}

	if(r_parallaxMapping->integer)
	{
		float           depthScale;

		GLSL_SetUniform_ParallaxMapping(&tr.forwardLightingShader_DBS_post, tess.surfaceShader->parallax);

		depthScale = RB_EvalExpression(&pStage->depthScaleExp, r_parallaxDepthScale->value);
		GLSL_SetUniform_DepthScale(&tr.forwardLightingShader_DBS_post, depthScale);
	}

	// bind u_LightMap
	GL_SelectTexture(0);
	GL_Bind(tr.lightRenderFBOImage);

	// bind u_DiffuseMap
	GL_SelectTexture(1);
	if(cmap2black)
	{
		GL_Bind(tr.blackImage);
	}
	else
	{
		GL_Bind(pStage->bundle[TB_DIFFUSEMAP].image[0]);
	}
	GLSL_SetUniform_DiffuseTextureMatrix(&tr.forwardLightingShader_DBS_post, tess.svars.texMatrices[TB_DIFFUSEMAP]);

	if(r_normalMapping->integer)
	{
		// bind u_NormalMap
		GL_SelectTexture(2);
		if(pStage->bundle[TB_NORMALMAP].image[0])
		{
			GL_Bind(pStage->bundle[TB_NORMALMAP].image[0]);
		}
		else
		{
			GL_Bind(tr.flatImage);
		}
		GLSL_SetUniform_NormalTextureMatrix(&tr.forwardLightingShader_DBS_post, tess.svars.texMatrices[TB_NORMALMAP]);


		// bind u_SpecularMap
		GL_SelectTexture(3);
		if(r_forceSpecular->integer)
		{
			GL_Bind(pStage->bundle[TB_DIFFUSEMAP].image[0]);
		}
		else if(pStage->bundle[TB_SPECULARMAP].image[0])
		{
			GL_Bind(pStage->bundle[TB_SPECULARMAP].image[0]);
		}
		else
		{
			GL_Bind(tr.blackImage);
		}
		GLSL_SetUniform_SpecularTextureMatrix(&tr.forwardLightingShader_DBS_post, tess.svars.texMatrices[TB_SPECULARMAP]);
	}

	Tess_DrawElements();

	GL_CheckErrors();
}

static void Render_geometricFill_DBS(int stage, qboolean cmap2black)
{
	shaderStage_t  *pStage;
	uint32_t        stateBits;
	vec3_t          viewOrigin;
	vec4_t          ambientColor;

	GLimp_LogComment("--- Render_geometricFill_DBS ---\n");

	pStage = tess.surfaceStages[stage];

	// remove blend mode
	stateBits = pStage->stateBits;
	stateBits &= ~(GLS_SRCBLEND_BITS | GLS_DSTBLEND_BITS | GLS_ATEST_BITS);

	GL_State(stateBits);

	// enable shader, set arrays
	GL_BindProgram(&tr.geometricFillShader_DBS);
	GL_VertexAttribsState(tr.geometricFillShader_DBS.attribs);

	// set uniforms
	VectorCopy(backEnd.viewParms.orientation.origin, viewOrigin);	// in world space

	if(DS_PREPASS_LIGHTING_ENABLED())
	{
		VectorClear(ambientColor);
	}
	else
	{
		if(r_precomputedLighting->integer)
		{
			VectorCopy(backEnd.currentEntity->ambientLight, ambientColor);
			ClampColor(ambientColor);
		}
		else if(r_forceAmbient->integer)
		{
			ambientColor[0] = r_forceAmbient->value;
			ambientColor[1] = r_forceAmbient->value;
			ambientColor[2] = r_forceAmbient->value;
		}
		else
		{
			VectorClear(ambientColor);
		}
	}

	GLSL_SetUniform_AlphaTest(&tr.geometricFillShader_DBS, pStage->stateBits);
	GLSL_SetUniform_ViewOrigin(&tr.geometricFillShader_DBS, viewOrigin);
	GLSL_SetUniform_AmbientColor(&tr.geometricFillShader_DBS, ambientColor);

	GLSL_SetUniform_ModelMatrix(&tr.geometricFillShader_DBS, backEnd.orientation.transformMatrix);
	GLSL_SetUniform_ModelViewMatrix(&tr.geometricFillShader_DBS, glState.modelViewMatrix[glState.stackIndex]);
	GLSL_SetUniform_ModelViewProjectionMatrix(&tr.geometricFillShader_DBS, glState.modelViewProjectionMatrix[glState.stackIndex]);

	if(glConfig2.vboVertexSkinningAvailable)
	{
		GLSL_SetUniform_VertexSkinning(&tr.geometricFillShader_DBS, tess.vboVertexSkinning);

		if(tess.vboVertexSkinning)
			glUniformMatrix4fvARB(tr.geometricFillShader_DBS.u_BoneMatrix, MAX_BONES, GL_FALSE, &tess.boneMatrices[0][0]);
	}

	if(tess.surfaceShader->numDeforms)
	{
		deformStage_t  *ds;

		// only support the first one
		ds = &tess.surfaceShader->deforms[0];

		switch (ds->deformation)
		{
			case DEFORM_WAVE:
				GLSL_SetUniform_DeformGen(&tr.geometricFillShader_DBS, (deformGen_t) ds->deformationWave.func);
				GLSL_SetUniform_DeformWave(&tr.geometricFillShader_DBS, &ds->deformationWave);
				GLSL_SetUniform_DeformSpread(&tr.geometricFillShader_DBS, ds->deformationSpread);
				GLSL_SetUniform_Time(&tr.geometricFillShader_DBS, backEnd.refdef.floatTime);
				break;

			case DEFORM_BULGE:
				GLSL_SetUniform_DeformGen(&tr.geometricFillShader_DBS, DGEN_BULGE);
				GLSL_SetUniform_DeformBulge(&tr.geometricFillShader_DBS, ds);
				GLSL_SetUniform_Time(&tr.geometricFillShader_DBS, backEnd.refdef.floatTime);
				break;

			default:
				GLSL_SetUniform_DeformGen(&tr.geometricFillShader_DBS, DGEN_NONE);
				break;
		}
	}
	else
	{
		GLSL_SetUniform_DeformGen(&tr.geometricFillShader_DBS, DGEN_NONE);
	}

	if(r_parallaxMapping->integer)
	{
		float           depthScale;

		GLSL_SetUniform_ParallaxMapping(&tr.geometricFillShader_DBS, tess.surfaceShader->parallax);

		depthScale = RB_EvalExpression(&pStage->depthScaleExp, r_parallaxDepthScale->value);
		GLSL_SetUniform_DepthScale(&tr.geometricFillShader_DBS, depthScale);
	}

	// bind u_DiffuseMap
	GL_SelectTexture(0);
	if(cmap2black)
	{
		GL_Bind(tr.blackImage);
	}
	else
	{
		GL_Bind(pStage->bundle[TB_DIFFUSEMAP].image[0]);
	}
	GLSL_SetUniform_DiffuseTextureMatrix(&tr.geometricFillShader_DBS, tess.svars.texMatrices[TB_DIFFUSEMAP]);

	if(r_normalMapping->integer)
	{
		// bind u_NormalMap
		GL_SelectTexture(1);
		if(pStage->bundle[TB_NORMALMAP].image[0])
		{
			GL_Bind(pStage->bundle[TB_NORMALMAP].image[0]);
		}
		else
		{
			GL_Bind(tr.flatImage);
		}
		GLSL_SetUniform_NormalTextureMatrix(&tr.geometricFillShader_DBS, tess.svars.texMatrices[TB_NORMALMAP]);

		if(r_deferredShading->integer == DS_STANDARD)
		{
			// bind u_SpecularMap
			GL_SelectTexture(2);
			if(r_forceSpecular->integer)
			{
				GL_Bind(pStage->bundle[TB_DIFFUSEMAP].image[0]);
			}
			else if(pStage->bundle[TB_SPECULARMAP].image[0])
			{
				GL_Bind(pStage->bundle[TB_SPECULARMAP].image[0]);
			}
			else
			{
				GL_Bind(tr.blackImage);
			}
			GLSL_SetUniform_SpecularTextureMatrix(&tr.geometricFillShader_DBS, tess.svars.texMatrices[TB_SPECULARMAP]);
		}
	}

	Tess_DrawElements();

	GL_CheckErrors();
}


static void Render_depthFill(int stage, qboolean cmap2black)
{
	shaderStage_t  *pStage;
	uint32_t        stateBits;
	vec4_t          ambientColor;

	GLimp_LogComment("--- Render_depthFill ---\n");

	pStage = tess.surfaceStages[stage];

	// remove alpha test
	stateBits = pStage->stateBits;
	stateBits &= ~(GLS_SRCBLEND_BITS | GLS_DSTBLEND_BITS | GLS_ATEST_BITS);

	GL_State(stateBits);

	// enable shader, set arrays
	GL_BindProgram(&tr.depthFillShader);
	GL_VertexAttribsState(tr.depthFillShader.attribs);

	if(r_precomputedLighting->integer)
	{
		GL_VertexAttribsState(tr.depthFillShader.attribs);
	}
	else
	{
		GL_VertexAttribsState(tr.depthFillShader.attribs & ~(ATTR_COLOR));
		glVertexAttrib4fvARB(ATTR_INDEX_COLOR, tess.svars.color);
	}

	// set uniforms
	if(glConfig2.vboVertexSkinningAvailable)
	{
		GLSL_SetUniform_VertexSkinning(&tr.depthFillShader, tess.vboVertexSkinning);

		if(tess.vboVertexSkinning)
			glUniformMatrix4fvARB(tr.depthFillShader.u_BoneMatrix, MAX_BONES, GL_FALSE, &tess.boneMatrices[0][0]);
	}

	GLSL_SetUniform_AlphaTest(&tr.depthFillShader, pStage->stateBits);

	// u_DeformGen
	if(tess.surfaceShader->numDeforms)
	{
		deformStage_t  *ds;

		// only support the first one
		ds = &tess.surfaceShader->deforms[0];

		switch (ds->deformation)
		{
			case DEFORM_WAVE:
				GLSL_SetUniform_DeformGen(&tr.depthFillShader, (deformGen_t) ds->deformationWave.func);
				GLSL_SetUniform_DeformWave(&tr.depthFillShader, &ds->deformationWave);
				GLSL_SetUniform_DeformSpread(&tr.depthFillShader, ds->deformationSpread);
				GLSL_SetUniform_Time(&tr.depthFillShader, backEnd.refdef.floatTime);
				break;

			case DEFORM_BULGE:
				GLSL_SetUniform_DeformGen(&tr.depthFillShader, DGEN_BULGE);
				GLSL_SetUniform_DeformBulge(&tr.depthFillShader, ds);
				GLSL_SetUniform_Time(&tr.depthFillShader, backEnd.refdef.floatTime);
				break;

			default:
				GLSL_SetUniform_DeformGen(&tr.depthFillShader, DGEN_NONE);
				break;
		}
	}
	else
	{
		GLSL_SetUniform_DeformGen(&tr.depthFillShader, DGEN_NONE);
	}

	if(r_precomputedLighting->integer)
	{
		VectorCopy(backEnd.currentEntity->ambientLight, ambientColor);
		ClampColor(ambientColor);
	}
	else if(r_forceAmbient->integer)
	{
		ambientColor[0] = r_forceAmbient->value;
		ambientColor[1] = r_forceAmbient->value;
		ambientColor[2] = r_forceAmbient->value;
	}
	else
	{
		VectorClear(ambientColor);
	}
	GLSL_SetUniform_AmbientColor(&tr.depthFillShader, ambientColor);

	GLSL_SetUniform_ModelViewProjectionMatrix(&tr.depthFillShader, glState.modelViewProjectionMatrix[glState.stackIndex]);

	// bind u_ColorMap
	GL_SelectTexture(0);
	if(cmap2black)
	{
		GL_Bind(tr.blackImage);
	}
	else
	{
		GL_Bind(pStage->bundle[TB_DIFFUSEMAP].image[0]);
	}
	GLSL_SetUniform_ColorTextureMatrix(&tr.depthFillShader, tess.svars.texMatrices[TB_DIFFUSEMAP]);

	Tess_DrawElements();

	GL_CheckErrors();
}

static void Render_shadowFill(int stage)
{
	shaderStage_t  *pStage;
	uint32_t        stateBits;


	GLimp_LogComment("--- Render_shadowFill ---\n");

	pStage = tess.surfaceStages[stage];

	// remove alpha test
	stateBits = pStage->stateBits;
	stateBits &= ~(GLS_SRCBLEND_BITS | GLS_DSTBLEND_BITS | GLS_ATEST_BITS);

	GL_State(stateBits);

	// enable shader, set arrays
	GL_BindProgram(&tr.shadowFillShader);
	GL_VertexAttribsState(tr.shadowFillShader.attribs & ~(ATTR_COLOR));

	if(r_debugShadowMaps->integer)
	{
		vec4_t          shadowMapColor;

		Vector4Copy(g_color_table[backEnd.pc.c_batches % 8], shadowMapColor);

		glVertexAttrib4fvARB(ATTR_INDEX_COLOR, shadowMapColor);
	}

	// set uniforms
	GLSL_SetUniform_AlphaTest(&tr.shadowFillShader, pStage->stateBits);

	if(backEnd.currentLight->l.rlType == RL_DIRECTIONAL)
	{
		GLSL_SetUniform_LightParallel(&tr.shadowFillShader, qtrue);
	}
	else
	{
		GLSL_SetUniform_LightParallel(&tr.shadowFillShader, qfalse);

		GLSL_SetUniform_LightRadius(&tr.shadowFillShader, backEnd.currentLight->sphereRadius);
		GLSL_SetUniform_LightOrigin(&tr.shadowFillShader, backEnd.currentLight->origin);
	}

	GLSL_SetUniform_ModelMatrix(&tr.shadowFillShader, backEnd.orientation.transformMatrix);
	GLSL_SetUniform_ModelViewProjectionMatrix(&tr.shadowFillShader, glState.modelViewProjectionMatrix[glState.stackIndex]);

	if(glConfig2.vboVertexSkinningAvailable)
	{
		GLSL_SetUniform_VertexSkinning(&tr.shadowFillShader, tess.vboVertexSkinning);

		if(tess.vboVertexSkinning)
			glUniformMatrix4fvARB(tr.shadowFillShader.u_BoneMatrix, MAX_BONES, GL_FALSE, &tess.boneMatrices[0][0]);
	}

	if(tess.surfaceShader->numDeforms)
	{
		deformStage_t  *ds;

		// only support the first one
		ds = &tess.surfaceShader->deforms[0];

		switch (ds->deformation)
		{
			case DEFORM_WAVE:
				GLSL_SetUniform_DeformGen(&tr.shadowFillShader, (deformGen_t) ds->deformationWave.func);
				GLSL_SetUniform_DeformWave(&tr.shadowFillShader, &ds->deformationWave);
				GLSL_SetUniform_DeformSpread(&tr.shadowFillShader, ds->deformationSpread);
				GLSL_SetUniform_Time(&tr.shadowFillShader, backEnd.refdef.floatTime);
				break;

			case DEFORM_BULGE:
				GLSL_SetUniform_DeformGen(&tr.shadowFillShader, DGEN_BULGE);
				GLSL_SetUniform_DeformBulge(&tr.shadowFillShader, ds);
				GLSL_SetUniform_Time(&tr.shadowFillShader, backEnd.refdef.floatTime);
				break;

			default:
				GLSL_SetUniform_DeformGen(&tr.shadowFillShader, DGEN_NONE);
				break;
		}
	}
	else
	{
		GLSL_SetUniform_DeformGen(&tr.shadowFillShader, DGEN_NONE);
	}

	// bind u_ColorMap
	GL_SelectTexture(0);

	if((pStage->stateBits & GLS_ATEST_BITS) != 0)
	{
		GL_Bind(pStage->bundle[TB_COLORMAP].image[0]);
		GLSL_SetUniform_ColorTextureMatrix(&tr.shadowFillShader, tess.svars.texMatrices[TB_COLORMAP]);
	}
	else
	{
		GL_Bind(tr.whiteImage);
	}

	Tess_DrawElements();

	GL_CheckErrors();
}

static void Render_forwardLighting_DBS_omni(shaderStage_t * diffuseStage,
											shaderStage_t * attenuationXYStage,
											shaderStage_t * attenuationZStage, trRefLight_t * light)
{
	vec3_t          viewOrigin;
	vec3_t          lightOrigin;
	vec4_t          lightColor;
	float           shadowTexelSize;
	qboolean        shadowCompare;

	GLimp_LogComment("--- Render_forwardLighting_DBS_omni ---\n");

	// enable shader, set arrays
	GL_BindProgram(&tr.forwardLightingShader_DBS_omni);

	/*
	if(diffuseStage->vertexColor || diffuseStage->inverseVertexColor)
	{
		GL_VertexAttribsState(tr.forwardLightingShader_DBS_omni.attribs);
	}
	else
	*/
	{
		GL_VertexAttribsState(tr.forwardLightingShader_DBS_omni.attribs & ~(ATTR_COLOR));
		glVertexAttrib4fvARB(ATTR_INDEX_COLOR, colorWhite);
	}

	// set uniforms
	VectorCopy(backEnd.viewParms.orientation.origin, viewOrigin);
	VectorCopy(light->origin, lightOrigin);
	VectorCopy(tess.svars.color, lightColor);

	shadowCompare = (qboolean) (r_shadows->integer >= SHADOWING_VSM16 && !light->l.noShadows && light->shadowLOD >= 0);

	if(shadowCompare)
		shadowTexelSize = 1.0f / shadowMapResolutions[light->shadowLOD];
	else
		shadowTexelSize = 1.0f;

	GLSL_SetUniform_ViewOrigin(&tr.forwardLightingShader_DBS_omni, viewOrigin);
//	GLSL_SetUniform_InverseVertexColor(&tr.forwardLightingShader_DBS_omni, diffuseStage->inverseVertexColor);
	GLSL_SetUniform_LightOrigin(&tr.forwardLightingShader_DBS_omni, lightOrigin);
	GLSL_SetUniform_LightColor(&tr.forwardLightingShader_DBS_omni, lightColor);
	GLSL_SetUniform_LightRadius(&tr.forwardLightingShader_DBS_omni, light->sphereRadius);
	GLSL_SetUniform_LightScale(&tr.forwardLightingShader_DBS_omni, light->l.scale);
	GLSL_SetUniform_LightWrapAround(&tr.forwardLightingShader_DBS_omni, RB_EvalExpression(&diffuseStage->wrapAroundLightingExp, 0));
	GLSL_SetUniform_LightAttenuationMatrix(&tr.forwardLightingShader_DBS_omni, light->attenuationMatrix2);

	GLSL_SetUniform_ShadowCompare(&tr.forwardLightingShader_DBS_omni, shadowCompare);
	if(shadowCompare)
	{
		GLSL_SetUniform_ShadowTexelSize(&tr.forwardLightingShader_DBS_omni, shadowTexelSize);
		GLSL_SetUniform_ShadowBlur(&tr.forwardLightingShader_DBS_omni, r_shadowBlur->value);
	}

	GLSL_SetUniform_ModelMatrix(&tr.forwardLightingShader_DBS_omni, backEnd.orientation.transformMatrix);
	GLSL_SetUniform_ModelViewProjectionMatrix(&tr.forwardLightingShader_DBS_omni, glState.modelViewProjectionMatrix[glState.stackIndex]);

	if(glConfig2.vboVertexSkinningAvailable)
	{
		GLSL_SetUniform_VertexSkinning(&tr.forwardLightingShader_DBS_omni, tess.vboVertexSkinning);

		if(tess.vboVertexSkinning)
			glUniformMatrix4fvARB(tr.forwardLightingShader_DBS_omni.u_BoneMatrix, MAX_BONES, GL_FALSE, &tess.boneMatrices[0][0]);
	}

	// u_DeformGen
	if(tess.surfaceShader->numDeforms)
	{
		deformStage_t  *ds;

		// only support the first one
		ds = &tess.surfaceShader->deforms[0];

		switch (ds->deformation)
		{
			case DEFORM_WAVE:
				GLSL_SetUniform_DeformGen(&tr.forwardLightingShader_DBS_omni, (deformGen_t) ds->deformationWave.func);
				GLSL_SetUniform_DeformWave(&tr.forwardLightingShader_DBS_omni, &ds->deformationWave);
				GLSL_SetUniform_DeformSpread(&tr.forwardLightingShader_DBS_omni, ds->deformationSpread);
				GLSL_SetUniform_Time(&tr.forwardLightingShader_DBS_omni, backEnd.refdef.floatTime);
				break;

			case DEFORM_BULGE:
				GLSL_SetUniform_DeformGen(&tr.forwardLightingShader_DBS_omni, DGEN_BULGE);
				GLSL_SetUniform_DeformBulge(&tr.forwardLightingShader_DBS_omni, ds);
				GLSL_SetUniform_Time(&tr.forwardLightingShader_DBS_omni, backEnd.refdef.floatTime);
				break;

			default:
				GLSL_SetUniform_DeformGen(&tr.forwardLightingShader_DBS_omni, DGEN_NONE);
				break;
		}
	}
	else
	{
		GLSL_SetUniform_DeformGen(&tr.forwardLightingShader_DBS_omni, DGEN_NONE);
	}

	GLSL_SetUniform_PortalClipping(&tr.forwardLightingShader_DBS_omni, backEnd.viewParms.isPortal);
	if(backEnd.viewParms.isPortal)
	{
		float           plane[4];

		// clipping plane in world space
		plane[0] = backEnd.viewParms.portalPlane.normal[0];
		plane[1] = backEnd.viewParms.portalPlane.normal[1];
		plane[2] = backEnd.viewParms.portalPlane.normal[2];
		plane[3] = backEnd.viewParms.portalPlane.dist;

		GLSL_SetUniform_PortalPlane(&tr.forwardLightingShader_DBS_omni, plane);
	}

	// bind u_DiffuseMap
	GL_SelectTexture(0);
	GL_Bind(diffuseStage->bundle[TB_DIFFUSEMAP].image[0]);
	GLSL_SetUniform_DiffuseTextureMatrix(&tr.forwardLightingShader_DBS_omni, tess.svars.texMatrices[TB_DIFFUSEMAP]);

	if(r_normalMapping->integer)
	{
		// bind u_NormalMap
		GL_SelectTexture(1);
		if(diffuseStage->bundle[TB_NORMALMAP].image[0])
		{
			GL_Bind(diffuseStage->bundle[TB_NORMALMAP].image[0]);
		}
		else
		{
			GL_Bind(tr.flatImage);
		}
		GLSL_SetUniform_NormalTextureMatrix(&tr.forwardLightingShader_DBS_omni, tess.svars.texMatrices[TB_NORMALMAP]);

		// bind u_SpecularMap
		GL_SelectTexture(2);
		if(r_forceSpecular->integer)
		{
			GL_Bind(diffuseStage->bundle[TB_DIFFUSEMAP].image[0]);
		}
		else if(diffuseStage->bundle[TB_SPECULARMAP].image[0])
		{
			GL_Bind(diffuseStage->bundle[TB_SPECULARMAP].image[0]);
		}
		else
		{
			GL_Bind(tr.blackImage);
		}
		GLSL_SetUniform_SpecularTextureMatrix(&tr.forwardLightingShader_DBS_omni, tess.svars.texMatrices[TB_SPECULARMAP]);
	}

	// bind u_AttenuationMapXY
	GL_SelectTexture(3);
	BindAnimatedImage(&attenuationXYStage->bundle[TB_COLORMAP]);

	// bind u_AttenuationMapZ
	GL_SelectTexture(4);
	BindAnimatedImage(&attenuationZStage->bundle[TB_COLORMAP]);

	// bind u_ShadowMap
	if(shadowCompare)
	{
		GL_SelectTexture(5);
		GL_Bind(tr.shadowCubeFBOImage[light->shadowLOD]);
	}

	Tess_DrawElements();

	GL_CheckErrors();
}

static void Render_forwardLighting_DBS_proj(shaderStage_t * diffuseStage,
											shaderStage_t * attenuationXYStage,
											shaderStage_t * attenuationZStage, trRefLight_t * light)
{
	vec3_t          viewOrigin;
	vec3_t          lightOrigin;
	vec4_t          lightColor;
	float           shadowTexelSize;
	qboolean        shadowCompare;

	GLimp_LogComment("--- Render_fowardLighting_DBS_proj ---\n");

	// enable shader, set arrays
	GL_BindProgram(&tr.forwardLightingShader_DBS_proj);

	/*
	if(diffuseStage->vertexColor || diffuseStage->inverseVertexColor)
	{
		GL_VertexAttribsState(tr.forwardLightingShader_DBS_proj.attribs);
	}
	else
	*/
	{
		GL_VertexAttribsState(tr.forwardLightingShader_DBS_proj.attribs & ~(ATTR_COLOR));
		glVertexAttrib4fvARB(ATTR_INDEX_COLOR, colorWhite);
	}

	// set uniforms
	VectorCopy(backEnd.viewParms.orientation.origin, viewOrigin);
	VectorCopy(light->origin, lightOrigin);
	VectorCopy(tess.svars.color, lightColor);

	shadowCompare = (qboolean) (r_shadows->integer >= SHADOWING_VSM16 && !light->l.noShadows && light->shadowLOD >= 0);

	if(shadowCompare)
		shadowTexelSize = 1.0f / shadowMapResolutions[light->shadowLOD];
	else
		shadowTexelSize = 1.0f;

	GLSL_SetUniform_ViewOrigin(&tr.forwardLightingShader_DBS_proj, viewOrigin);
//	GLSL_SetUniform_InverseVertexColor(&tr.forwardLightingShader_DBS_proj, diffuseStage->inverseVertexColor);
	GLSL_SetUniform_LightOrigin(&tr.forwardLightingShader_DBS_proj, lightOrigin);
	GLSL_SetUniform_LightColor(&tr.forwardLightingShader_DBS_proj, lightColor);
	GLSL_SetUniform_LightRadius(&tr.forwardLightingShader_DBS_proj, light->sphereRadius);
	GLSL_SetUniform_LightScale(&tr.forwardLightingShader_DBS_proj, light->l.scale);
	GLSL_SetUniform_LightWrapAround(&tr.forwardLightingShader_DBS_proj, RB_EvalExpression(&diffuseStage->wrapAroundLightingExp, 0));
	GLSL_SetUniform_LightAttenuationMatrix(&tr.forwardLightingShader_DBS_proj, light->attenuationMatrix2);

	GLSL_SetUniform_ShadowCompare(&tr.forwardLightingShader_DBS_proj, shadowCompare);
	if(shadowCompare)
	{
		GLSL_SetUniform_ShadowMatrix(&tr.forwardLightingShader_DBS_proj, light->shadowMatrices);
		GLSL_SetUniform_ShadowTexelSize(&tr.forwardLightingShader_DBS_proj, shadowTexelSize);
		GLSL_SetUniform_ShadowBlur(&tr.forwardLightingShader_DBS_proj, r_shadowBlur->value);
	}

	GLSL_SetUniform_ModelMatrix(&tr.forwardLightingShader_DBS_proj, backEnd.orientation.transformMatrix);
	GLSL_SetUniform_ModelViewProjectionMatrix(&tr.forwardLightingShader_DBS_proj, glState.modelViewProjectionMatrix[glState.stackIndex]);

	if(glConfig2.vboVertexSkinningAvailable)
	{
		GLSL_SetUniform_VertexSkinning(&tr.forwardLightingShader_DBS_proj, tess.vboVertexSkinning);

		if(tess.vboVertexSkinning)
			glUniformMatrix4fvARB(tr.forwardLightingShader_DBS_proj.u_BoneMatrix, MAX_BONES, GL_FALSE, &tess.boneMatrices[0][0]);
	}

	// u_DeformGen
	if(tess.surfaceShader->numDeforms)
	{
		deformStage_t  *ds;

		// only support the first one
		ds = &tess.surfaceShader->deforms[0];

		switch (ds->deformation)
		{
			case DEFORM_WAVE:
				GLSL_SetUniform_DeformGen(&tr.forwardLightingShader_DBS_proj, (deformGen_t) ds->deformationWave.func);
				GLSL_SetUniform_DeformWave(&tr.forwardLightingShader_DBS_proj, &ds->deformationWave);
				GLSL_SetUniform_DeformSpread(&tr.forwardLightingShader_DBS_proj, ds->deformationSpread);
				GLSL_SetUniform_Time(&tr.forwardLightingShader_DBS_proj, backEnd.refdef.floatTime);
				break;

			case DEFORM_BULGE:
				GLSL_SetUniform_DeformGen(&tr.forwardLightingShader_DBS_proj, DGEN_BULGE);
				GLSL_SetUniform_DeformBulge(&tr.forwardLightingShader_DBS_proj, ds);
				GLSL_SetUniform_Time(&tr.forwardLightingShader_DBS_proj, backEnd.refdef.floatTime);
				break;

			default:
				GLSL_SetUniform_DeformGen(&tr.forwardLightingShader_DBS_proj, DGEN_NONE);
				break;
		}
	}
	else
	{
		GLSL_SetUniform_DeformGen(&tr.forwardLightingShader_DBS_proj, DGEN_NONE);
	}

	GLSL_SetUniform_PortalClipping(&tr.forwardLightingShader_DBS_proj, backEnd.viewParms.isPortal);
	if(backEnd.viewParms.isPortal)
	{
		float           plane[4];

		// clipping plane in world space
		plane[0] = backEnd.viewParms.portalPlane.normal[0];
		plane[1] = backEnd.viewParms.portalPlane.normal[1];
		plane[2] = backEnd.viewParms.portalPlane.normal[2];
		plane[3] = backEnd.viewParms.portalPlane.dist;

		GLSL_SetUniform_PortalPlane(&tr.forwardLightingShader_DBS_proj, plane);
	}

	// bind u_DiffuseMap
	GL_SelectTexture(0);
	GL_Bind(diffuseStage->bundle[TB_DIFFUSEMAP].image[0]);
	GLSL_SetUniform_DiffuseTextureMatrix(&tr.forwardLightingShader_DBS_proj, tess.svars.texMatrices[TB_DIFFUSEMAP]);

	if(r_normalMapping->integer)
	{
		// bind u_NormalMap
		GL_SelectTexture(1);
		if(diffuseStage->bundle[TB_NORMALMAP].image[0])
		{
			GL_Bind(diffuseStage->bundle[TB_NORMALMAP].image[0]);
		}
		else
		{
			GL_Bind(tr.flatImage);
		}
		GLSL_SetUniform_NormalTextureMatrix(&tr.forwardLightingShader_DBS_proj, tess.svars.texMatrices[TB_NORMALMAP]);

		// bind u_SpecularMap
		GL_SelectTexture(2);
		if(r_forceSpecular->integer)
		{
			GL_Bind(diffuseStage->bundle[TB_DIFFUSEMAP].image[0]);
		}
		else if(diffuseStage->bundle[TB_SPECULARMAP].image[0])
		{
			GL_Bind(diffuseStage->bundle[TB_SPECULARMAP].image[0]);
		}
		else
		{
			GL_Bind(tr.blackImage);
		}
		GLSL_SetUniform_SpecularTextureMatrix(&tr.forwardLightingShader_DBS_proj, tess.svars.texMatrices[TB_SPECULARMAP]);
	}

	// bind u_AttenuationMapXY
	GL_SelectTexture(3);
	BindAnimatedImage(&attenuationXYStage->bundle[TB_COLORMAP]);

	// bind u_AttenuationMapZ
	GL_SelectTexture(4);
	BindAnimatedImage(&attenuationZStage->bundle[TB_COLORMAP]);

	// bind u_ShadowMap
	if(shadowCompare)
	{
		GL_SelectTexture(5);
		GL_Bind(tr.shadowMapFBOImage[light->shadowLOD]);
	}

	Tess_DrawElements();

	GL_CheckErrors();
}

static void Render_forwardLighting_DBS_directional(shaderStage_t * diffuseStage,
											shaderStage_t * attenuationXYStage,
											shaderStage_t * attenuationZStage, trRefLight_t * light)
{
	vec3_t          viewOrigin;
	vec3_t          lightDirection;
	vec4_t          lightColor;
	float           shadowTexelSize;
	qboolean        shadowCompare;

	GLimp_LogComment("--- Render_fowardLighting_DBS_directional ---\n");

	// enable shader, set arrays
	GL_BindProgram(&tr.forwardLightingShader_DBS_directional);

	/*
	if(diffuseStage->vertexColor || diffuseStage->inverseVertexColor)
	{
		GL_VertexAttribsState(tr.forwardLightingShader_DBS_directional.attribs);
	}
	else
	*/
	{
		GL_VertexAttribsState(tr.forwardLightingShader_DBS_directional.attribs & ~(ATTR_COLOR));
		glVertexAttrib4fvARB(ATTR_INDEX_COLOR, colorWhite);
	}

	// set uniforms
	VectorCopy(backEnd.viewParms.orientation.origin, viewOrigin);
	VectorCopy(tess.svars.color, lightColor);

#if 1
	VectorCopy(tr.sunDirection, lightDirection);
#else
	VectorCopy(light->direction, lightDirection);
#endif

	shadowCompare = (qboolean) (r_shadows->integer >= SHADOWING_VSM16 && !light->l.noShadows && light->shadowLOD >= 0);

	if(shadowCompare)
		shadowTexelSize = 1.0f / shadowMapResolutions[light->shadowLOD];
	else
		shadowTexelSize = 1.0f;

	GLSL_SetUniform_ViewOrigin(&tr.forwardLightingShader_DBS_directional, viewOrigin);
//	GLSL_SetUniform_InverseVertexColor(&tr.forwardLightingShader_DBS_directional, diffuseStage->inverseVertexColor);
	GLSL_SetUniform_LightDir(&tr.forwardLightingShader_DBS_directional, lightDirection);
	GLSL_SetUniform_LightColor(&tr.forwardLightingShader_DBS_directional, lightColor);
	GLSL_SetUniform_LightRadius(&tr.forwardLightingShader_DBS_directional, light->sphereRadius);
	GLSL_SetUniform_LightScale(&tr.forwardLightingShader_DBS_directional, light->l.scale);
	GLSL_SetUniform_LightWrapAround(&tr.forwardLightingShader_DBS_directional, RB_EvalExpression(&diffuseStage->wrapAroundLightingExp, 0));
//	GLSL_SetUniform_LightWrapAround(&tr.forwardLightingShader_DBS_directional, r_wrapAroundLighting->value);
	GLSL_SetUniform_LightAttenuationMatrix(&tr.forwardLightingShader_DBS_directional, light->attenuationMatrix2);

	GLSL_SetUniform_ShadowCompare(&tr.forwardLightingShader_DBS_directional, shadowCompare);
	if(shadowCompare)
	{
		GLSL_SetUniform_ShadowMatrix(&tr.forwardLightingShader_DBS_directional, light->shadowMatricesBiased);
		GLSL_SetUniform_ShadowParallelSplitDistances(&tr.forwardLightingShader_DBS_directional, backEnd.viewParms.parallelSplitDistances);
		GLSL_SetUniform_ShadowTexelSize(&tr.forwardLightingShader_DBS_directional, shadowTexelSize);
		GLSL_SetUniform_ShadowBlur(&tr.forwardLightingShader_DBS_directional, r_shadowBlur->value);
	}

	GLSL_SetUniform_ModelMatrix(&tr.forwardLightingShader_DBS_directional, backEnd.orientation.transformMatrix);
	GLSL_SetUniform_ViewMatrix(&tr.forwardLightingShader_DBS_directional, backEnd.viewParms.world.viewMatrix);
//	GLSL_SetUniform_ModelViewMatrix(&tr.forwardLightingShader_DBS_directional, glState.modelViewMatrix[glState.stackIndex]);
	GLSL_SetUniform_ModelViewProjectionMatrix(&tr.forwardLightingShader_DBS_directional, glState.modelViewProjectionMatrix[glState.stackIndex]);

	if(glConfig2.vboVertexSkinningAvailable)
	{
		GLSL_SetUniform_VertexSkinning(&tr.forwardLightingShader_DBS_directional, tess.vboVertexSkinning);

		if(tess.vboVertexSkinning)
			glUniformMatrix4fvARB(tr.forwardLightingShader_DBS_directional.u_BoneMatrix, MAX_BONES, GL_FALSE, &tess.boneMatrices[0][0]);
	}

	// u_DeformGen
	if(tess.surfaceShader->numDeforms)
	{
		deformStage_t  *ds;

		// only support the first one
		ds = &tess.surfaceShader->deforms[0];

		switch (ds->deformation)
		{
			case DEFORM_WAVE:
				GLSL_SetUniform_DeformGen(&tr.forwardLightingShader_DBS_directional, (deformGen_t) ds->deformationWave.func);
				GLSL_SetUniform_DeformWave(&tr.forwardLightingShader_DBS_directional, &ds->deformationWave);
				GLSL_SetUniform_DeformSpread(&tr.forwardLightingShader_DBS_directional, ds->deformationSpread);
				GLSL_SetUniform_Time(&tr.forwardLightingShader_DBS_directional, backEnd.refdef.floatTime);
				break;

			case DEFORM_BULGE:
				GLSL_SetUniform_DeformGen(&tr.forwardLightingShader_DBS_directional, DGEN_BULGE);
				GLSL_SetUniform_DeformBulge(&tr.forwardLightingShader_DBS_directional, ds);
				GLSL_SetUniform_Time(&tr.forwardLightingShader_DBS_directional, backEnd.refdef.floatTime);
				break;

			default:
				GLSL_SetUniform_DeformGen(&tr.forwardLightingShader_DBS_directional, DGEN_NONE);
				break;
		}
	}
	else
	{
		GLSL_SetUniform_DeformGen(&tr.forwardLightingShader_DBS_directional, DGEN_NONE);
	}

	GLSL_SetUniform_PortalClipping(&tr.forwardLightingShader_DBS_directional, backEnd.viewParms.isPortal);
	if(backEnd.viewParms.isPortal)
	{
		float           plane[4];

		// clipping plane in world space
		plane[0] = backEnd.viewParms.portalPlane.normal[0];
		plane[1] = backEnd.viewParms.portalPlane.normal[1];
		plane[2] = backEnd.viewParms.portalPlane.normal[2];
		plane[3] = backEnd.viewParms.portalPlane.dist;

		GLSL_SetUniform_PortalPlane(&tr.forwardLightingShader_DBS_directional, plane);
	}

	// bind u_DiffuseMap
	GL_SelectTexture(0);
	GL_Bind(diffuseStage->bundle[TB_DIFFUSEMAP].image[0]);
	GLSL_SetUniform_DiffuseTextureMatrix(&tr.forwardLightingShader_DBS_directional, tess.svars.texMatrices[TB_DIFFUSEMAP]);

	if(r_normalMapping->integer)
	{
		// bind u_NormalMap
		GL_SelectTexture(1);
		if(diffuseStage->bundle[TB_NORMALMAP].image[0])
		{
			GL_Bind(diffuseStage->bundle[TB_NORMALMAP].image[0]);
		}
		else
		{
			GL_Bind(tr.flatImage);
		}
		GLSL_SetUniform_NormalTextureMatrix(&tr.forwardLightingShader_DBS_directional, tess.svars.texMatrices[TB_NORMALMAP]);

		// bind u_SpecularMap
		GL_SelectTexture(2);
		if(r_forceSpecular->integer)
		{
			GL_Bind(diffuseStage->bundle[TB_DIFFUSEMAP].image[0]);
		}
		else if(diffuseStage->bundle[TB_SPECULARMAP].image[0])
		{
			GL_Bind(diffuseStage->bundle[TB_SPECULARMAP].image[0]);
		}
		else
		{
			GL_Bind(tr.blackImage);
		}
		GLSL_SetUniform_SpecularTextureMatrix(&tr.forwardLightingShader_DBS_directional, tess.svars.texMatrices[TB_SPECULARMAP]);
	}

	// bind u_ShadowMap
	if(shadowCompare)
	{
		GL_SelectTexture(5);
		GL_Bind(tr.shadowMapFBOImage[0]);

		if(r_parallelShadowSplits->integer >= 1)
		{
			GL_SelectTexture(6);
			GL_Bind(tr.shadowMapFBOImage[1]);
		}

		if(r_parallelShadowSplits->integer >= 2)
		{
			GL_SelectTexture(7);
			GL_Bind(tr.shadowMapFBOImage[2]);
		}

		if(r_parallelShadowSplits->integer >= 3)
		{
			GL_SelectTexture(8);
			GL_Bind(tr.shadowMapFBOImage[3]);
		}

		if(r_parallelShadowSplits->integer >= 4)
		{
			GL_SelectTexture(9);
			GL_Bind(tr.shadowMapFBOImage[4]);
		}
	}

	Tess_DrawElements();

	GL_CheckErrors();
}

static void Render_reflection_C(int stage)
{
	vec3_t          viewOrigin;
	shaderStage_t  *pStage = tess.surfaceStages[stage];

	GLimp_LogComment("--- Render_reflection_C ---\n");

	GL_State(pStage->stateBits);

	// enable shader, set arrays
	GL_BindProgram(&tr.reflectionShader_C);
	GL_VertexAttribsState(tr.reflectionShader_C.attribs);

	// set uniforms
	VectorCopy(backEnd.viewParms.orientation.origin, viewOrigin);	// in world space
	GLSL_SetUniform_ViewOrigin(&tr.reflectionShader_C, viewOrigin);

	GLSL_SetUniform_ModelMatrix(&tr.reflectionShader_C, backEnd.orientation.transformMatrix);
	GLSL_SetUniform_ModelViewProjectionMatrix(&tr.reflectionShader_C, glState.modelViewProjectionMatrix[glState.stackIndex]);

	if(glConfig2.vboVertexSkinningAvailable)
	{
		GLSL_SetUniform_VertexSkinning(&tr.reflectionShader_C, tess.vboVertexSkinning);

		if(tess.vboVertexSkinning)
			glUniformMatrix4fvARB(tr.reflectionShader_C.u_BoneMatrix, MAX_BONES, GL_FALSE, &tess.boneMatrices[0][0]);
	}

	// bind u_ColorMap
	GL_SelectTexture(0);
#if 1
	if(backEnd.currentEntity && (backEnd.currentEntity != &tr.worldEntity))
	{
		GL_BindNearestCubeMap(backEnd.currentEntity->e.origin);
	}
	else
	{
		GL_BindNearestCubeMap(viewOrigin);
	}
#else
	GL_Bind(pStage->bundle[TB_COLORMAP].image[0]);
#endif

	Tess_DrawElements();

	GL_CheckErrors();
}

static void Render_reflection_CB(int stage)
{
	vec3_t          viewOrigin;
	shaderStage_t  *pStage = tess.surfaceStages[stage];

	GLimp_LogComment("--- Render_reflection_CB ---\n");

	GL_State(pStage->stateBits);

	// enable shader, set arrays
	GL_BindProgram(&tr.reflectionShader_CB);
	GL_VertexAttribsState(tr.reflectionShader_CB.attribs);

	// set uniforms
	VectorCopy(backEnd.viewParms.orientation.origin, viewOrigin);	// in world space
	GLSL_SetUniform_ViewOrigin(&tr.reflectionShader_CB, viewOrigin);

	GLSL_SetUniform_ModelMatrix(&tr.reflectionShader_CB, backEnd.orientation.transformMatrix);
	GLSL_SetUniform_ModelViewProjectionMatrix(&tr.reflectionShader_CB, glState.modelViewProjectionMatrix[glState.stackIndex]);

	if(glConfig2.vboVertexSkinningAvailable)
	{
		GLSL_SetUniform_VertexSkinning(&tr.reflectionShader_CB, tess.vboVertexSkinning);

		if(tess.vboVertexSkinning)
			glUniformMatrix4fvARB(tr.reflectionShader_CB.u_BoneMatrix, MAX_BONES, GL_FALSE, &tess.boneMatrices[0][0]);
	}

	// bind u_ColorMap
	GL_SelectTexture(0);
#if 1
	if(backEnd.currentEntity && (backEnd.currentEntity != &tr.worldEntity))
	{
		GL_BindNearestCubeMap(backEnd.currentEntity->e.origin);
	}
	else
	{
		GL_BindNearestCubeMap(viewOrigin);
	}
#else
	GL_Bind(pStage->bundle[TB_COLORMAP].image[0]);
#endif

	// bind u_NormalMap
	GL_SelectTexture(1);
	GL_Bind(pStage->bundle[TB_NORMALMAP].image[0]);
	GLSL_SetUniform_NormalTextureMatrix(&tr.reflectionShader_CB, tess.svars.texMatrices[TB_NORMALMAP]);

	Tess_DrawElements();

	GL_CheckErrors();
}

static void Render_refraction_C(int stage)
{
	vec3_t          viewOrigin;
	shaderStage_t  *pStage = tess.surfaceStages[stage];

	GLimp_LogComment("--- Render_refraction_C ---\n");

	GL_State(pStage->stateBits);

	// enable shader, set arrays
	GL_BindProgram(&tr.refractionShader_C);
	GL_VertexAttribsState(tr.refractionShader_C.attribs);

	// set uniforms
	VectorCopy(backEnd.viewParms.orientation.origin, viewOrigin);	// in world space
	GLSL_SetUniform_ViewOrigin(&tr.refractionShader_C, viewOrigin);
	GLSL_SetUniform_RefractionIndex(&tr.refractionShader_C, RB_EvalExpression(&pStage->refractionIndexExp, 1.0));
	glUniform1fARB(tr.refractionShader_C.u_FresnelPower, RB_EvalExpression(&pStage->fresnelPowerExp, 2.0));
	glUniform1fARB(tr.refractionShader_C.u_FresnelScale, RB_EvalExpression(&pStage->fresnelScaleExp, 2.0));
	glUniform1fARB(tr.refractionShader_C.u_FresnelBias, RB_EvalExpression(&pStage->fresnelBiasExp, 1.0));

	GLSL_SetUniform_ModelMatrix(&tr.refractionShader_C, backEnd.orientation.transformMatrix);
	GLSL_SetUniform_ModelViewProjectionMatrix(&tr.refractionShader_C, glState.modelViewProjectionMatrix[glState.stackIndex]);

	if(glConfig2.vboVertexSkinningAvailable)
	{
		GLSL_SetUniform_VertexSkinning(&tr.refractionShader_C, tess.vboVertexSkinning);

		if(tess.vboVertexSkinning)
			glUniformMatrix4fvARB(tr.refractionShader_C.u_BoneMatrix, MAX_BONES, GL_FALSE, &tess.boneMatrices[0][0]);
	}

	// bind u_ColorMap
	GL_SelectTexture(0);
	GL_Bind(pStage->bundle[TB_COLORMAP].image[0]);

	Tess_DrawElements();

	GL_CheckErrors();
}

static void Render_dispersion_C(int stage)
{
	vec3_t          viewOrigin;
	shaderStage_t  *pStage = tess.surfaceStages[stage];
	float           eta;
	float           etaDelta;

	GLimp_LogComment("--- Render_dispersion_C ---\n");

	GL_State(pStage->stateBits);

	// enable shader, set arrays
	GL_BindProgram(&tr.dispersionShader_C);
	GL_VertexAttribsState(tr.dispersionShader_C.attribs);

	// set uniforms
	VectorCopy(backEnd.viewParms.orientation.origin, viewOrigin);	// in world space
	eta = RB_EvalExpression(&pStage->etaExp, (float)1.1);
	etaDelta = RB_EvalExpression(&pStage->etaDeltaExp, (float)-0.02);

	GLSL_SetUniform_ViewOrigin(&tr.dispersionShader_C, viewOrigin);
	glUniform3fARB(tr.dispersionShader_C.u_EtaRatio, eta, eta + etaDelta, eta + (etaDelta * 2));
	glUniform1fARB(tr.dispersionShader_C.u_FresnelPower, RB_EvalExpression(&pStage->fresnelPowerExp, 2.0f));
	glUniform1fARB(tr.dispersionShader_C.u_FresnelScale, RB_EvalExpression(&pStage->fresnelScaleExp, 2.0f));
	glUniform1fARB(tr.dispersionShader_C.u_FresnelBias, RB_EvalExpression(&pStage->fresnelBiasExp, 1.0f));

	GLSL_SetUniform_ModelMatrix(&tr.dispersionShader_C, backEnd.orientation.transformMatrix);
	GLSL_SetUniform_ModelViewProjectionMatrix(&tr.dispersionShader_C, glState.modelViewProjectionMatrix[glState.stackIndex]);

	if(glConfig2.vboVertexSkinningAvailable)
	{
		GLSL_SetUniform_VertexSkinning(&tr.dispersionShader_C, tess.vboVertexSkinning);

		if(tess.vboVertexSkinning)
			glUniformMatrix4fvARB(tr.dispersionShader_C.u_BoneMatrix, MAX_BONES, GL_FALSE, &tess.boneMatrices[0][0]);
	}

	// bind u_ColorMap
	GL_SelectTexture(0);
	GL_Bind(pStage->bundle[TB_COLORMAP].image[0]);

	Tess_DrawElements();

	GL_CheckErrors();
}

static void Render_skybox(int stage)
{
	vec3_t          viewOrigin;
	shaderStage_t  *pStage = tess.surfaceStages[stage];

	GLimp_LogComment("--- Render_skybox ---\n");

	GL_State(pStage->stateBits);

	// enable shader, set arrays
	GL_BindProgram(&tr.skyBoxShader);
	GL_VertexAttribsState(tr.skyBoxShader.attribs);

	// set uniforms
	VectorCopy(backEnd.viewParms.orientation.origin, viewOrigin);	// in world space

	GLSL_SetUniform_ViewOrigin(&tr.skyBoxShader, viewOrigin);

	GLSL_SetUniform_ModelMatrix(&tr.skyBoxShader, backEnd.orientation.transformMatrix);
	GLSL_SetUniform_ModelViewProjectionMatrix(&tr.skyBoxShader, glState.modelViewProjectionMatrix[glState.stackIndex]);

	// bind u_ColorMap
	GL_SelectTexture(0);
	GL_Bind(pStage->bundle[TB_COLORMAP].image[0]);

	Tess_DrawElements();

	GL_CheckErrors();
}

static void Render_screen(int stage)
{
	shaderStage_t  *pStage = tess.surfaceStages[stage];

	GLimp_LogComment("--- Render_screen ---\n");

	GL_State(pStage->stateBits);

	// enable shader, set arrays
	GL_BindProgram(&tr.screenShader);

	/*
	if(pStage->vertexColor || pStage->inverseVertexColor)
	{
		GL_VertexAttribsState(tr.screenShader.attribs);
	}
	else
	*/
	{
		GL_VertexAttribsState(tr.screenShader.attribs & ~(ATTR_COLOR));
		glVertexAttrib4fvARB(ATTR_INDEX_COLOR, tess.svars.color);
	}

	GLSL_SetUniform_ModelViewProjectionMatrix(&tr.screenShader, glState.modelViewProjectionMatrix[glState.stackIndex]);

	// bind u_CurrentMap
	GL_SelectTexture(0);
	BindAnimatedImage(&pStage->bundle[TB_COLORMAP]);

	Tess_DrawElements();

	GL_CheckErrors();
}

static void Render_portal(int stage)
{
	shaderStage_t  *pStage = tess.surfaceStages[stage];

	GLimp_LogComment("--- Render_portal ---\n");

	GL_State(pStage->stateBits);

	// enable shader, set arrays
	GL_BindProgram(&tr.portalShader);

	/*
	if(pStage->vertexColor || pStage->inverseVertexColor)
	{
		GL_VertexAttribsState(tr.portalShader.attribs);
	}
	else
	*/
	{
		GL_VertexAttribsState(tr.portalShader.attribs & ~(ATTR_COLOR));
		glVertexAttrib4fvARB(ATTR_INDEX_COLOR, tess.svars.color);
	}

	glUniform1fARB(tr.portalShader.u_PortalRange, tess.surfaceShader->portalRange);

	GLSL_SetUniform_ModelViewMatrix(&tr.portalShader, glState.modelViewMatrix[glState.stackIndex]);
	GLSL_SetUniform_ModelViewProjectionMatrix(&tr.portalShader, glState.modelViewProjectionMatrix[glState.stackIndex]);

	// bind u_CurrentMap
	GL_SelectTexture(0);
	BindAnimatedImage(&pStage->bundle[TB_COLORMAP]);

	Tess_DrawElements();

	GL_CheckErrors();
}

static void Render_heatHaze(int stage)
{
	uint32_t        stateBits;
	float           deformMagnitude;
	shaderStage_t  *pStage = tess.surfaceStages[stage];

	GLimp_LogComment("--- Render_heatHaze ---\n");

	if(r_heatHazeFix->integer && glConfig2.framebufferBlitAvailable && glConfig.hardwareType != GLHW_ATI && glConfig.hardwareType != GLHW_ATI_DX10 && glConfig.driverType != GLDRV_MESA)
	{
		FBO_t          *previousFBO;
		uint32_t        stateBits;

		GLimp_LogComment("--- HEATHAZE FIX BEGIN ---\n");

		// capture current color buffer for u_CurrentMap
		/*
		GL_SelectTexture(0);
		GL_Bind(tr.currentRenderImage);
		glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, tr.currentRenderImage->uploadWidth,
							 tr.currentRenderImage->uploadHeight);

		*/

		previousFBO = glState.currentFBO;

		if(DS_STANDARD_ENABLED())
		{
			// copy deferredRenderFBO to portalRenderFBO
			glBindFramebufferEXT(GL_READ_FRAMEBUFFER_EXT, tr.deferredRenderFBO->frameBuffer);
			glBindFramebufferEXT(GL_DRAW_FRAMEBUFFER_EXT, tr.occlusionRenderFBO->frameBuffer);
			glBlitFramebufferEXT(0, 0, tr.deferredRenderFBO->width, tr.deferredRenderFBO->height,
								   0, 0, tr.occlusionRenderFBO->width, tr.occlusionRenderFBO->height,
								   GL_DEPTH_BUFFER_BIT,
								   GL_NEAREST);
		}
		else if(DS_PREPASS_LIGHTING_ENABLED())
		{
#if defined(OFFSCREEN_PREPASS_LIGHTING)
			// copy deferredRenderFBO to portalRenderFBO
			glBindFramebufferEXT(GL_READ_FRAMEBUFFER_EXT, tr.deferredRenderFBO->frameBuffer);
			glBindFramebufferEXT(GL_DRAW_FRAMEBUFFER_EXT, tr.occlusionRenderFBO->frameBuffer);
			glBlitFramebufferEXT(0, 0, tr.deferredRenderFBO->width, tr.deferredRenderFBO->height,
								   0, 0, tr.occlusionRenderFBO->width, tr.occlusionRenderFBO->height,
								   GL_DEPTH_BUFFER_BIT,
								   GL_NEAREST);
#else
#if 1
			// copy depth of the main context to deferredRenderFBO
			glBindFramebufferEXT(GL_READ_FRAMEBUFFER_EXT, 0);
			glBindFramebufferEXT(GL_DRAW_FRAMEBUFFER_EXT, tr.occlusionRenderFBO->frameBuffer);
			glBlitFramebufferEXT(0, 0, glConfig.vidWidth, glConfig.vidHeight,
								   0, 0, glConfig.vidWidth, glConfig.vidHeight,
								   GL_DEPTH_BUFFER_BIT,
								   GL_NEAREST);
#endif
#endif
		}
		else if(HDR_ENABLED())
		{
			// copy deferredRenderFBO to portalRenderFBO
			glBindFramebufferEXT(GL_READ_FRAMEBUFFER_EXT, tr.deferredRenderFBO->frameBuffer);
			glBindFramebufferEXT(GL_DRAW_FRAMEBUFFER_EXT, tr.occlusionRenderFBO->frameBuffer);
			glBlitFramebufferEXT(0, 0, tr.deferredRenderFBO->width, tr.deferredRenderFBO->height,
								   0, 0, tr.occlusionRenderFBO->width, tr.occlusionRenderFBO->height,
								   GL_DEPTH_BUFFER_BIT,
								   GL_NEAREST);
		}
		else
		{
			// copy depth of the main context to deferredRenderFBO
			glBindFramebufferEXT(GL_READ_FRAMEBUFFER_EXT, 0);
			glBindFramebufferEXT(GL_DRAW_FRAMEBUFFER_EXT, tr.occlusionRenderFBO->frameBuffer);
			glBlitFramebufferEXT(0, 0, glConfig.vidWidth, glConfig.vidHeight,
								   0, 0, glConfig.vidWidth, glConfig.vidHeight,
								   GL_DEPTH_BUFFER_BIT,
								   GL_NEAREST);
		}

		R_BindFBO(tr.occlusionRenderFBO);
		R_AttachFBOTexture2D(GL_TEXTURE_2D, tr.occlusionRenderFBOImage->texnum, 0);

		// clear color buffer
		glClear(GL_COLOR_BUFFER_BIT);

		// remove blend mode
		stateBits = pStage->stateBits;
		stateBits &= ~(GLS_SRCBLEND_BITS | GLS_DSTBLEND_BITS | GLS_DEPTHMASK_TRUE);

		GL_State(stateBits);

		// enable shader, set arrays
		GL_BindProgram(&tr.depthTestShader);
		GL_VertexAttribsState(tr.depthTestShader.attribs);

		// set uniforms
		GLSL_SetUniform_ModelViewProjectionMatrix(&tr.depthTestShader, glState.modelViewProjectionMatrix[glState.stackIndex]);

		// bind u_ColorMap
		GL_SelectTexture(0);
		GL_Bind(pStage->bundle[TB_COLORMAP].image[0]);
		GLSL_SetUniform_ColorTextureMatrix(&tr.depthTestShader, tess.svars.texMatrices[TB_COLORMAP]);

		// bind u_CurrentMap
		GL_SelectTexture(1);
		GL_Bind(tr.currentRenderImage);

		Tess_DrawElements();

		R_BindFBO(previousFBO);

		GL_CheckErrors();

		GLimp_LogComment("--- HEATHAZE FIX END ---\n");
	}

	// remove alpha test
	stateBits = pStage->stateBits;
	stateBits &= ~GLS_ATEST_BITS;
	stateBits &= ~GLS_DEPTHMASK_TRUE;

	GL_State(stateBits);

	// enable shader, set arrays
	GL_BindProgram(&tr.heatHazeShader);
	GL_VertexAttribsState(tr.heatHazeShader.attribs);

	// set uniforms
	GLSL_SetUniform_AlphaTest(&tr.heatHazeShader, pStage->stateBits);

	deformMagnitude = RB_EvalExpression(&pStage->deformMagnitudeExp, 1.0);

	glUniform1fARB(tr.heatHazeShader.u_DeformMagnitude, deformMagnitude);

	GLSL_SetUniform_ModelViewMatrixTranspose(&tr.heatHazeShader, glState.modelViewMatrix[glState.stackIndex]);
	GLSL_SetUniform_ProjectionMatrixTranspose(&tr.heatHazeShader, glState.projectionMatrix[glState.stackIndex]);
	GLSL_SetUniform_ModelViewProjectionMatrix(&tr.heatHazeShader, glState.modelViewProjectionMatrix[glState.stackIndex]);

	if(glConfig2.vboVertexSkinningAvailable)
	{
		GLSL_SetUniform_VertexSkinning(&tr.heatHazeShader, tess.vboVertexSkinning);

		if(tess.vboVertexSkinning)
			glUniformMatrix4fvARB(tr.heatHazeShader.u_BoneMatrix, MAX_BONES, GL_FALSE, &tess.boneMatrices[0][0]);
	}

	// bind u_NormalMap
	GL_SelectTexture(0);
	GL_Bind(pStage->bundle[TB_COLORMAP].image[0]);
	GLSL_SetUniform_NormalTextureMatrix(&tr.heatHazeShader, tess.svars.texMatrices[TB_COLORMAP]);

	// bind u_CurrentMap
	GL_SelectTexture(1);
	if(DS_STANDARD_ENABLED())
	{
		GL_Bind(tr.deferredRenderFBOImage);
	}
	else if(DS_PREPASS_LIGHTING_ENABLED())
	{
#if defined(OFFSCREEN_PREPASS_LIGHTING)
		GL_Bind(tr.deferredRenderFBOImage);
#else
		GL_Bind(tr.currentRenderImage);
		glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, tr.currentRenderImage->uploadWidth, tr.currentRenderImage->uploadHeight);
#endif
	}
	else if(HDR_ENABLED())
	{
		GL_Bind(tr.deferredRenderFBOImage);
	}
	else
	{
		GL_Bind(tr.currentRenderImage);
		glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, tr.currentRenderImage->uploadWidth, tr.currentRenderImage->uploadHeight);
	}

	// bind u_ContrastMap
	if(r_heatHazeFix->integer && glConfig2.framebufferBlitAvailable && glConfig.hardwareType != GLHW_ATI && glConfig.hardwareType != GLHW_ATI_DX10 && glConfig.driverType != GLDRV_MESA)
	{
		GL_SelectTexture(2);
		GL_Bind(tr.occlusionRenderFBOImage);
	}

	Tess_DrawElements();

	GL_CheckErrors();
}

static void Render_liquid(int stage)
{
	vec3_t          viewOrigin;
	float           fogDensity;
	vec3_t          fogColor;
	shaderStage_t  *pStage = tess.surfaceStages[stage];

	GLimp_LogComment("--- Render_liquid ---\n");

	// Tr3B: don't allow blend effects
	GL_State(pStage->stateBits & ~(GLS_SRCBLEND_BITS | GLS_DSTBLEND_BITS | GLS_DEPTHMASK_TRUE));

	// enable shader, set arrays
	GL_BindProgram(&tr.liquidShader);
	GL_VertexAttribsState(tr.liquidShader.attribs);

	// set uniforms
	VectorCopy(backEnd.viewParms.orientation.origin, viewOrigin);	// in world space

	fogDensity = RB_EvalExpression(&pStage->fogDensityExp, 0.001);
	VectorCopy(tess.svars.color, fogColor);

	GLSL_SetUniform_ViewOrigin(&tr.liquidShader, viewOrigin);
	GLSL_SetUniform_RefractionIndex(&tr.liquidShader, RB_EvalExpression(&pStage->refractionIndexExp, 1.0));
	glUniform1fARB(tr.liquidShader.u_FresnelPower, RB_EvalExpression(&pStage->fresnelPowerExp, 2.0));
	glUniform1fARB(tr.liquidShader.u_FresnelScale, RB_EvalExpression(&pStage->fresnelScaleExp, 1.0));
	glUniform1fARB(tr.liquidShader.u_FresnelBias, RB_EvalExpression(&pStage->fresnelBiasExp, 0.05));
	glUniform1fARB(tr.liquidShader.u_NormalScale, RB_EvalExpression(&pStage->normalScaleExp, 0.05));
	glUniform1fARB(tr.liquidShader.u_FogDensity, fogDensity);
	glUniform3fARB(tr.liquidShader.u_FogColor, fogColor[0], fogColor[1], fogColor[2]);

	GLSL_SetUniform_UnprojectMatrix(&tr.liquidShader, backEnd.viewParms.unprojectionMatrix);
	GLSL_SetUniform_ModelMatrix(&tr.liquidShader, backEnd.orientation.transformMatrix);
	GLSL_SetUniform_ModelViewProjectionMatrix(&tr.liquidShader, glState.modelViewProjectionMatrix[glState.stackIndex]);

	// capture current color buffer for u_CurrentMap
	GL_SelectTexture(0);
	if(DS_STANDARD_ENABLED())
	{
		GL_Bind(tr.deferredRenderFBOImage);
	}
	else if(DS_PREPASS_LIGHTING_ENABLED())
	{
#if defined(OFFSCREEN_PREPASS_LIGHTING)
		GL_Bind(tr.deferredRenderFBOImage);
#else
		GL_Bind(tr.currentRenderImage);
		glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, tr.currentRenderImage->uploadWidth, tr.currentRenderImage->uploadHeight);
#endif
	}
	else if(HDR_ENABLED())
	{
		GL_Bind(tr.deferredRenderFBOImage);
	}
	else
	{
		GL_Bind(tr.currentRenderImage);
		glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, tr.currentRenderImage->uploadWidth, tr.currentRenderImage->uploadHeight);
	}

	// bind u_PortalMap
	GL_SelectTexture(1);
	GL_Bind(tr.portalRenderImage);

	// bind u_DepthMap
	GL_SelectTexture(2);
	if(DS_STANDARD_ENABLED())
	{
		GL_Bind(tr.depthRenderImage);
	}
	else if(DS_PREPASS_LIGHTING_ENABLED())
	{
#if defined(OFFSCREEN_PREPASS_LIGHTING)
		GL_Bind(tr.depthRenderImage);
#else
		GL_Bind(tr.depthRenderImage);
		glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, tr.depthRenderImage->uploadWidth, tr.depthRenderImage->uploadHeight);
#endif
	}
	else if(HDR_ENABLED())
	{
		GL_Bind(tr.depthRenderImage);
	}
	else
	{
		// depth texture is not bound to a FBO
		GL_Bind(tr.depthRenderImage);
		glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, tr.depthRenderImage->uploadWidth, tr.depthRenderImage->uploadHeight);
	}

	// bind u_NormalMap
	GL_SelectTexture(3);
	GL_Bind(pStage->bundle[TB_COLORMAP].image[0]);
	GLSL_SetUniform_NormalTextureMatrix(&tr.liquidShader, tess.svars.texMatrices[TB_COLORMAP]);

	Tess_DrawElements();

	GL_CheckErrors();
}



// see Fog Polygon Volumes documentation by Nvidia for further information
static void Render_volumetricFog()
{
#if 0
	vec3_t          viewOrigin;
	float           fogDensity;
	vec3_t          fogColor;

	GLimp_LogComment("--- Render_volumetricFog---\n");

	if(glConfig2.framebufferBlitAvailable)
	{
		FBO_t          *previousFBO;

		previousFBO = glState.currentFBO;

		if(r_deferredShading->integer && glConfig2.framebufferObjectAvailable && glConfig2.textureFloatAvailable &&
			   glConfig2.drawBuffersAvailable && glConfig2.maxDrawBuffers >= 4)
		{
			// copy deferredRenderFBO to occlusionRenderFBO
			glBindFramebufferEXT(GL_READ_FRAMEBUFFER_EXT, tr.deferredRenderFBO->frameBuffer);
			glBindFramebufferEXT(GL_DRAW_FRAMEBUFFER_EXT, tr.occlusionRenderFBO->frameBuffer);
			glBlitFramebufferEXT(0, 0, tr.deferredRenderFBO->width, tr.deferredRenderFBO->height,
								   0, 0, tr.occlusionRenderFBO->width, tr.occlusionRenderFBO->height,
								   GL_DEPTH_BUFFER_BIT,
								   GL_NEAREST);
		}
		else if(r_hdrRendering->integer && glConfig2.framebufferObjectAvailable && glConfig2.textureFloatAvailable)
		{
			// copy deferredRenderFBO to occlusionRenderFBO
			glBindFramebufferEXT(GL_READ_FRAMEBUFFER_EXT, tr.deferredRenderFBO->frameBuffer);
			glBindFramebufferEXT(GL_DRAW_FRAMEBUFFER_EXT, tr.occlusionRenderFBO->frameBuffer);
			glBlitFramebufferEXT(0, 0, tr.deferredRenderFBO->width, tr.deferredRenderFBO->height,
								   0, 0, tr.occlusionRenderFBO->width, tr.occlusionRenderFBO->height,
								   GL_DEPTH_BUFFER_BIT,
								   GL_NEAREST);
		}
		else
		{
			// copy depth of the main context to occlusionRenderFBO
			glBindFramebufferEXT(GL_READ_FRAMEBUFFER_EXT, 0);
			glBindFramebufferEXT(GL_DRAW_FRAMEBUFFER_EXT, tr.occlusionRenderFBO->frameBuffer);
			glBlitFramebufferEXT(0, 0, glConfig.vidWidth, glConfig.vidHeight,
								   0, 0, glConfig.vidWidth, glConfig.vidHeight,
								   GL_DEPTH_BUFFER_BIT,
								   GL_NEAREST);
		}

		// setup shader with uniforms
		GL_BindProgram(&tr.depthToColorShader);
		GL_VertexAttribsState(tr.depthToColorShader.attribs);
		GL_State(0);//GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE);

		GLSL_SetUniform_ModelViewProjectionMatrix(&tr.depthToColorShader, glState.modelViewProjectionMatrix[glState.stackIndex]);

		// Tr3B: might be cool for ghost player effects
		if(glConfig2.vboVertexSkinningAvailable)
		{
			GLSL_SetUniform_VertexSkinning(&tr.depthToColorShader, tess.vboVertexSkinning);

			if(tess.vboVertexSkinning)
				glUniformMatrix4fvARB(tr.depthToColorShader.u_BoneMatrix, MAX_BONES, GL_FALSE, &tess.boneMatrices[0][0]);
		}


		// render back faces
		R_BindFBO(tr.occlusionRenderFBO);
		R_AttachFBOTexture2D(GL_TEXTURE_2D, tr.depthToColorBackFacesFBOImage->texnum, 0);

		GL_ClearColor(0.0f, 0.0f, 0.0f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT);
		GL_Cull(CT_BACK_SIDED);
		Tess_DrawElements();

		// render front faces
		R_AttachFBOTexture2D(GL_TEXTURE_2D, tr.depthToColorFrontFacesFBOImage->texnum, 0);

		glClear(GL_COLOR_BUFFER_BIT);
		GL_Cull(CT_FRONT_SIDED);
		Tess_DrawElements();

		R_BindFBO(previousFBO);






		// enable shader, set arrays
		GL_BindProgram(&tr.volumetricFogShader);
		GL_VertexAttribsState(tr.volumetricFogShader.attribs);

		//GL_State(GLS_DEPTHTEST_DISABLE);	// | GLS_DEPTHMASK_TRUE);
		//GL_State(GLS_DEPTHTEST_DISABLE | GLS_SRCBLEND_ZERO | GLS_DSTBLEND_ONE_MINUS_SRC_COLOR);
		GL_State(GLS_DEPTHTEST_DISABLE | GLS_SRCBLEND_ONE_MINUS_SRC_ALPHA | GLS_DSTBLEND_SRC_ALPHA);
		GL_Cull(CT_TWO_SIDED);

		glVertexAttrib4fvARB(ATTR_INDEX_COLOR, colorWhite);

		// set uniforms
		VectorCopy(backEnd.viewParms.orientation.origin, viewOrigin);	// in world space

		{
			fogDensity = tess.surfaceShader->fogParms.density;
			VectorCopy(tess.surfaceShader->fogParms.color, fogColor);
		}

		GLSL_SetUniform_ModelViewProjectionMatrix(&tr.volumetricFogShader, glState.modelViewProjectionMatrix[glState.stackIndex]);
		GLSL_SetUniform_UnprojectMatrix(&tr.volumetricFogShader, backEnd.viewParms.unprojectionMatrix);

		GLSL_SetUniform_ViewOrigin(&tr.volumetricFogShader, viewOrigin);
		glUniform1fARB(tr.volumetricFogShader.u_FogDensity, fogDensity);
		glUniform3fARB(tr.volumetricFogShader.u_FogColor, fogColor[0], fogColor[1], fogColor[2]);

		// bind u_DepthMap
		GL_SelectTexture(0);
		if(r_deferredShading->integer && glConfig2.framebufferObjectAvailable && glConfig2.textureFloatAvailable &&
				   glConfig2.drawBuffersAvailable && glConfig2.maxDrawBuffers >= 4)
		{
			GL_Bind(tr.depthRenderImage);
		}
		else if(r_hdrRendering->integer && glConfig2.framebufferObjectAvailable && glConfig2.textureFloatAvailable)
		{
			GL_Bind(tr.depthRenderImage);
		}
		else
		{
			// depth texture is not bound to a FBO
			GL_Bind(tr.depthRenderImage);
			glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, tr.depthRenderImage->uploadWidth, tr.depthRenderImage->uploadHeight);
		}

		// bind u_DepthMapBack
		GL_SelectTexture(1);
		GL_Bind(tr.depthToColorBackFacesFBOImage);

		// bind u_DepthMapFront
		GL_SelectTexture(2);
		GL_Bind(tr.depthToColorFrontFacesFBOImage);

		Tess_DrawElements();
	}

	GL_CheckErrors();
#endif
}

/*
===============
Tess_ComputeColor
===============
*/
void Tess_ComputeColor(shaderStage_t * pStage)
{
	float           rgb;
	float           red;
	float           green;
	float           blue;
	float           alpha;

	GLimp_LogComment("--- Tess_ComputeColor ---\n");

	// rgbGen
	switch (pStage->rgbGen)
	{
		case CGEN_IDENTITY:
		{
			tess.svars.color[0] = 1.0;
			tess.svars.color[1] = 1.0;
			tess.svars.color[2] = 1.0;
			tess.svars.color[3] = 1.0;
			break;
		}

		default:
		case CGEN_IDENTITY_LIGHTING:
		{
			tess.svars.color[0] = tr.identityLight;
			tess.svars.color[1] = tr.identityLight;
			tess.svars.color[2] = tr.identityLight;
			tess.svars.color[3] = tr.identityLight;
			break;
		}

		case CGEN_CONST:
		{
			tess.svars.color[0] = pStage->constantColor[0] * (1.0 / 255.0);
			tess.svars.color[1] = pStage->constantColor[1] * (1.0 / 255.0);
			tess.svars.color[2] = pStage->constantColor[2] * (1.0 / 255.0);
			tess.svars.color[3] = pStage->constantColor[3] * (1.0 / 255.0);
			break;
		}

		case CGEN_ENTITY:
		{
			if(backEnd.currentLight)
			{
				tess.svars.color[0] = Q_bound(0.0, backEnd.currentLight->l.color[0], 1.0);
				tess.svars.color[1] = Q_bound(0.0, backEnd.currentLight->l.color[1], 1.0);
				tess.svars.color[2] = Q_bound(0.0, backEnd.currentLight->l.color[2], 1.0);
				tess.svars.color[3] = 1.0;
			}
			else if(backEnd.currentEntity)
			{
				tess.svars.color[0] = Q_bound(0.0, backEnd.currentEntity->e.shaderRGBA[0] * (1.0 / 255.0), 1.0);
				tess.svars.color[1] = Q_bound(0.0, backEnd.currentEntity->e.shaderRGBA[1] * (1.0 / 255.0), 1.0);
				tess.svars.color[2] = Q_bound(0.0, backEnd.currentEntity->e.shaderRGBA[2] * (1.0 / 255.0), 1.0);
				tess.svars.color[3] = Q_bound(0.0, backEnd.currentEntity->e.shaderRGBA[3] * (1.0 / 255.0), 1.0);
			}
			else
			{
				tess.svars.color[0] = 1.0;
				tess.svars.color[1] = 1.0;
				tess.svars.color[2] = 1.0;
				tess.svars.color[3] = 1.0;
			}
			break;
		}

		case CGEN_ONE_MINUS_ENTITY:
		{
			if(backEnd.currentLight)
			{
				tess.svars.color[0] = 1.0 - Q_bound(0.0, backEnd.currentLight->l.color[0], 1.0);
				tess.svars.color[1] = 1.0 - Q_bound(0.0, backEnd.currentLight->l.color[1], 1.0);
				tess.svars.color[2] = 1.0 - Q_bound(0.0, backEnd.currentLight->l.color[2], 1.0);
				tess.svars.color[3] = 0.0;	// FIXME
			}
			else if(backEnd.currentEntity)
			{
				tess.svars.color[0] = 1.0 - Q_bound(0.0, backEnd.currentEntity->e.shaderRGBA[0] * (1.0 / 255.0), 1.0);
				tess.svars.color[1] = 1.0 - Q_bound(0.0, backEnd.currentEntity->e.shaderRGBA[1] * (1.0 / 255.0), 1.0);
				tess.svars.color[2] = 1.0 - Q_bound(0.0, backEnd.currentEntity->e.shaderRGBA[2] * (1.0 / 255.0), 1.0);
				tess.svars.color[3] = 1.0 - Q_bound(0.0, backEnd.currentEntity->e.shaderRGBA[3] * (1.0 / 255.0), 1.0);
			}
			else
			{
				tess.svars.color[0] = 0.0;
				tess.svars.color[1] = 0.0;
				tess.svars.color[2] = 0.0;
				tess.svars.color[3] = 0.0;
			}
			break;
		}

		case CGEN_WAVEFORM:
		{
			float           glow;
			waveForm_t     *wf;

			wf = &pStage->rgbWave;

			if(wf->func == GF_NOISE)
			{
				glow = wf->base + R_NoiseGet4f(0, 0, 0, (backEnd.refdef.floatTime + wf->phase) * wf->frequency) * wf->amplitude;
			}
			else
			{
				glow = RB_EvalWaveForm(wf) * tr.identityLight;
			}

			if(glow < 0)
			{
				glow = 0;
			}
			else if(glow > 1)
			{
				glow = 1;
			}

			tess.svars.color[0] = glow;
			tess.svars.color[1] = glow;
			tess.svars.color[2] = glow;
			tess.svars.color[3] = 1.0;
			break;
		}

		case CGEN_CUSTOM_RGB:
		{
			rgb = Q_bound(0.0, RB_EvalExpression(&pStage->rgbExp, 1.0), 1.0);

			tess.svars.color[0] = rgb;
			tess.svars.color[1] = rgb;
			tess.svars.color[2] = rgb;
			break;
		}

		case CGEN_CUSTOM_RGBs:
		{
			if(backEnd.currentLight)
			{
				red = Q_bound(0.0, RB_EvalExpression(&pStage->redExp, backEnd.currentLight->l.color[0]), 1.0);
				green = Q_bound(0.0, RB_EvalExpression(&pStage->greenExp, backEnd.currentLight->l.color[1]), 1.0);
				blue = Q_bound(0.0, RB_EvalExpression(&pStage->blueExp, backEnd.currentLight->l.color[2]), 1.0);
			}
			else if(backEnd.currentEntity)
			{
				red =
					Q_bound(0.0, RB_EvalExpression(&pStage->redExp, backEnd.currentEntity->e.shaderRGBA[0] * (1.0 / 255.0)), 1.0);
				green =
					Q_bound(0.0, RB_EvalExpression(&pStage->greenExp, backEnd.currentEntity->e.shaderRGBA[1] * (1.0 / 255.0)),
							1.0);
				blue =
					Q_bound(0.0, RB_EvalExpression(&pStage->blueExp, backEnd.currentEntity->e.shaderRGBA[2] * (1.0 / 255.0)),
							1.0);
			}
			else
			{
				red = Q_bound(0.0, RB_EvalExpression(&pStage->redExp, 1.0), 1.0);
				green = Q_bound(0.0, RB_EvalExpression(&pStage->greenExp, 1.0), 1.0);
				blue = Q_bound(0.0, RB_EvalExpression(&pStage->blueExp, 1.0), 1.0);
			}

			tess.svars.color[0] = red;
			tess.svars.color[1] = green;
			tess.svars.color[2] = blue;
			break;
		}
	}

	// alphaGen
	switch (pStage->alphaGen)
	{
		default:
		case AGEN_IDENTITY:
		{
			if(pStage->rgbGen != CGEN_IDENTITY)
			{
				tess.svars.color[3] = 1.0;
			}
			break;
		}

		case AGEN_CONST:
		{
			if(pStage->rgbGen != CGEN_CONST)
			{
				tess.svars.color[3] = pStage->constantColor[3] * (1.0 / 255.0);
			}
			break;
		}

		case AGEN_ENTITY:
		{
			if(backEnd.currentLight)
			{
				tess.svars.color[3] = 1.0;	// FIXME ?
			}
			else if(backEnd.currentEntity)
			{
				tess.svars.color[3] = Q_bound(0.0, backEnd.currentEntity->e.shaderRGBA[3] * (1.0 / 255.0), 1.0);
			}
			else
			{
				tess.svars.color[3] = 1.0;
			}
			break;
		}

		case AGEN_ONE_MINUS_ENTITY:
		{
			if(backEnd.currentLight)
			{
				tess.svars.color[3] = 0.0;	// FIXME ?
			}
			else if(backEnd.currentEntity)
			{
				tess.svars.color[3] = 1.0 - Q_bound(0.0, backEnd.currentEntity->e.shaderRGBA[3] * (1.0 / 255.0), 1.0);
			}
			else
			{
				tess.svars.color[3] = 0.0;
			}
			break;
		}

		case AGEN_WAVEFORM:
		{
			float           glow;
			waveForm_t     *wf;

			wf = &pStage->alphaWave;

			glow = RB_EvalWaveFormClamped(wf);

			tess.svars.color[3] = glow;
			break;
		}

		case AGEN_CUSTOM:
		{
			alpha = Q_bound(0.0, RB_EvalExpression(&pStage->alphaExp, 1.0), 1.0);

			tess.svars.color[3] = alpha;
			break;
		}
	}
}


/*
===============
Tess_ComputeTexMatrices
===============
*/
static void Tess_ComputeTexMatrices(shaderStage_t * pStage)
{
	int             i;
	vec_t          *matrix;

	GLimp_LogComment("--- Tess_ComputeTexMatrices ---\n");

	for(i = 0; i < MAX_TEXTURE_BUNDLES; i++)
	{
		matrix = tess.svars.texMatrices[i];

		RB_CalcTexMatrix(&pStage->bundle[i], matrix);
	}
}


void Tess_StageIteratorDebug()
{
	// log this call
	if(r_logFile->integer)
	{
		// don't just call LogComment, or we will get
		// a call to va() every frame!
		GLimp_LogComment(va("--- Tess_StageIteratorDebug( %i vertices, %i triangles ) ---\n", tess.numVertexes, tess.numIndexes / 3));
	}

	GL_CheckErrors();

	if(!glState.currentVBO || !glState.currentIBO || glState.currentVBO == tess.vbo || glState.currentIBO == tess.ibo)
	{
		// Tr3B: FIXME analyze required vertex attribs by the current material
		Tess_UpdateVBOs(0);
	}

	Tess_DrawElements();
}


void Tess_StageIteratorGeneric()
{
	int             stage;

	// log this call
	if(r_logFile->integer)
	{
		// don't just call LogComment, or we will get
		// a call to va() every frame!
		GLimp_LogComment(va
						 ("--- Tess_StageIteratorGeneric( %s, %i vertices, %i triangles ) ---\n", tess.surfaceShader->name,
						  tess.numVertexes, tess.numIndexes / 3));
	}

	GL_CheckErrors();

	Tess_DeformGeometry();

	if(!glState.currentVBO || !glState.currentIBO || glState.currentVBO == tess.vbo || glState.currentIBO == tess.ibo)
	{
		// Tr3B: FIXME analyze required vertex attribs by the current material
		Tess_UpdateVBOs(0);
	}

	if(tess.surfaceShader->fogVolume)
	{
		Render_volumetricFog();
		return;
	}

	// set face culling appropriately
	GL_Cull(tess.surfaceShader->cullType);

	// set polygon offset if necessary
	if(tess.surfaceShader->polygonOffset)
	{
		glEnable(GL_POLYGON_OFFSET_FILL);
		glPolygonOffset(r_offsetFactor->value, r_offsetUnits->value);
	}

	// call shader function
	for(stage = 0; stage < MAX_SHADER_STAGES; stage++)
	{
		shaderStage_t  *pStage = tess.surfaceStages[stage];

		if(!pStage)
		{
			break;
		}

		if(!RB_EvalExpression(&pStage->ifExp, 1.0))
		{
			continue;
		}

		Tess_ComputeColor(pStage);
		Tess_ComputeTexMatrices(pStage);

		switch (pStage->type)
		{
			case ST_COLORMAP:
			{
				Render_genericSingle(stage);
				break;
			}

#if defined(COMPAT_ET)
			case ST_LIGHTMAP:
			{
				Render_lightMapping(stage, qtrue);
				break;
			}
#endif

			case ST_DIFFUSEMAP:
			case ST_COLLAPSE_lighting_DB:
			case ST_COLLAPSE_lighting_DBS:
			{
				//if(tess.surfaceShader->sort <= SS_OPAQUE)
				{
					if(r_precomputedLighting->integer || r_vertexLighting->integer)
					{
						if(!r_vertexLighting->integer && tess.lightmapNum >= 0 && tess.lightmapNum < tr.lightmaps.currentElements)
						{
							if(tr.worldDeluxeMapping && r_normalMapping->integer)
							{
								Render_deluxeMapping(stage);
							}
							else
							{
								Render_lightMapping(stage, qfalse);
							}
						}
						else if(backEnd.currentEntity != &tr.worldEntity)
						{
							Render_vertexLighting_DBS_entity(stage);
						}
						else
						{
							Render_vertexLighting_DBS_world(stage);
						}

						if(DS_PREPASS_LIGHTING_ENABLED())
						{
							Render_forwardLighting_DBS_post(stage, qfalse);
						}
					}
					else
					{
						if(DS_PREPASS_LIGHTING_ENABLED())
						{
							Render_forwardLighting_DBS_post(stage, qfalse);
						}
						else
						{
							Render_depthFill(stage, qfalse);
						}
					}
				}
				break;
			}

			case ST_COLLAPSE_reflection_CB:
			{
				if(r_reflectionMapping->integer)
				Render_reflection_CB(stage);
				break;
			}

			case ST_REFLECTIONMAP:
			{
				if(r_reflectionMapping->integer)
				Render_reflection_C(stage);
				break;
			}

			case ST_REFRACTIONMAP:
			{
				Render_refraction_C(stage);
				break;
			}

			case ST_DISPERSIONMAP:
			{
				Render_dispersion_C(stage);
				break;
			}

			case ST_SKYBOXMAP:
			{
				Render_skybox(stage);
				break;
			}

			case ST_SCREENMAP:
			{
				Render_screen(stage);
				break;
			}

			case ST_PORTALMAP:
			{
				Render_portal(stage);
				break;
			}


			case ST_HEATHAZEMAP:
			{
				Render_heatHaze(stage);
				break;
			}

			case ST_LIQUIDMAP:
			{
				Render_liquid(stage);
				break;
			}

			default:
				break;
		}

#if defined(COMPAT_ET)
		if(r_showLightMaps->integer && pStage->type == ST_LIGHTMAP)
			break;
#endif
	}

	// reset polygon offset
	if(tess.surfaceShader->polygonOffset)
	{
		glDisable(GL_POLYGON_OFFSET_FILL);
	}
}

void Tess_StageIteratorGBuffer()
{
	int             stage;

	// log this call
	if(r_logFile->integer)
	{
		// don't just call LogComment, or we will get
		// a call to va() every frame!
		GLimp_LogComment(va
						 ("--- Tess_StageIteratorGBuffer( %s, %i vertices, %i triangles ) ---\n", tess.surfaceShader->name,
						  tess.numVertexes, tess.numIndexes / 3));
	}

	GL_CheckErrors();

	Tess_DeformGeometry();

	if(!glState.currentVBO || !glState.currentIBO || glState.currentVBO == tess.vbo || glState.currentIBO == tess.ibo)
	{
		// Tr3B: FIXME analyze required vertex attribs by the current material
		Tess_UpdateVBOs(0);
	}

#if 0
	if(tess.surfaceShader->fogVolume)
	{
		//R_BindFBO(tr.deferredRenderFBO);
		Render_volumetricFog();
		return;
	}
#endif

	// set face culling appropriately
	GL_Cull(tess.surfaceShader->cullType);

	// set polygon offset if necessary
	if(tess.surfaceShader->polygonOffset)
	{
		glEnable(GL_POLYGON_OFFSET_FILL);
		glPolygonOffset(r_offsetFactor->value, r_offsetUnits->value);
	}

	// call shader function
	for(stage = 0; stage < MAX_SHADER_STAGES; stage++)
	{
		shaderStage_t  *pStage = tess.surfaceStages[stage];

		if(!pStage)
		{
			break;
		}

		if(!RB_EvalExpression(&pStage->ifExp, 1.0))
		{
			continue;
		}

		Tess_ComputeColor(pStage);
		Tess_ComputeTexMatrices(pStage);

		switch (pStage->type)
		{
			case ST_COLORMAP:
			{
#if !defined(DEFERRED_SHADING_Z_PREPASS)
				R_BindFBO(tr.deferredRenderFBO);
				Render_genericSingle(stage);
#endif

#if 1
				if(tess.surfaceShader->sort <= SS_OPAQUE && !(pStage->stateBits & (GLS_SRCBLEND_BITS | GLS_DSTBLEND_BITS)))
				{
					if(r_deferredShading->integer == DS_PREPASS_LIGHTING)
					{
#if defined(OFFSCREEN_PREPASS_LIGHTING)
						R_BindFBO(tr.geometricRenderFBO);
#else
						R_BindNullFBO();
#endif
					}
					else
					{
						R_BindFBO(tr.geometricRenderFBO);
					}
					Render_geometricFill_DBS(stage, qtrue);
				}
#endif
				break;
			}

			case ST_DIFFUSEMAP:
			case ST_COLLAPSE_lighting_DB:
			case ST_COLLAPSE_lighting_DBS:
			{
#if !defined(DEFERRED_SHADING_Z_PREPASS)
				if(r_deferredShading->integer == DS_STANDARD)
				{
					R_BindFBO(tr.deferredRenderFBO);
					if(r_precomputedLighting->integer || r_vertexLighting->integer)
					{
						if(!r_vertexLighting->integer && tess.lightmapNum >= 0 && tess.lightmapNum < tr.lightmaps.currentElements)
						{
							if(tr.worldDeluxeMapping && r_normalMapping->integer)
							{
								Render_deluxeMapping(stage);
							}
							else
							{
								Render_lightMapping(stage, qfalse);
							}
						}
						else if(backEnd.currentEntity != &tr.worldEntity)
						{
							Render_vertexLighting_DBS_entity(stage);
						}
						else
						{
							Render_vertexLighting_DBS_world(stage);
						}
					}
					else
					{
						Render_depthFill(stage, qfalse);
					}
				}
#endif

				if(r_deferredShading->integer == DS_PREPASS_LIGHTING)
				{
#if defined(OFFSCREEN_PREPASS_LIGHTING)
					R_BindFBO(tr.geometricRenderFBO);
#else
					R_BindNullFBO();
#endif
				}
				else
				{
					R_BindFBO(tr.geometricRenderFBO);
				}
				Render_geometricFill_DBS(stage, qfalse);
				break;
			}

#if !defined(DEFERRED_SHADING_Z_PREPASS)
			case ST_COLLAPSE_reflection_CB:
			{
				if(r_deferredShading->integer == DS_STANDARD)
				{
					R_BindFBO(tr.deferredRenderFBO);
					Render_reflection_CB(stage);
				}
				break;
			}

			case ST_REFLECTIONMAP:
			{
				if(r_deferredShading->integer == DS_STANDARD)
				{
					R_BindFBO(tr.deferredRenderFBO);
					Render_reflection_C(stage);
				}
				break;
			}

			case ST_REFRACTIONMAP:
			{
				if(r_deferredShading->integer == DS_STANDARD)
				{

					R_BindFBO(tr.deferredRenderFBO);
					Render_refraction_C(stage);
				}
				break;
			}

			case ST_DISPERSIONMAP:
			{
				if(r_deferredShading->integer == DS_STANDARD)
				{
					R_BindFBO(tr.deferredRenderFBO);
					Render_dispersion_C(stage);
				}
				break;
			}

			case ST_SKYBOXMAP:
			{
				if(r_deferredShading->integer == DS_STANDARD)
				{
					R_BindFBO(tr.deferredRenderFBO);
					Render_skybox(stage);
				}
				break;
			}

			case ST_SCREENMAP:
			{
				if(r_deferredShading->integer == DS_STANDARD)
				{
					R_BindFBO(tr.deferredRenderFBO);
					Render_screen(stage);
				}
				break;
			}

			case ST_PORTALMAP:
			{
				if(r_deferredShading->integer == DS_STANDARD)
				{
					R_BindFBO(tr.deferredRenderFBO);
					Render_portal(stage);
				}
				break;
			}

			case ST_HEATHAZEMAP:
			{
				if(r_deferredShading->integer == DS_STANDARD)
				{
					R_BindFBO(tr.deferredRenderFBO);
					Render_heatHaze(stage);
				}
				break;
			}

			case ST_LIQUIDMAP:
			{
				if(r_deferredShading->integer == DS_STANDARD)
				{
					R_BindFBO(tr.deferredRenderFBO);
					Render_liquid(stage);
				}
				break;
			}
#endif

			default:
				break;
		}
	}

	// reset polygon offset
	glDisable(GL_POLYGON_OFFSET_FILL);
}

void Tess_StageIteratorDepthFill()
{
	int             stage;

	// log this call
	if(r_logFile->integer)
	{
		// don't just call LogComment, or we will get
		// a call to va() every frame!
		GLimp_LogComment(va
						 ("--- Tess_StageIteratorDepthFill( %s, %i vertices, %i triangles ) ---\n", tess.surfaceShader->name,
						  tess.numVertexes, tess.numIndexes / 3));
	}

	GL_CheckErrors();

	Tess_DeformGeometry();

	if(!glState.currentVBO || !glState.currentIBO || glState.currentVBO == tess.vbo || glState.currentIBO == tess.ibo)
	{
		Tess_UpdateVBOs(ATTR_POSITION | ATTR_TEXCOORD);
	}

	// set face culling appropriately
	GL_Cull(tess.surfaceShader->cullType);

	// set polygon offset if necessary
	if(tess.surfaceShader->polygonOffset)
	{
		glEnable(GL_POLYGON_OFFSET_FILL);
		glPolygonOffset(r_offsetFactor->value, r_offsetUnits->value);
	}

	// call shader function
	for(stage = 0; stage < MAX_SHADER_STAGES; stage++)
	{
		shaderStage_t  *pStage = tess.surfaceStages[stage];

		if(!pStage)
		{
			break;
		}

		if(!RB_EvalExpression(&pStage->ifExp, 1.0))
		{
			continue;
		}

		Tess_ComputeTexMatrices(pStage);

		switch (pStage->type)
		{
			case ST_COLORMAP:
			{
				if(tess.surfaceShader->sort <= SS_OPAQUE)
				{
					Render_depthFill(stage, qfalse);
				}
				break;
			}

#if defined(COMPAT_ET)
			case ST_LIGHTMAP:
			{
				Render_depthFill(stage, qtrue);
				break;
			}
#endif
			case ST_DIFFUSEMAP:
			case ST_COLLAPSE_lighting_DB:
			case ST_COLLAPSE_lighting_DBS:
			{
				Render_depthFill(stage, qfalse);
				break;
			}

			default:
				break;
		}
	}

	// reset polygon offset
	glDisable(GL_POLYGON_OFFSET_FILL);
}

void Tess_StageIteratorShadowFill()
{
	int             stage;

	// log this call
	if(r_logFile->integer)
	{
		// don't just call LogComment, or we will get
		// a call to va() every frame!
		GLimp_LogComment(va
						 ("--- Tess_StageIteratorShadowFill( %s, %i vertices, %i triangles ) ---\n", tess.surfaceShader->name,
						  tess.numVertexes, tess.numIndexes / 3));
	}

	GL_CheckErrors();

	Tess_DeformGeometry();

	if(!glState.currentVBO || !glState.currentIBO || glState.currentVBO == tess.vbo || glState.currentIBO == tess.ibo)
	{
		Tess_UpdateVBOs(ATTR_POSITION | ATTR_TEXCOORD);
	}

	// set face culling appropriately
	GL_Cull(tess.surfaceShader->cullType);

	// set polygon offset if necessary
	if(tess.surfaceShader->polygonOffset)
	{
		glEnable(GL_POLYGON_OFFSET_FILL);
		glPolygonOffset(r_offsetFactor->value, r_offsetUnits->value);
	}

	// call shader function
	for(stage = 0; stage < MAX_SHADER_STAGES; stage++)
	{
		shaderStage_t  *pStage = tess.surfaceStages[stage];

		if(!pStage)
		{
			break;
		}

		if(!RB_EvalExpression(&pStage->ifExp, 1.0))
		{
			continue;
		}

		Tess_ComputeTexMatrices(pStage);

		switch (pStage->type)
		{
			case ST_COLORMAP:
			{
				if(tess.surfaceShader->sort <= SS_OPAQUE)
				{
					Render_shadowFill(stage);
				}
				break;
			}

#if defined(COMPAT_ET)
			case ST_LIGHTMAP:
#endif
			case ST_DIFFUSEMAP:
			case ST_COLLAPSE_lighting_DB:
			case ST_COLLAPSE_lighting_DBS:
			{
				Render_shadowFill(stage);
				break;
			}

			default:
				break;
		}
	}

	// reset polygon offset
	glDisable(GL_POLYGON_OFFSET_FILL);
}

void Tess_StageIteratorStencilShadowVolume()
{
	// log this call
	if(r_logFile->integer)
	{
		// don't just call LogComment, or we will get
		// a call to va() every frame!
		GLimp_LogComment(va
						 ("--- Tess_StageIteratorStencilShadowVolume( %s, %i vertices, %i triangles ) ---\n",
						  tess.surfaceShader->name, tess.numVertexes, tess.numIndexes / 3));
	}

	GL_CheckErrors();

	if(!glState.currentVBO || !glState.currentIBO || glState.currentVBO == tess.vbo || glState.currentIBO == tess.ibo)
	{
		Tess_UpdateVBOs(ATTR_POSITION);
	}

	if(r_showShadowVolumes->integer)
	{
		//GL_State(GLS_DEPTHMASK_TRUE | GLS_SRCBLEND_SRC_ALPHA | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA);
		GL_State(GLS_DEPTHFUNC_LESS | GLS_SRCBLEND_SRC_ALPHA | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA);
		//GL_State(GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE);
		//GL_State(GLS_DEPTHMASK_TRUE);
#if 1
		GL_Cull(CT_FRONT_SIDED);
		//glColor4f(1.0f, 1.0f, 0.7f, 0.05f);
		glVertexAttrib4fARB(ATTR_INDEX_COLOR, 1.0f, 0.0f, 0.0f, 0.05f);
		Tess_DrawElements();
#endif

#if 1
		GL_Cull(CT_BACK_SIDED);
		glVertexAttrib4fARB(ATTR_INDEX_COLOR, 0.0f, 1.0f, 0.0f, 0.05f);
		Tess_DrawElements();
#endif

#if 1
		GL_State(GLS_DEPTHFUNC_LESS | GLS_POLYMODE_LINE | GLS_SRCBLEND_SRC_ALPHA | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA);
		GL_Cull(CT_TWO_SIDED);
		glVertexAttrib4fARB(ATTR_INDEX_COLOR, 0.0f, 0.0f, 1.0f, 0.05f);
		Tess_DrawElements();
#endif
	}
	else
	{
		if(backEnd.currentEntity->needZFail)
		{
			// mirrors have the culling order reversed
			//if(backEnd.viewParms.isMirror)
			//  GL_FrontFace(GL_CW);

			if(GLEW_ATI_separate_stencil && glConfig2.stencilWrapAvailable)
			{
				GL_Cull(CT_TWO_SIDED);

				glStencilFuncSeparateATI(GL_ALWAYS, GL_ALWAYS, 0, (GLuint) ~ 0);

				glStencilOpSeparateATI(GL_BACK, GL_KEEP, GL_DECR_WRAP_EXT, GL_KEEP);
				glStencilOpSeparateATI(GL_FRONT, GL_KEEP, GL_INCR_WRAP_EXT, GL_KEEP);

				Tess_DrawElements();
			}
			else if(GLEW_EXT_stencil_two_side)
			{
				// render both sides at once
				GL_Cull(CT_TWO_SIDED);

				glEnable(GL_STENCIL_TEST_TWO_SIDE_EXT);

				glActiveStencilFaceEXT(GL_BACK);
				if(glConfig2.stencilWrapAvailable)
				{
					glStencilOp(GL_KEEP, GL_DECR_WRAP_EXT, GL_KEEP);
				}
				else
				{
					glStencilOp(GL_KEEP, GL_DECR, GL_KEEP);
				}

				glActiveStencilFaceEXT(GL_FRONT);
				if(glConfig2.stencilWrapAvailable)
				{
					glStencilOp(GL_KEEP, GL_INCR_WRAP_EXT, GL_KEEP);
				}
				else
				{
					glStencilOp(GL_KEEP, GL_INCR, GL_KEEP);
				}

				Tess_DrawElements();

				glDisable(GL_STENCIL_TEST_TWO_SIDE_EXT);
			}
			else
			{
				// draw only the front faces of the shadow volume
				GL_Cull(CT_FRONT_SIDED);

				// increment the stencil value on zfail
				if(glConfig2.stencilWrapAvailable)
				{
					glStencilOp(GL_KEEP, GL_INCR_WRAP_EXT, GL_KEEP);
				}
				else
				{
					glStencilOp(GL_KEEP, GL_INCR, GL_KEEP);
				}

				Tess_DrawElements();

				// draw only the back faces of the shadow volume
				GL_Cull(CT_BACK_SIDED);

				// decrement the stencil value on zfail
				if(glConfig2.stencilWrapAvailable)
				{
					glStencilOp(GL_KEEP, GL_DECR_WRAP_EXT, GL_KEEP);
				}
				else
				{
					glStencilOp(GL_KEEP, GL_DECR, GL_KEEP);
				}

				Tess_DrawElements();
			}

			//if(backEnd.viewParms.isMirror)
			//  GL_FrontFace(GL_CCW);
		}
		else
		{
			// Tr3B - zpass rendering is cheaper because we can skip the lightcap and darkcap
			// see GPU Gems1 9.5.4

			// mirrors have the culling order reversed
			//if(backEnd.viewParms.isMirror)
			//  GL_FrontFace(GL_CW);

			if(GLEW_ATI_separate_stencil && glConfig2.stencilWrapAvailable)
			{
				GL_Cull(CT_TWO_SIDED);

				glStencilFuncSeparateATI(GL_ALWAYS, GL_ALWAYS, 0, (GLuint) ~ 0);

				glStencilOpSeparateATI(GL_FRONT, GL_KEEP, GL_KEEP, GL_INCR_WRAP_EXT);
				glStencilOpSeparateATI(GL_BACK, GL_KEEP, GL_KEEP, GL_DECR_WRAP_EXT);

				Tess_DrawElements();
			}
			else if(GLEW_EXT_stencil_two_side)
			{
				// render both sides at once
				GL_Cull(CT_TWO_SIDED);

				glEnable(GL_STENCIL_TEST_TWO_SIDE_EXT);

				glActiveStencilFaceEXT(GL_BACK);
				if(glConfig2.stencilWrapAvailable)
				{
					glStencilOp(GL_KEEP, GL_KEEP, GL_INCR_WRAP_EXT);
				}
				else
				{
					glStencilOp(GL_KEEP, GL_KEEP, GL_INCR);
				}

				glActiveStencilFaceEXT(GL_FRONT);
				if(glConfig2.stencilWrapAvailable)
				{
					glStencilOp(GL_KEEP, GL_KEEP, GL_DECR_WRAP_EXT);
				}
				else
				{
					glStencilOp(GL_KEEP, GL_KEEP, GL_DECR);
				}

				Tess_DrawElements();

				glDisable(GL_STENCIL_TEST_TWO_SIDE_EXT);
			}
			else
			{
				// draw only the back faces of the shadow volume
				GL_Cull(CT_BACK_SIDED);

				// increment the stencil value on zpass
				if(glConfig2.stencilWrapAvailable)
				{
					glStencilOp(GL_KEEP, GL_KEEP, GL_INCR_WRAP_EXT);
				}
				else
				{
					glStencilOp(GL_KEEP, GL_KEEP, GL_INCR);
				}

				Tess_DrawElements();

				// draw only the front faces of the shadow volume
				GL_Cull(CT_FRONT_SIDED);

				// decrement the stencil value on zpass
				if(glConfig2.stencilWrapAvailable)
				{
					glStencilOp(GL_KEEP, GL_KEEP, GL_DECR_WRAP_EXT);
				}
				else
				{
					glStencilOp(GL_KEEP, GL_KEEP, GL_DECR);
				}

				Tess_DrawElements();
			}

			//if(backEnd.viewParms.isMirror)
			//  GL_FrontFace(GL_CCW);
		}
	}
}

void Tess_StageIteratorStencilLighting()
{
	int             i, j;
	trRefLight_t   *light;
	shaderStage_t  *attenuationXYStage;
	shaderStage_t  *attenuationZStage;

	// log this call
	if(r_logFile->integer)
	{
		// don't just call LogComment, or we will get
		// a call to va() every frame!
		GLimp_LogComment(va
						 ("--- Tess_StageIteratorStencilLighting( %s, %s, %i vertices, %i triangles ) ---\n",
						  tess.surfaceShader->name, tess.lightShader->name, tess.numVertexes, tess.numIndexes / 3));
	}

	GL_CheckErrors();

	light = backEnd.currentLight;

	Tess_DeformGeometry();

	if(!glState.currentVBO || !glState.currentIBO || glState.currentVBO == tess.vbo || glState.currentIBO == tess.ibo)
	{
		// Tr3B: FIXME analyze required vertex attribs by the current material
		Tess_UpdateVBOs(0);
	}

	// set OpenGL state for lighting
#if 0
	if(!light->additive)
	{
		GL_State(GLS_SRCBLEND_DST_COLOR | GLS_DSTBLEND_ONE | GLS_DEPTHFUNC_EQUAL | GLS_STENCILTEST_ENABLE);
	}
	else
#endif
	{
		if(tess.surfaceShader->sort > SS_OPAQUE)
		{
			GL_State(GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE | GLS_STENCILTEST_ENABLE);
		}
		else
		{
			GL_State(GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE | GLS_DEPTHFUNC_EQUAL | GLS_STENCILTEST_ENABLE);
		}
	}

	// set face culling appropriately
	GL_Cull(tess.surfaceShader->cullType);

	// set polygon offset if necessary
	if(tess.surfaceShader->polygonOffset)
	{
		glEnable(GL_POLYGON_OFFSET_FILL);
		glPolygonOffset(r_offsetFactor->value, r_offsetUnits->value);
	}

	// call shader function
	attenuationZStage = tess.lightShader->stages[0];

	for(i = 0; i < MAX_SHADER_STAGES; i++)
	{
		shaderStage_t  *diffuseStage = tess.surfaceStages[i];

		if(!diffuseStage)
		{
			break;
		}

		if(!RB_EvalExpression(&diffuseStage->ifExp, 1.0))
		{
			continue;
		}

		Tess_ComputeTexMatrices(diffuseStage);

		for(j = 1; j < MAX_SHADER_STAGES; j++)
		{
			attenuationXYStage = tess.lightShader->stages[j];

			if(!attenuationXYStage)
			{
				break;
			}

			if(attenuationXYStage->type != ST_ATTENUATIONMAP_XY)
			{
				continue;
			}

			if(!RB_EvalExpression(&attenuationXYStage->ifExp, 1.0))
			{
				continue;
			}

			Tess_ComputeColor(attenuationXYStage);
			R_ComputeFinalAttenuation(attenuationXYStage, light);

			switch (diffuseStage->type)
			{
				case ST_DIFFUSEMAP:
				case ST_COLLAPSE_lighting_DB:
				case ST_COLLAPSE_lighting_DBS:
					if(light->l.rlType == RL_OMNI)
					{
						Render_forwardLighting_DBS_omni(diffuseStage, attenuationXYStage, attenuationZStage, light);
					}
					else if(light->l.rlType == RL_PROJ)
					{
						Render_forwardLighting_DBS_proj(diffuseStage, attenuationXYStage, attenuationZStage, light);
					}
					else if(light->l.rlType == RL_DIRECTIONAL)
					{
						Render_forwardLighting_DBS_directional(diffuseStage, attenuationXYStage, attenuationZStage, light);
					}
					break;

				default:
					break;
			}
		}
	}

	// reset polygon offset
	if(tess.surfaceShader->polygonOffset)
	{
		glDisable(GL_POLYGON_OFFSET_FILL);
	}
}

void Tess_StageIteratorLighting()
{
	int             i, j;
	trRefLight_t   *light;
	shaderStage_t  *attenuationXYStage;
	shaderStage_t  *attenuationZStage;

	// log this call
	if(r_logFile->integer)
	{
		// don't just call LogComment, or we will get
		// a call to va() every frame!
		GLimp_LogComment(va
						 ("--- Tess_StageIteratorLighting( %s, %s, %i vertices, %i triangles ) ---\n", tess.surfaceShader->name,
						  tess.lightShader->name, tess.numVertexes, tess.numIndexes / 3));
	}

	GL_CheckErrors();

	light = backEnd.currentLight;

	Tess_DeformGeometry();

	if(!glState.currentVBO || !glState.currentIBO || glState.currentVBO == tess.vbo || glState.currentIBO == tess.ibo)
	{
		// Tr3B: FIXME analyze required vertex attribs by the current material
		Tess_UpdateVBOs(0);
	}

	// set OpenGL state for lighting
	if(light->l.inverseShadows)
	{
		GL_State(GLS_SRCBLEND_ZERO | GLS_DSTBLEND_ONE_MINUS_SRC_COLOR);
	}
	else
	{
		if(tess.surfaceShader->sort > SS_OPAQUE)
		{
			GL_State(GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE);
		}
		else
		{
			GL_State(GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE | GLS_DEPTHFUNC_EQUAL);
		}
	}

	// set face culling appropriately
	GL_Cull(tess.surfaceShader->cullType);

	// set polygon offset if necessary
	if(tess.surfaceShader->polygonOffset)
	{
		glEnable(GL_POLYGON_OFFSET_FILL);
		glPolygonOffset(r_offsetFactor->value, r_offsetUnits->value);
	}

	// call shader function
	attenuationZStage = tess.lightShader->stages[0];

	for(i = 0; i < MAX_SHADER_STAGES; i++)
	{
		shaderStage_t  *diffuseStage = tess.surfaceStages[i];

		if(!diffuseStage)
		{
			break;
		}

		if(!RB_EvalExpression(&diffuseStage->ifExp, 1.0))
		{
			continue;
		}

		Tess_ComputeTexMatrices(diffuseStage);

		for(j = 1; j < MAX_SHADER_STAGES; j++)
		{
			attenuationXYStage = tess.lightShader->stages[j];

			if(!attenuationXYStage)
			{
				break;
			}

			if(attenuationXYStage->type != ST_ATTENUATIONMAP_XY)
			{
				continue;
			}

			if(!RB_EvalExpression(&attenuationXYStage->ifExp, 1.0))
			{
				continue;
			}

			Tess_ComputeColor(attenuationXYStage);
			R_ComputeFinalAttenuation(attenuationXYStage, light);

			switch (diffuseStage->type)
			{
				case ST_DIFFUSEMAP:
				case ST_COLLAPSE_lighting_DB:
				case ST_COLLAPSE_lighting_DBS:
					if(light->l.rlType == RL_OMNI)
					{
						Render_forwardLighting_DBS_omni(diffuseStage, attenuationXYStage, attenuationZStage, light);
					}
					else if(light->l.rlType == RL_PROJ)
					{
						if(!light->l.inverseShadows)
						{
							Render_forwardLighting_DBS_proj(diffuseStage, attenuationXYStage, attenuationZStage, light);
						}
					}
					else if(light->l.rlType == RL_DIRECTIONAL)
					{
						//if(!light->l.inverseShadows)
						{
							Render_forwardLighting_DBS_directional(diffuseStage, attenuationXYStage, attenuationZStage, light);
						}
					}
					break;

				default:
					break;
			}
		}
	}

	// reset polygon offset
	if(tess.surfaceShader->polygonOffset)
	{
		glDisable(GL_POLYGON_OFFSET_FILL);
	}
}



/*
=================
Tess_End

Render tesselated data
=================
*/
void Tess_End()
{
	if((tess.numIndexes == 0 || tess.numVertexes == 0) && tess.multiDrawPrimitives == 0)
	{
		return;
	}

	if(tess.indexes[SHADER_MAX_INDEXES - 1] != 0)
	{
		ri.Error(ERR_DROP, "Tess_End() - SHADER_MAX_INDEXES hit");
	}
	if(tess.xyz[SHADER_MAX_VERTEXES - 1][0] != 0)
	{
		ri.Error(ERR_DROP, "Tess_End() - SHADER_MAX_VERTEXES hit");
	}

	// for debugging of sort order issues, stop rendering after a given sort value
	if(r_debugSort->integer && r_debugSort->integer < tess.surfaceShader->sort)
	{
		return;
	}

	// update performance counter
	backEnd.pc.c_batches++;

	GL_CheckErrors();

	// call off to shader specific tess end function
	tess.stageIteratorFunc();

	if(!tess.shadowVolume && 
		(tess.stageIteratorFunc != Tess_StageIteratorShadowFill) &&
		(tess.stageIteratorFunc != Tess_StageIteratorDebug))
	{
		// draw debugging stuff
		if(r_showTris->integer || r_showBatches->integer ||
		   (r_showLightBatches->integer && (tess.stageIteratorFunc == Tess_StageIteratorLighting)))
		{
			DrawTris();
		}
	}

	tess.vboVertexSkinning = qfalse;

	// clear shader so we can tell we don't have any unclosed surfaces
	tess.multiDrawPrimitives = 0;
	tess.numIndexes = 0;
	tess.numVertexes = 0;

	GLimp_LogComment("--- Tess_End ---\n");

	GL_CheckErrors();
}
