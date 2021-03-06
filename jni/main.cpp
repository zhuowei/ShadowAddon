#include <jni.h>
#include <dlfcn.h>
#include <android/log.h>
#include <stdlib.h>
#include <mcpe.h>
#include <Substrate.h>
#include <GLES2/gl2.h>
#include "mcpelauncher.h"
#include <unordered_map>
#include <cmath>
#include "glm/gtx/string_cast.hpp"

// from the original mod that this was ported from:
// Code written by daxnitro.  Do what you want with it but give me some credit if you use it in whole or in part.
// end original copyright notice.

// also portions from http://www.opengl-tutorial.org/intermediate-tutorials/tutorial-16-shadow-mapping/

#define LOG_TAG "shadow"

#define LOGI(...) ((void)__android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__))

#define GLERROR() do { int err = glGetError(); if (err != 0) { LOGI("GL Error: %i line %i", err, __LINE__);} } while (0)

static bool isShadowPass = false;

static bool hasSetup = false;

// Shadow stuff

// configuration
static int shadowPassInterval   = 1;
static int shadowMapWidth       = 512;
static int shadowMapHeight      = 512;
static float shadowMapHalfPlane = 30.0f;
	
static int shadowPassCounter  = 0;

static bool preShadowPassThirdPersonView;

static GLuint sfb = 0; // Shadow FrameBuffer
static GLuint sfbColorTexture = 0;
static GLuint sfbDepthTexture = 0;
static GLuint sfbRenderBuffer = 0;
static GLuint sfbDepthBuffer  = 0;

static Matrix shadowProjection;
static Matrix shadowProjectionInverse;

static Matrix shadowModelView;
static Matrix shadowModelViewInverse;

// end shadow stuff

static bool useDepthExtension = false; // Tegra sucks

static MinecraftClient* mc;

static void (*GameRenderer_setupCamera_real)(GameRenderer*, float, int);
static void (*GameRenderer_renderLevel_real)(GameRenderer*, float);

// from glm

static void orthoMatrix(Matrix& mat,
		float left,
		float right,
		float bottom,
		float top,
		float zNear,
		float zFar)
{
	mat.m[0][0] = 2.0f / (right - left);
	mat.m[1][1] = 2.0f / (top - bottom);
	mat.m[2][2] = - 2.0f / (zFar - zNear);
	mat.m[3][0] = - (right + left) / (right - left);
	mat.m[3][1] = - (top + bottom) / (top - bottom);
	mat.m[3][2] = - (zFar + zNear) / (zFar - zNear);
}

// end glm

// setup

static void setupShadowRenderTexture() {
	if (shadowPassInterval <= 0) {
		return;
	}

	// depth
	glDeleteTextures(1, &sfbDepthTexture);
	glGenTextures(1, &sfbDepthTexture);
	glBindTexture(GL_TEXTURE_2D, sfbDepthTexture);

	GLERROR();

	glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

	GLERROR();

	if (useDepthExtension) {
		glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT, shadowMapWidth, shadowMapHeight,
			0, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, nullptr);
	} else {
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, shadowMapWidth, shadowMapHeight,
			0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
	}

	GLERROR();
}

static void setupShadowFrameBuffer() {
	if (shadowPassInterval <= 0) {
		return;
	}

	setupShadowRenderTexture();

	glDeleteFramebuffers(1, &sfb);

	glGenFramebuffers(1, &sfb);
	glBindFramebuffer(GL_FRAMEBUFFER, sfb);

	GLERROR();

	// https://forum.qt.io/topic/11451/opengl-es-2-0-the-depth-buffer-texture/6
	// http://forum.unity3d.com/threads/unity-tegra-shadows.167686/

	glDeleteRenderbuffers(1, &sfbDepthBuffer);
	glGenRenderbuffers(1, &sfbDepthBuffer);
	glBindRenderbuffer(GL_RENDERBUFFER, sfbDepthBuffer);

	if (useDepthExtension) {
		glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT, shadowMapWidth, shadowMapHeight);
		glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, sfbDepthBuffer);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, sfbDepthTexture, 0);
	} else {
		glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA, shadowMapWidth, shadowMapHeight);
		glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_RGBA, GL_RENDERBUFFER, sfbDepthBuffer);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, sfbDepthTexture, 0);
	}

	GLERROR();

	int status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	if (status != GL_FRAMEBUFFER_COMPLETE) {
		LOGI("Failed creating shadow framebuffer! (Status %d)", status);
	}
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

// end setup

