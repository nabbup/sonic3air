/*
*	Part of the Oxygen Engine / Sonic 3 A.I.R. software distribution.
*	Copyright (C) 2017-2025 by Eukaryot
*
*	Published under the GNU GPLv3 open source software license, see license.txt
*	or https://www.gnu.org/licenses/gpl-3.0.en.html
*/

#include "oxygen/pch.h"

#ifdef RMX_WITH_OPENGL_SUPPORT

#include "oxygen/drawing/opengl/OpenGLDrawer.h"
#include "oxygen/drawing/opengl/OpenGLDrawerResources.h"
#include "oxygen/drawing/opengl/OpenGLDrawerTexture.h"
#include "oxygen/drawing/opengl/OpenGLSpriteTextureManager.h"
#include "oxygen/drawing/opengl/Upscaler.h"
#include "oxygen/drawing/DrawCollection.h"
#include "oxygen/drawing/DrawCommand.h"
#include "oxygen/application/EngineMain.h"
#include "oxygen/helper/Logging.h"
#include "oxygen/rendering/opengl/shaders/SimpleRectColoredShader.h"
#include "oxygen/rendering/opengl/shaders/SimpleRectIndexedShader.h"
#include "oxygen/rendering/opengl/shaders/SimpleRectTexturedShader.h"
#include "oxygen/rendering/opengl/shaders/SimpleRectTexturedUVShader.h"
#include "oxygen/rendering/opengl/shaders/SimpleRectVertexColorShader.h"
#include "oxygen/resources/PaletteCollection.h"
#include "oxygen/resources/SpriteCollection.h"


#if defined(DEBUG) && defined(PLATFORM_WINDOWS)
	// Note that this requires OpenGL 4.3
	#define USE_OPENGL_MESSAGE_CALLBACK
#endif


namespace opengldrawer
{
	void performShaderSourcePostProcessing(String& source, Shader::ShaderType shaderType)
	{
		// Require glsl version 1.50 on Mac
	#ifdef PLATFORM_MAC
		String line;
		int pos = 0;
		while (pos < source.length())
		{
			const int lineStart = pos;
			pos = source.getLine(line, pos);

			// Only versions 130 and 140 are currently used in our shaders, and it's unlikely we'll use other versions before 150, so this should be fine
			line.trimWhitespace();
			int substringOffset = line.findString("#version 130");
			if (substringOffset == -1)
				substringOffset = line.findString("#version 140");

			if (substringOffset != -1)
			{
				// Replace only the one digit, to change the version to "#version 150"
				source[lineStart + substringOffset + 10] = '5';
				return;
			}
		}
	#endif
	}

	bool applyShaderBlendMode(Shader::BlendMode blendMode, OpenGLDrawerResources* resources)
	{
		switch (blendMode)
		{
			case Shader::BlendMode::OPAQUE:		resources->setBlendMode(BlendMode::OPAQUE);		return true;
			case Shader::BlendMode::ALPHA:		resources->setBlendMode(BlendMode::ALPHA);		return true;
			case Shader::BlendMode::ADD:		resources->setBlendMode(BlendMode::ADDITIVE);	return true;
			case Shader::BlendMode::UNDEFINED:	break;
		}
		return false;
	}

#ifdef USE_OPENGL_MESSAGE_CALLBACK
	void GLAPIENTRY openGLMessageCallback(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length, const GLchar* message, const void* userParam)
	{
		const constexpr bool showMessages = true;			// Only errors, or other messages as well?
		const constexpr bool showNotifications = false;		// If showing messages, include notifications as well?

		if (type != GL_DEBUG_TYPE_ERROR)
		{
			if (!showMessages)
				return;
			if (!showNotifications && severity == GL_DEBUG_SEVERITY_NOTIFICATION)
				return;
		}

		const char* titleString = (type == GL_DEBUG_TYPE_ERROR) ? "OpenGL Error" : "OpenGL Message";
		const char* severityString = (severity == GL_DEBUG_SEVERITY_HIGH)		  ? "High" :
									 (severity == GL_DEBUG_SEVERITY_MEDIUM)		  ? "Medium" :
									 (severity == GL_DEBUG_SEVERITY_LOW)		  ? "Low" :
									 (severity == GL_DEBUG_SEVERITY_NOTIFICATION) ? "Notification" : "<unknown>";
		const char* typeString = (type == GL_DEBUG_TYPE_ERROR)				 ? "Error" :
								 (type == GL_DEBUG_TYPE_DEPRECATED_BEHAVIOR) ? "Deprecated Behavior" :
								 (type == GL_DEBUG_TYPE_UNDEFINED_BEHAVIOR)	 ? "Undefined Behavior" :
								 (type == GL_DEBUG_TYPE_PORTABILITY)		 ? "Portability" :
								 (type == GL_DEBUG_TYPE_PERFORMANCE)		 ? "Performance" :
								 (type == GL_DEBUG_TYPE_MARKER)				 ? "Marker" :
								 (type == GL_DEBUG_TYPE_PUSH_GROUP)			 ? "Push Group" :
								 (type == GL_DEBUG_TYPE_POP_GROUP)			 ? "Pop Group" : "<other>";

		RMX_ERROR(titleString << " (severity = " << severityString << ", type = " << typeString << "):\n" << message, );
	}
#endif


