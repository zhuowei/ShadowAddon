#include <jni.h>
#include <dlfcn.h>
#include <android/log.h>
#include <stdlib.h>
#include <mcpe.h>
#include <Substrate.h>
#include <GLES2/gl2.h>

// from the original mod that this was ported from:
// Code written by daxnitro.  Do what you want with it but give me some credit if you use it in whole or in part.
// end original copyright notice.

#define LOG_TAG "shadow"

#define LOGI(...) ((void)__android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__))

static bool isShadowPass = false;

// Shadow stuff

// configuration
static int shadowPassInterval   = 4;
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

static MinecraftClient* mc;

static void (*GameRenderer_renderLevel_real)(GameRenderer*, float);

static void GameRenderer_renderLevel_hook(GameRenderer* self, float partialTicks) {
	mc = self->minecraft;
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

		isShadowPass = false;

		options->set(&Options::Option::THIRD_PERSON, preShadowPassThirdPersonView);
	}
	GameRenderer_renderLevel_real(self, partialTicks);
}

JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void* reserved) {
	
	return JNI_VERSION_1_2;
}