static void GameRenderer_setupCamera_hook(GameRenderer* self, float f, int mode) {
	GameRenderer_setupCamera_real(self, f, mode);
	if (isShadowPass) {
		glViewport(0, 0, shadowMapWidth, shadowMapHeight);

		shadowProjection = Matrix::IDENTITY;

		orthoMatrix(shadowProjection, -shadowMapHalfPlane, shadowMapHalfPlane, -shadowMapHalfPlane,
			shadowMapHalfPlane, 0.05f, 256.0f);

		shadowModelView = Matrix::IDENTITY;
		shadowModelView.translate(Vec3 {0.0f, 0.0f, -100.0f});
		shadowModelView.rotate(90.0f, Vec3 {1.0f, 0.0f, 0.0f});
		float angle = -(mc->level->getSunAngle(f) / M_PI) * 180.0f;
		if (angle < -90.0 && angle > -270.0) {
			// night time
			shadowModelView.rotate(angle + 180.0f, Vec3 {0.0f, 0.0f, 1.0f});
		} else {
			// day time
			shadowModelView.rotate(angle, Vec3 {0.0f, 0.0f, 1.0f});
		}
		// reduces jitter
		//glTranslatef((float)x % 10.0f - 5.0f, (float)y % 10.0f - 5.0f, (float)z % 10.0f - 5.0f);

		*(MatrixStack::Projection.getTop()) = shadowProjection;
		//shadowProjectionInverse = invertMat4x(shadowProjection);

		*(MatrixStack::View.getTop()) = shadowModelView; // I have no idea.
		//shadowModelViewInverse = invertMat4x(shadowModelView);
	}
}

static void GameRenderer_renderLevel_hook(GameRenderer* self, float partialTicks) {
		isShadowPass = true;
	mc = self->minecraft;
	if (!hasSetup) {
		setupShadowFrameBuffer();
		hasSetup = true;
	}
	if (shadowPassInterval > 0 && --shadowPassCounter <= 0) { 
		// do shadow pass
		Options* options = mc->getOptions();
		preShadowPassThirdPersonView = options->getBooleanValue(&Options::Option::THIRD_PERSON);

		options->set(&Options::Option::THIRD_PERSON, true);

		isShadowPass = true;
		shadowPassCounter = shadowPassInterval;

		glBindFramebuffer(GL_FRAMEBUFFER, sfb);
		
		GameRenderer_renderLevel_real(self, partialTicks);
		
		glFlush();

		glBindFramebuffer(GL_FRAMEBUFFER, 0);

		isShadowPass = false;

		options->set(&Options::Option::THIRD_PERSON, preShadowPassThirdPersonView);
	}
	GameRenderer_renderLevel_real(self, partialTicks);
}

// shader

class ShadowShaderInfo {
public:
	GLuint shadowSamplerUniform;
	GLuint shadowPassUniform;
	GLuint shadowMVPUniform;
	bool hasPopulated;
	ShadowShaderInfo() : shadowSamplerUniform(0), shadowPassUniform(0), shadowMVPUniform(0), hasPopulated(false) {
	};
	void dumpInfo() {
		LOGI("shadowSampler %i shadowPass %i shadowMVP %i",
			shadowSamplerUniform, shadowPassUniform, shadowMVPUniform);
	}
};
static std::unordered_map<GLuint, ShadowShaderInfo> shaderInfo;
static void (*Shader_bind_real)(Shader* self, VertexFormat const&, void* data);

// for porting to iOS, this could just hook glUseProgram and it should be fine...
static void Shader_bind_hook(Shader* self, VertexFormat const& format, void* data) {
	Shader_bind_real(self, format, data);
	GLuint program = self->program;
	ShadowShaderInfo& info = shaderInfo[program];
	if (!info.hasPopulated) {
		info.shadowSamplerUniform = glGetUniformLocation(program, "shadow");
		info.shadowPassUniform = glGetUniformLocation(program, "isShadowPass");
		info.shadowMVPUniform = glGetUniformLocation(program, "shadowMVP");
		LOGI("Populated shader %i", program);
		info.dumpInfo();
		info.hasPopulated = true;
		GLERROR();
	}
	if (info.shadowSamplerUniform > 0) {
		glActiveTexture(GL_TEXTURE7);
		glBindTexture(GL_TEXTURE_2D, sfbDepthTexture);
		glUniform1i(info.shadowSamplerUniform, 7);
		glActiveTexture(GL_TEXTURE0);
	}
	if (info.shadowPassUniform > 0) {
		glUniform1i(info.shadowPassUniform, isShadowPass);
	}
	if (info.shadowMVPUniform > 0) {
		glm::mat4 biasMatrix(
		0.5, 0.0, 0.0, 0.0,
		0.0, 0.5, 0.0, 0.0,
		0.0, 0.0, 0.5, 0.0,
		0.5, 0.5, 0.5, 1.0
		);
		glm::mat4 mvp = biasMatrix * shadowProjection.m * shadowModelView.m * MatrixStack::World.getTop()->m;
		//glm::mat4 mvp = MatrixStack::Projection.getTop()->m * MatrixStack::View.getTop()->m * MatrixStack::World.getTop()->m;
		//LOGI("Mat:\n%s", glm::to_string(mvp).c_str());
		glUniformMatrix4fv(info.shadowMVPUniform, 1, false, &mvp[0][0]);
	}
	// TODO: swap out shaders when doing shadow pass
}

// end shader

JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void* reserved) {
	mcpelauncher_hook((void*) &GameRenderer::renderLevel, (void*) &GameRenderer_renderLevel_hook,
		(void**) &GameRenderer_renderLevel_real);
	mcpelauncher_hook((void*) &GameRenderer::setupCamera, (void*) &GameRenderer_setupCamera_hook,
		(void**) &GameRenderer_setupCamera_real);
	mcpelauncher_hook((void*) &Shader::bind, (void*) &Shader_bind_hook,
		(void**) &Shader_bind_real);
	return JNI_VERSION_1_2;
}