	struct Internal
	{
	public:
		Internal() :
			mUpscaler(mResources)
		{
		#if defined(RMX_USE_GLEW)
			// GLEW initialization
			RMX_LOG_INFO("GLEW initialization...");
			const GLenum result = glewInit();
			if (result != GLEW_OK)
			{
				RMX_ERROR("Error in OpenGL initialization (glewInit):\n" << glewGetErrorString(result), );
				return;
			}
		#endif

		#if defined(RMX_USE_GLAD)
			// GLAD initialization
			RMX_LOG_INFO("GLAD initialization...");
			gladLoadGL();
		#endif

			// Register oxygen-specific callback for shader source code post-processing
			//  -> Must be done before loading first shaders in "OpenGLDrawerResources::startup" and "Upscaler::startup"
			Shader::mShaderSourcePostProcessCallback = std::bind(&opengldrawer::performShaderSourcePostProcessing, std::placeholders::_1, std::placeholders::_2);

			// Also register callback for blend mode changes by shaders
			Shader::mShaderApplyBlendModeCallback = std::bind(&opengldrawer::applyShaderBlendMode, std::placeholders::_1, &mResources);

		#ifdef USE_OPENGL_MESSAGE_CALLBACK
			// Register OpenGL message callback for debugging
			glEnable(GL_DEBUG_OUTPUT);
			glDebugMessageCallback(openGLMessageCallback, 0);
		#endif

			// Startup OpenGL drawer resources, including quad VAO and some basic shaders
			RMX_LOG_INFO("OpenGL drawer resources startup");
			mResources.startup();

			// Setup OpenGL defaults
			RMX_LOG_INFO("Setting OpenGL defaults...");
			mResources.setBlendMode(BlendMode::OPAQUE);

			// Startup upscaler
			RMX_LOG_INFO("Upscaler startup");
			mUpscaler.startup();

			mSetupSuccessful = true;
		}

		~Internal()
		{
			if (mSetupSuccessful)
			{
				mUpscaler.shutdown();
				mResources.shutdown();
			}
		}

		OpenGLDrawerTexture* createTexture(DrawerTexture& outTexture)
		{
			OpenGLDrawerTexture* texture = new OpenGLDrawerTexture(outTexture);
			return texture;
		}

		Vec4f getTransformOfRectInViewport(const Recti& inputRect)
		{
			// This transform is the right one to use if vertex positions are normalized inside the given input rectangle
			//  -> I.e. (0.0f, 0.0f) refers to the rect's upper left corner, and (1.0f, 1.0f) to the lower right corner
			Vec4f transform;
			transform.x = mPixelToViewSpaceTransform.x + (float)inputRect.x * mPixelToViewSpaceTransform.z;
			transform.y = mPixelToViewSpaceTransform.y + (float)inputRect.y * mPixelToViewSpaceTransform.w;
			transform.z = (float)inputRect.width * mPixelToViewSpaceTransform.z;
			transform.w = (float)inputRect.height * mPixelToViewSpaceTransform.w;
			return transform;
		}

		inline const Vec4f& getPixelToViewSpaceTransform() const  { return mPixelToViewSpaceTransform; }

		bool mayRenderAnything() const
		{
			return !mInvalidScissorRegion;
		}

