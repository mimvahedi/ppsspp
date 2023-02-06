#pragma once

#include <mutex>
#include <condition_variable>
#include <vector>
#include <set>

#include "Common/GPU/OpenGL/GLCommon.h"
#include "Common/Data/Collections/Hashmaps.h"

class GLRShader;
class GLRBuffer;
class GLRTexture;
class GLRInputLayout;
class GLRFramebuffer;
class GLPushBuffer;
class GLRProgram;
class GLRenderManager;

class GLDeleter {
public:
	void Perform(GLRenderManager *renderManager, bool skipGLCalls);

	bool IsEmpty() const {
		return shaders.empty() && programs.empty() && buffers.empty() && textures.empty() && inputLayouts.empty() && framebuffers.empty() && pushBuffers.empty();
	}

	void Take(GLDeleter &other);

	std::vector<GLRShader *> shaders;
	std::vector<GLRProgram *> programs;
	std::vector<GLRBuffer *> buffers;
	std::vector<GLRTexture *> textures;
	std::vector<GLRInputLayout *> inputLayouts;
	std::vector<GLRFramebuffer *> framebuffers;
	std::vector<GLPushBuffer *> pushBuffers;
};

// TODO: To be safe, should probably add some more stuff here, like format and even readback count, maybe.
struct GLReadbackKey {
	const GLRFramebuffer *framebuf;
	int width;
	int height;
};

struct GLCachedReadback {
	GLuint buffer;  // PBO
	size_t bufferSize;

	void Destroy(bool skipGLCalls);
};

// Per-frame data, round-robin so we can overlap submission with execution of the previous frame.
struct GLFrameData {
	GLFrameData() : readbacks_(8) {}

	bool skipSwap = false;

	std::mutex fenceMutex;
	std::condition_variable fenceCondVar;
	bool readyForFence = true;

	// Swapchain.
	bool hasBegun = false;

	GLDeleter deleter;
	GLDeleter deleter_prev;
	std::set<GLPushBuffer *> activePushBuffers;

	DenseHashMap<GLReadbackKey, GLCachedReadback *, nullptr> readbacks_;

	void Destroy(bool skipGLCalls);
};