		void applySamplingMode()
		{
			switch (mCurrentSamplingMode)
			{
				case SamplingMode::POINT:
				{
					glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
					glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
					break;
				}
				case SamplingMode::BILINEAR:
				{
					glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
					glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
					break;
				}
			}
		}

		void applyWrapMode()
		{
			switch (mCurrentWrapMode)
			{
				case TextureWrapMode::CLAMP:
				{
					glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
					glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
					break;
				}
				case TextureWrapMode::REPEAT:
				{
					glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
					glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
					break;
				}
			}
		}

		GLuint setupTexture(DrawerTexture& drawerTexture)
		{
			OpenGLDrawerTexture* openGLDrawerTexture = drawerTexture.getImplementation<OpenGLDrawerTexture>();
			RMX_CHECK(nullptr != openGLDrawerTexture, "Invalid OpenGL texture implementation", return 0);
			const GLuint textureHandle = openGLDrawerTexture->getTextureHandle();

			if (openGLDrawerTexture->mSamplingMode != mCurrentSamplingMode || openGLDrawerTexture->mWrapMode != mCurrentWrapMode)
			{
				glBindTexture(GL_TEXTURE_2D, textureHandle);
				applySamplingMode();
				applyWrapMode();
				openGLDrawerTexture->mSamplingMode = mCurrentSamplingMode;
				openGLDrawerTexture->mWrapMode = mCurrentWrapMode;
			}
			return textureHandle;
		}

		OpenGLFontOutput& getOpenGLFontOutput(Font& font)
		{
			// Get or create OpenGLFontOutput instance
			OpenGLFontOutput* fontOutput = mapFind(mFontOutputMap, &font);
			if (nullptr != fontOutput)
				return *fontOutput;

			const auto pair = mFontOutputMap.emplace(&font, font);
			return pair.first->second;
		}

		void drawRect(Recti targetRect, GLuint textureHandle, const Color& color, Vec2f uv0 = Vec2f(0.0f, 0.0f), Vec2f uv1 = Vec2f(1.0f, 1.0f))
		{
			const Vec4f transform = getTransformOfRectInViewport(targetRect);
			if (textureHandle != 0)
			{
				const bool needsTintColor = (color != Color::WHITE);

				if (uv0.x == 0.0f && uv0.y == 0.0f && uv1.x == 1.0f && uv1.y == 1.0f)
				{
					SimpleRectTexturedShader& shader = mResources.getSimpleRectTexturedShader(needsTintColor, mResources.getBlendMode() == BlendMode::ALPHA);
					shader.setup(textureHandle, transform, color);
					mResources.getSimpleQuadVAO().draw(GL_TRIANGLES);
				}
				else
				{
					SimpleRectTexturedUVShader& shader = mResources.getSimpleRectTexturedUVShader(needsTintColor, mResources.getBlendMode() == BlendMode::ALPHA);
					shader.setup(textureHandle, transform, color);

					const float vertexData[] =
					{
						0.0f, 0.0f, uv0.x, uv0.y,		// Upper left
						0.0f, 1.0f, uv0.x, uv1.y,		// Lower left
						1.0f, 1.0f, uv1.x, uv1.y,		// Lower right
						1.0f, 1.0f, uv1.x, uv1.y,		// Lower right
						1.0f, 0.0f, uv1.x, uv0.y,		// Upper right
						0.0f, 0.0f, uv0.x, uv0.y		// Upper left
					};

					mMeshVAO.setup(opengl::VertexArrayObject::Format::P2_T2);
					mMeshVAO.updateVertexData(&vertexData[0], 6);
					mMeshVAO.draw(GL_TRIANGLES);
				}
			}
			else
			{
				SimpleRectColoredShader& shader = mResources.getSimpleRectColoredShader();
				shader.setup(color, transform);
				mResources.getSimpleQuadVAO().draw(GL_TRIANGLES);
			}
		}

		void drawIndexed(Recti targetRect, BufferTexture& texture, const OpenGLTexture& paletteTexture, const Color& color)
		{
			if (!texture.isValid())
				return;

			const Vec4f transform = getTransformOfRectInViewport(targetRect);
			const bool needsTintColor = (color != Color::WHITE);

			OpenGLShader::resetLastUsedShader();	// Needed as long as not all shaders are implemented using the OpenGLShader base class
			SimpleRectIndexedShader& shader = mResources.getSimpleRectIndexedShader(needsTintColor, mResources.getBlendMode() == BlendMode::ALPHA);
			shader.setup(texture, paletteTexture, transform, color);
			mResources.getSimpleQuadVAO().draw(GL_TRIANGLES);
		}

		void printText(Font& font, const StringReader& text, const Recti& rect, const DrawerPrintOptions& printOptions)
		{
			OpenGLFontOutput& fontOutput = getOpenGLFontOutput(font);
			const Vec2i pos = font.alignText(rect, text, printOptions.mAlignment);

			static std::vector<Font::TypeInfo> typeInfos;
			typeInfos.clear();
			font.getTypeInfos(typeInfos, pos, text, printOptions.mSpacing);
			if (typeInfos.empty())
				return;

			// Simple culling by checking the bounding box before rendering
			// TODO: This is not particularly precise, as it's not considering the real impact of effects (outlines, shadows, etc.) - instead, we're using a fixed tolerance value
			{
				const constexpr int TOLERANCE = 10;
				Vec2i boundingBoxMin(+0x7fffffff, +0x7fffffff);
				Vec2i boundingBoxMax(-0x7fffffff, -0x7fffffff);
				for (const Font::TypeInfo& typeInfo : typeInfos)
				{
					if (nullptr != typeInfo.mBitmap)
					{
						boundingBoxMin.x = std::min(boundingBoxMin.x, typeInfo.mPosition.x);
						boundingBoxMin.y = std::min(boundingBoxMin.y, typeInfo.mPosition.y);
						boundingBoxMax.x = std::max(boundingBoxMax.x, typeInfo.mPosition.x + typeInfo.mBitmap->getWidth());
						boundingBoxMax.y = std::max(boundingBoxMax.y, typeInfo.mPosition.y + typeInfo.mBitmap->getHeight());
					}
				}
				if (boundingBoxMin.x >= mCurrentViewport.x + mCurrentViewport.width + TOLERANCE || boundingBoxMax.x <= mCurrentViewport.x - TOLERANCE ||
					boundingBoxMin.y >= mCurrentViewport.y + mCurrentViewport.height + TOLERANCE || boundingBoxMax.y <= mCurrentViewport.y - TOLERANCE)
					return;
			}

			static OpenGLFontOutput::VertexGroups vertexGroups;
			fontOutput.buildVertexGroups(vertexGroups, typeInfos);

			SimpleRectTexturedUVShader& shader = mResources.getSimpleRectTexturedUVShader(true, true);
			for (const OpenGLFontOutput::VertexGroup& vertexGroup : vertexGroups.mVertexGroups)
			{
				shader.setup(vertexGroup.mTexture->getHandle(), getPixelToViewSpaceTransform(), printOptions.mTintColor);

				static std::vector<float> vertexData;
				vertexData.resize(vertexGroup.mNumVertices * 4);
				for (size_t i = 0; i < vertexGroup.mNumVertices; ++i)
				{
					const OpenGLFontOutput::Vertex& src = vertexGroups.mVertices[vertexGroup.mStartIndex + i];
					float* dst = &vertexData[i * 4];
					dst[0] = src.mPosition.x;
					dst[1] = src.mPosition.y;
					dst[2] = src.mTexcoords.x;
					dst[3] = src.mTexcoords.y;
				}

				mMeshVAO.setup(opengl::VertexArrayObject::Format::P2_T2);
				mMeshVAO.updateVertexData(&vertexData[0], vertexGroup.mNumVertices);
				mMeshVAO.draw(GL_TRIANGLES);
			}
		}

	public:
		bool mSetupSuccessful = false;
		SDL_Window* mOutputWindow = nullptr;

		OpenGLDrawerResources mResources;
		Upscaler mUpscaler;
		OpenGLSpriteTextureManager mSpriteTextureManager;

		Recti mCurrentViewport;
		Vec4f mPixelToViewSpaceTransform;	// Transformation from pixel-based coordinates view space, in the form: (x, y) = offset; (z, w) = scale

		SamplingMode mCurrentSamplingMode = SamplingMode::POINT;
		TextureWrapMode mCurrentWrapMode = TextureWrapMode::CLAMP;

		std::vector<Recti> mScissorStack;
		bool mInvalidScissorRegion = false;

		opengl::VertexArrayObject mMeshVAO;			// Always using the same instances with different contents -- TODO: Some kind of caching could be useful

	private:
		std::unordered_map<Font*, OpenGLFontOutput> mFontOutputMap;
	};
}



OpenGLDrawer::OpenGLDrawer() :
	mInternal(*new opengldrawer::Internal())
{
}

OpenGLDrawer::~OpenGLDrawer()
{
	delete &mInternal;
}

bool OpenGLDrawer::wasSetupSuccessful()
{
	return mInternal.mSetupSuccessful;
}

void OpenGLDrawer::updateDrawer(float deltaSeconds)
{
	mInternal.mResources.refresh(deltaSeconds);
}

void OpenGLDrawer::createTexture(DrawerTexture& outTexture)
{
	outTexture.setImplementation(mInternal.createTexture(outTexture));
}

void OpenGLDrawer::refreshTexture(DrawerTexture& texture)
{
	createTexture(texture);
}

void OpenGLDrawer::setupRenderWindow(SDL_Window* window)
{
	mInternal.mOutputWindow = window;
}

void OpenGLDrawer::performRendering(const DrawCollection& drawCollection)
{
	for (DrawCommand* drawCommand : drawCollection.getDrawCommands())
	{
		switch (drawCommand->getType())
		{
			case DrawCommand::Type::UNDEFINED:
			{
				RMX_ERROR("Got invalid draw command", );
				continue;
			}

			case DrawCommand::Type::SET_WINDOW_RENDER_TARGET:
			{
				SetWindowRenderTargetDrawCommand& dc = drawCommand->as<SetWindowRenderTargetDrawCommand>();

				// Bind no frame buffer at all
				glBindFramebuffer(GL_FRAMEBUFFER, 0);

				const Recti& viewport = dc.mViewport;
				mInternal.mCurrentViewport = viewport;
				glViewport_Recti(viewport);

				// Flip vertically
				mInternal.mPixelToViewSpaceTransform.x = -1.0f;
				mInternal.mPixelToViewSpaceTransform.y = 1.0f;
				mInternal.mPixelToViewSpaceTransform.z = 2.0f / (float)viewport.width;
				mInternal.mPixelToViewSpaceTransform.w = -2.0f / (float)viewport.height;
				break;
			}

			case DrawCommand::Type::SET_RENDER_TARGET:
			{
				SetRenderTargetDrawCommand& dc = drawCommand->as<SetRenderTargetDrawCommand>();

				// Bind as frame buffer
				OpenGLDrawerTexture& drawerTexture = *dc.mTexture->getImplementation<OpenGLDrawerTexture>();
				glBindFramebuffer(GL_FRAMEBUFFER, drawerTexture.getFrameBufferHandle());

				const Recti& viewport = dc.mViewport;
				mInternal.mCurrentViewport = viewport;
				glViewport_Recti(viewport);

				mInternal.mPixelToViewSpaceTransform.x = -1.0f;
				mInternal.mPixelToViewSpaceTransform.y = -1.0f;
				mInternal.mPixelToViewSpaceTransform.z = 2.0f / (float)viewport.width;
				mInternal.mPixelToViewSpaceTransform.w = 2.0f / (float)viewport.height;
				break;
			}

			case DrawCommand::Type::RECT:
			{
				if (!mInternal.mayRenderAnything())
					break;

				RectDrawCommand& dc = drawCommand->as<RectDrawCommand>();
				GLuint textureHandle = 0;
				if (nullptr != dc.mTexture)
				{
					textureHandle = mInternal.setupTexture(*dc.mTexture);
				}

				mInternal.drawRect(dc.mRect, textureHandle, dc.mColor, dc.mUV0, dc.mUV1);
				break;
			}

			case DrawCommand::Type::UPSCALED_RECT:
			{
				if (!mInternal.mayRenderAnything())
					break;

				UpscaledRectDrawCommand& dc = drawCommand->as<UpscaledRectDrawCommand>();
				mInternal.mUpscaler.renderImage(dc.mRect, dc.mTexture->getImplementation<OpenGLDrawerTexture>()->getTextureHandle(), dc.mTexture->getSize());
				break;
			}

			case DrawCommand::Type::SPRITE:
			{
				SpriteDrawCommand& sc = drawCommand->as<SpriteDrawCommand>();
				const SpriteCollection::Item* item = SpriteCollection::instance().getSprite(sc.mSpriteKey);
				if (nullptr == item)
					break;

				SpriteBase& sprite = *item->mSprite;
				Vec2i offset = sprite.mOffset;
				Vec2i size = sprite.getSize();
				if (sc.mScale.x != 1.0f || sc.mScale.y != 1.0f)
				{
					offset.x = roundToInt((float)offset.x * sc.mScale.x);
					offset.y = roundToInt((float)offset.y * sc.mScale.y);
					size.x = roundToInt((float)size.x * sc.mScale.x);
					size.y = roundToInt((float)size.y * sc.mScale.y);
				}
				const Recti targetRect(sc.mPosition + offset, size);

				if (item->mUsesComponentSprite)
				{
					const OpenGLTexture* texture = mInternal.mSpriteTextureManager.getComponentSpriteTexture(*item);
					if (nullptr == texture)
						break;

					// TODO: Cache sampling mode for the texture?
					//  -> That requires the sprite texture manager to store (more high level) OpenGLDrawerTexture instead of OpenGLTexture instances
					glBindTexture(GL_TEXTURE_2D, texture->getHandle());
					mInternal.applySamplingMode();

					mInternal.drawRect(targetRect, texture->getHandle(), sc.mTintColor);
				}
				else
				{
					BufferTexture* texture = mInternal.mSpriteTextureManager.getPaletteSpriteTexture(*item, false);
					if (nullptr == texture)
						break;

					const PaletteBase* palette = PaletteCollection::instance().getPalette(sc.mPaletteKey, 0);
					if (nullptr == palette)
						break;

					const OpenGLTexture& paletteTexture = mInternal.mResources.getCustomPaletteTexture(*palette, *palette);
					mInternal.drawIndexed(targetRect, *texture, paletteTexture, sc.mTintColor);
				}
				break;
			}

			case DrawCommand::Type::SPRITE_RECT:
			{
				SpriteRectDrawCommand& sc = drawCommand->as<SpriteRectDrawCommand>();
				const SpriteCollection::Item* item = SpriteCollection::instance().getSprite(sc.mSpriteKey);
				if (nullptr == item)
					break;
				if (!item->mUsesComponentSprite)
					break;

				OpenGLTexture* texture = mInternal.mSpriteTextureManager.getComponentSpriteTexture(*item);
				if (nullptr == texture)
					break;

				// TODO: Cache sampling mode for the texture?
				//  -> That requires the sprite texture manager to store (more high level) OpenGLDrawerTexture instead of OpenGLTexture instances
				glBindTexture(GL_TEXTURE_2D, texture->getHandle());
				mInternal.applySamplingMode();

				mInternal.drawRect(sc.mRect, texture->getHandle(), sc.mTintColor);
				break;
			}

			case DrawCommand::Type::MESH:
			{
				if (!mInternal.mayRenderAnything())
					break;

				MeshDrawCommand& dc = drawCommand->as<MeshDrawCommand>();
				if (dc.mTriangles.empty())
					break;
				if (nullptr == dc.mTexture)
					break;

				SimpleRectTexturedUVShader& shader = mInternal.mResources.getSimpleRectTexturedUVShader(false, true);
				shader.setup(mInternal.setupTexture(*dc.mTexture), mInternal.getPixelToViewSpaceTransform());

				static std::vector<float> vertexData;
				vertexData.resize(dc.mTriangles.size() * 4);
				for (size_t i = 0; i < dc.mTriangles.size(); ++i)
				{
					const DrawerMeshVertex& src = dc.mTriangles[i];
					float* dst = &vertexData[i * 4];
					dst[0] = src.mPosition.x;
					dst[1] = src.mPosition.y;
					dst[2] = src.mTexcoords.x;
					dst[3] = src.mTexcoords.y;
				}

				mInternal.mMeshVAO.setup(opengl::VertexArrayObject::Format::P2_T2);
				mInternal.mMeshVAO.updateVertexData(&vertexData[0], dc.mTriangles.size());
				mInternal.mMeshVAO.draw(GL_TRIANGLES);
				break;
			}

			case DrawCommand::Type::MESH_VERTEX_COLOR:
			{
				if (!mInternal.mayRenderAnything())
					break;

				MeshVertexColorDrawCommand& dc = drawCommand->as<MeshVertexColorDrawCommand>();
				if (dc.mTriangles.empty())
					break;

				SimpleRectVertexColorShader& shader = mInternal.mResources.getSimpleRectVertexColorShader();
				shader.setup(mInternal.getPixelToViewSpaceTransform());

				static std::vector<float> vertexData;
				vertexData.resize(dc.mTriangles.size() * 6);
				for (size_t i = 0; i < dc.mTriangles.size(); ++i)
				{
					const DrawerMeshVertex_P2_C4& src = dc.mTriangles[i];
					float* dst = &vertexData[i * 6];
					dst[0] = src.mPosition.x;
					dst[1] = src.mPosition.y;
					dst[2] = src.mColor.r;
					dst[3] = src.mColor.g;
					dst[4] = src.mColor.b;
					dst[5] = src.mColor.a;
				}

				mInternal.mMeshVAO.setup(opengl::VertexArrayObject::Format::P2_C4);
				mInternal.mMeshVAO.updateVertexData(&vertexData[0], dc.mTriangles.size());
				mInternal.mMeshVAO.draw(GL_TRIANGLES);
				break;
			}

			case DrawCommand::Type::SET_BLEND_MODE:
			{
				SetBlendModeDrawCommand& dc = drawCommand->as<SetBlendModeDrawCommand>();
				mInternal.mResources.setBlendMode(dc.mBlendMode);
				break;
			}

			case DrawCommand::Type::SET_SAMPLING_MODE:
			{
				SetSamplingModeDrawCommand& dc = drawCommand->as<SetSamplingModeDrawCommand>();
				mInternal.mCurrentSamplingMode = dc.mSamplingMode;
				break;
			}

			case DrawCommand::Type::SET_WRAP_MODE:
			{
				SetWrapModeDrawCommand& dc = drawCommand->as<SetWrapModeDrawCommand>();
				mInternal.mCurrentWrapMode = dc.mWrapMode;
				break;
			}

			case DrawCommand::Type::PRINT_TEXT:
			{
				if (!mInternal.mayRenderAnything())
					break;

				PrintTextDrawCommand& dc = drawCommand->as<PrintTextDrawCommand>();
				mInternal.printText(*dc.mFont, dc.mText, dc.mRect, dc.mPrintOptions);
				break;
			}

			case DrawCommand::Type::PRINT_TEXT_W:
			{
				if (!mInternal.mayRenderAnything())
					break;

				PrintTextWDrawCommand& dc = drawCommand->as<PrintTextWDrawCommand>();
				mInternal.printText(*dc.mFont, dc.mText, dc.mRect, dc.mPrintOptions);
				break;
			}

			case DrawCommand::Type::PUSH_SCISSOR:
			{
				PushScissorDrawCommand& dc = drawCommand->as<PushScissorDrawCommand>();

				Recti scissorRect = dc.mRect;
				if (mInternal.mScissorStack.empty())
				{
					glEnable(GL_SCISSOR_TEST);
				}
				else
				{
					scissorRect.intersect(mInternal.mScissorStack.back());
				}
				mInternal.mScissorStack.emplace_back(scissorRect);

				glScissor(scissorRect.x, scissorRect.y, std::max(scissorRect.width, 0), std::max(scissorRect.height, 0));
				mInternal.mInvalidScissorRegion = scissorRect.empty();
				break;
			}

			case DrawCommand::Type::POP_SCISSOR:
			{
				mInternal.mScissorStack.pop_back();
				if (mInternal.mScissorStack.empty())
				{
					glDisable(GL_SCISSOR_TEST);
					mInternal.mInvalidScissorRegion = false;
				}
				else
				{
					const Recti scissorRect = mInternal.mScissorStack.back();
					glScissor(scissorRect.x, scissorRect.y, std::max(scissorRect.width, 0), std::max(scissorRect.height, 0));
					mInternal.mInvalidScissorRegion = scissorRect.empty();
				}
				break;
			}
		}
	}
}

void OpenGLDrawer::presentScreen()
{
	SDL_GL_SwapWindow(mInternal.mOutputWindow);
}

OpenGLDrawerResources& OpenGLDrawer::getResources()
{
	return mInternal.mResources;
}

#endif
