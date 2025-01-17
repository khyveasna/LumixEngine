#include "engine/engine.h"
#include "engine/allocator.h"
#include "engine/associative_array.h"
#include "engine/flag_set.h"
#include "engine/input_system.h"
#include "engine/log.h"
#include "engine/os.h"
#include "engine/plugin_manager.h"
#include "engine/reflection.h"
#include "engine/resource_manager.h"
#include "engine/serializer.h"
#include "engine/universe/universe.h"
#include "gui_scene.h"
#include "gui_system.h"
#include "renderer/font.h"
#include "renderer/pipeline.h"
#include "renderer/texture.h"
#include "sprite.h"
#include <cmath>


namespace Lumix
{


static const ComponentType GUI_BUTTON_TYPE = Reflection::getComponentType("gui_button");
static const ComponentType GUI_RECT_TYPE = Reflection::getComponentType("gui_rect");
static const ComponentType GUI_RENDER_TARGET_TYPE = Reflection::getComponentType("gui_render_target");
static const ComponentType GUI_IMAGE_TYPE = Reflection::getComponentType("gui_image");
static const ComponentType GUI_TEXT_TYPE = Reflection::getComponentType("gui_text");
static const ComponentType GUI_INPUT_FIELD_TYPE = Reflection::getComponentType("gui_input_field");
static const float CURSOR_BLINK_PERIOD = 1.0f;
static ffr::TextureHandle EMPTY_RENDER_TARGET = ffr::INVALID_TEXTURE;

struct GUIText
{
	GUIText(IAllocator& allocator) : text("", allocator) {}
	~GUIText() { setFontResource(nullptr); }


	void setFontResource(FontResource* res)
	{
		if (m_font_resource)
		{
			if (m_font)
			{
				m_font_resource->removeRef(*m_font);
				m_font = nullptr;
			}
			m_font_resource->getObserverCb().unbind<GUIText, &GUIText::onFontLoaded>(this);
			m_font_resource->getResourceManager().unload(*m_font_resource);
		}
		m_font_resource = res;
		if (res) res->onLoaded<GUIText, &GUIText::onFontLoaded>(this);
	}


	void onFontLoaded(Resource::State old_state, Resource::State new_state, Resource&)
	{
		if (m_font && new_state != Resource::State::READY)
		{
			m_font_resource->removeRef(*m_font);
			m_font = nullptr;
		}
		if (new_state == Resource::State::READY) m_font = m_font_resource->addRef(m_font_size);
	}

	void setFontSize(int value)
	{
		m_font_size = value;
		if (m_font_resource && m_font_resource->isReady())
		{
			if(m_font) m_font_resource->removeRef(*m_font);
			m_font = m_font_resource->addRef(m_font_size);
		}
	}


	FontResource* getFontResource() const { return m_font_resource; }
	int getFontSize() const { return m_font_size; }
	Font* getFont() const { return m_font; }


	String text;
	GUIScene::TextHAlign horizontal_align = GUIScene::TextHAlign::LEFT;
	u32 color = 0xff000000;

private:
	int m_font_size = 13;
	Font* m_font = nullptr;
	FontResource* m_font_resource = nullptr;
};


struct GUIButton
{
	u32 normal_color = 0xffFFffFF;
	u32 hovered_color = 0xffFFffFF;
};


struct GUIInputField
{
	int cursor = 0;
	float anim = 0;
};


struct GUIImage
{
	enum Flags
	{
		IS_ENABLED = 1 << 1
	};
	Sprite* sprite = nullptr;
	u32 color = 0xffffFFFF;
	FlagSet<Flags, u32> flags;
};


struct GUIRect
{
	enum Flags
	{
		IS_VALID = 1 << 0,
		IS_ENABLED = 1 << 1,
		IS_CLIP = 1 << 2
	};

	struct Anchor
	{
		float points = 0;
		float relative = 0;
	};

	EntityRef entity;
	FlagSet<Flags, u32> flags;
	Anchor top;
	Anchor right = { 0, 1 };
	Anchor bottom = { 0, 1 };
	Anchor left;

	GUIImage* image = nullptr;
	GUIText* text = nullptr;
	GUIInputField* input_field = nullptr;
	ffr::TextureHandle* render_target = nullptr;
};


struct GUISceneImpl final : public GUIScene
{
	GUISceneImpl(GUISystem& system, Universe& context, IAllocator& allocator)
		: m_allocator(allocator)
		, m_universe(context)
		, m_system(system)
		, m_rects(allocator)
		, m_buttons(allocator)
		, m_rect_hovered(allocator)
		, m_rect_hovered_out(allocator)
		, m_button_clicked(allocator)
		, m_buttons_down_count(0)
		, m_canvas_size(800, 600)
	{
		context.registerComponentType(GUI_RECT_TYPE
			, this
			, &GUISceneImpl::createRect
			, &GUISceneImpl::destroyRect
			, &GUISceneImpl::serializeRect
			, &GUISceneImpl::deserializeRect);
		context.registerComponentType(GUI_IMAGE_TYPE
			, this
			, &GUISceneImpl::createImage
			, &GUISceneImpl::destroyImage
			, &GUISceneImpl::serializeImage
			, &GUISceneImpl::deserializeImage);
		context.registerComponentType(GUI_RENDER_TARGET_TYPE
			, this
			, &GUISceneImpl::createRenderTarget
			, &GUISceneImpl::destroyRenderTarget
			, &GUISceneImpl::serializeRenderTarget
			, &GUISceneImpl::deserializeRenderTarget);
		context.registerComponentType(GUI_INPUT_FIELD_TYPE
			, this
			, &GUISceneImpl::createInputField
			, &GUISceneImpl::destroyInputField
			, &GUISceneImpl::serializeInputField
			, &GUISceneImpl::deserializeInputField);
		context.registerComponentType(GUI_TEXT_TYPE
			, this
			, &GUISceneImpl::createText
			, &GUISceneImpl::destroyText
			, &GUISceneImpl::serializeText
			, &GUISceneImpl::deserializeText);
		context.registerComponentType(GUI_BUTTON_TYPE
			, this
			, &GUISceneImpl::createButton
			, &GUISceneImpl::destroyButton
			, &GUISceneImpl::serializeButton
			, &GUISceneImpl::deserializeButton);
		m_font_manager = (FontManager*)system.getEngine().getResourceManager().get(FontResource::TYPE);
	}

	void renderTextCursor(GUIRect& rect, Draw2D& draw, const Vec2& pos)
	{
		if (!rect.input_field) return;
		if (m_focused_entity != rect.entity) return;
		if (rect.input_field->anim > CURSOR_BLINK_PERIOD * 0.5f) return;

		const char* text = rect.text->text.c_str();
		const char* text_end = text + rect.input_field->cursor;
		Font* font = rect.text->getFont();
		float font_size = (float)rect.text->getFontSize();
		Vec2 text_size = measureTextA(*font, text, text_end);
		draw.addLine({ pos.x + text_size.x, pos.y }
			, { pos.x + text_size.x, pos.y + text_size.y }
			, *(Color*)&rect.text->color
			, 1);
	}


	void renderRect(GUIRect& rect, Pipeline& pipeline, const Rect& parent_rect)
	{
		if (!rect.flags.isSet(GUIRect::IS_VALID)) return;
		if (!rect.flags.isSet(GUIRect::IS_ENABLED)) return;

		float l = parent_rect.x + rect.left.points + parent_rect.w * rect.left.relative;
		float r = parent_rect.x + rect.right.points + parent_rect.w * rect.right.relative;
		float t = parent_rect.y + rect.top.points + parent_rect.h * rect.top.relative;
		float b = parent_rect.y + rect.bottom.points + parent_rect.h * rect.bottom.relative;
			 
		Draw2D& draw = pipeline.getDraw2D();
		if (rect.flags.isSet(GUIRect::IS_CLIP)) draw.pushClipRect({ l, t }, { r, b });

		if (rect.image && rect.image->flags.isSet(GUIImage::IS_ENABLED))
		{
			if (rect.image->sprite && rect.image->sprite->getTexture())
			{
				Sprite* sprite = rect.image->sprite;
				Texture* tex = sprite->getTexture();
				if (sprite->type == Sprite::PATCH9)
				{
					struct Quad {
						float l, t, r, b;
					} pos = {
						l + sprite->left,
						t + sprite->top,
						r - tex->width + sprite->right,
						b - tex->height + sprite->bottom
					};
					Quad uvs = {
						sprite->left / (float)tex->width,
						sprite->top / (float)tex->height,
						sprite->right / (float)tex->width,
						sprite->bottom / (float)tex->height
					};

					draw.addImage(&tex->handle, { l, t }, { pos.l, pos.t }, { 0, 0 }, { uvs.l, uvs.t });
					draw.addImage(&tex->handle, { pos.l, t }, { pos.r, pos.t }, { uvs.l, 0 }, { uvs.r, uvs.t });
					draw.addImage(&tex->handle, { pos.r, t }, { r, pos.t }, { uvs.r, 0 }, { 1, uvs.t });

					draw.addImage(&tex->handle, { l, pos.t }, { pos.l, pos.b }, { 0, uvs.t }, { uvs.l, uvs.b });
					draw.addImage(&tex->handle, { pos.l, pos.t }, { pos.r, pos.b }, { uvs.l, uvs.t }, { uvs.r, uvs.b });
					draw.addImage(&tex->handle, { pos.r, pos.t }, { r, pos.b }, { uvs.r, uvs.t }, { 1, uvs.b });

					draw.addImage(&tex->handle, { l, pos.b }, { pos.l, b }, { 0, uvs.b }, { uvs.l, 1 });
					draw.addImage(&tex->handle, { pos.l, pos.b }, { pos.r, b }, { uvs.l, uvs.b }, { uvs.r, 1 });
					draw.addImage(&tex->handle, { pos.r, pos.b }, { r, b }, { uvs.r, uvs.b }, { 1, 1 });

				}
				else
				{
					draw.addImage(&tex->handle, { l, t }, { r, b }, {0, 0}, {1, 1});
				}
			}
			else
			{
				draw.addRectFilled({ l, t }, { r, b }, *(Color*)&rect.image->color);
			}
		}

		if (rect.render_target && rect.render_target->isValid())
		{
			draw.addImage(rect.render_target, { l, t }, { r, b }, {0, 0}, {1, 1});
		}

		if (rect.text)
		{
			Font* font = rect.text->getFont();
			if (font) {
				const char* text_cstr = rect.text->text.c_str();
				float font_size = (float)rect.text->getFontSize();
				Vec2 text_size = measureTextA(*font, text_cstr, nullptr);
				Vec2 text_pos(l, t);

				switch (rect.text->horizontal_align)
				{
					case TextHAlign::LEFT: break;
					case TextHAlign::RIGHT: text_pos.x = r - text_size.x; break;
					case TextHAlign::CENTER: text_pos.x = (r + l - text_size.x) * 0.5f; break;
				}

				draw.addText(*font, text_pos, *(Color*)&rect.text->color, text_cstr);
				renderTextCursor(rect, draw, text_pos);
			}
		}

		EntityPtr child = m_universe.getFirstChild(rect.entity);
		while (child.isValid())
		{
			int idx = m_rects.find((EntityRef)child);
			if (idx >= 0)
			{
				renderRect(*m_rects.at(idx), pipeline, { l, t, r - l, b - t });
			}
			child = m_universe.getNextSibling((EntityRef)child);
		}
		if (rect.flags.isSet(GUIRect::IS_CLIP)) draw.popClipRect();
	}


	void render(Pipeline& pipeline, const Vec2& canvas_size) override
	{
		if (!m_root) return;

		m_canvas_size = canvas_size;
		renderRect(*m_root, pipeline, {0, 0, canvas_size.x, canvas_size.y});
	}


	Vec4 getButtonNormalColorRGBA(EntityRef entity) override
	{
		return ABGRu32ToRGBAVec4(m_buttons[entity].normal_color);
	}


	void setButtonNormalColorRGBA(EntityRef entity, const Vec4& color) override
	{
		m_buttons[entity].normal_color = RGBAVec4ToABGRu32(color);
	}


	Vec4 getButtonHoveredColorRGBA(EntityRef entity) override
	{
		return ABGRu32ToRGBAVec4(m_buttons[entity].hovered_color);
	}


	void setButtonHoveredColorRGBA(EntityRef entity, const Vec4& color) override
	{
		m_buttons[entity].hovered_color = RGBAVec4ToABGRu32(color);
	}


	void enableImage(EntityRef entity, bool enable) override { m_rects[entity]->image->flags.set(GUIImage::IS_ENABLED, enable); }
	bool isImageEnabled(EntityRef entity) override { return m_rects[entity]->image->flags.isSet(GUIImage::IS_ENABLED); }


	Vec4 getImageColorRGBA(EntityRef entity) override
	{
		GUIImage* image = m_rects[entity]->image;
		return ABGRu32ToRGBAVec4(image->color);
	}


	static Vec4 ABGRu32ToRGBAVec4(u32 value)
	{
		float inv = 1 / 255.0f;
		return {
			((value >> 0) & 0xFF) * inv,
			((value >> 8) & 0xFF) * inv,
			((value >> 16) & 0xFF) * inv,
			((value >> 24) & 0xFF) * inv,
		};
	}


	static u32 RGBAVec4ToABGRu32(const Vec4& value)
	{
		u8 r = u8(value.x * 255 + 0.5f);
		u8 g = u8(value.y * 255 + 0.5f);
		u8 b = u8(value.z * 255 + 0.5f);
		u8 a = u8(value.w * 255 + 0.5f);
		return (a << 24) + (b << 16) + (g << 8) + r;
	}


	Path getImageSprite(EntityRef entity) override
	{
		GUIImage* image = m_rects[entity]->image;
		return image->sprite ? image->sprite->getPath() : Path();
	}


	void setImageSprite(EntityRef entity, const Path& path) override
	{
		GUIImage* image = m_rects[entity]->image;
		if (image->sprite)
		{
			image->sprite->getResourceManager().unload(*image->sprite);
		}
		ResourceManagerHub& manager = m_system.getEngine().getResourceManager();
		if (path.isValid())
		{
			image->sprite = manager.load<Sprite>(path);
		}
		else
		{
			image->sprite = nullptr;
		}
	}


	void setImageColorRGBA(EntityRef entity, const Vec4& color) override
	{
		GUIImage* image = m_rects[entity]->image;
		image->color = RGBAVec4ToABGRu32(color);
	}


	bool hasGUI(EntityRef entity) const override
	{
		int idx = m_rects.find(entity);
		if (idx < 0) return false;
		return m_rects.at(idx)->flags.isSet(GUIRect::IS_VALID);
	}


	EntityPtr getRectAt(GUIRect& rect, const Vec2& pos, const Rect& parent_rect) const
	{
		if (!rect.flags.isSet(GUIRect::IS_VALID)) return INVALID_ENTITY;

		Rect r;
		r.x = parent_rect.x + rect.left.points + parent_rect.w * rect.left.relative;
		r.y = parent_rect.y + rect.top.points + parent_rect.h * rect.top.relative;
		float right = parent_rect.x + rect.right.points + parent_rect.w * rect.right.relative;
		float bottom = parent_rect.y + rect.bottom.points + parent_rect.h * rect.bottom.relative;

		r.w = right - r.x;
		r.h = bottom - r.y;

		bool intersect = pos.x >= r.x && pos.y >= r.y && pos.x <= r.x + r.w && pos.y <= r.y + r.h;

		for (EntityPtr child = m_universe.getFirstChild(rect.entity); child.isValid(); child = m_universe.getNextSibling((EntityRef)child))
		{
			int idx = m_rects.find((EntityRef)child);
			if (idx < 0) continue;

			GUIRect* child_rect = m_rects.at(idx);
			EntityPtr entity = getRectAt(*child_rect, pos, r);
			if (entity.isValid()) return entity;
		}

		return intersect ? rect.entity : INVALID_ENTITY;
	}


	EntityPtr getRectAt(const Vec2& pos, const Vec2& canvas_size) const override
	{
		if (!m_root) return INVALID_ENTITY;

		return getRectAt(*m_root, pos, { 0, 0, canvas_size.x, canvas_size.y });
	}


	static Rect getRectOnCanvas(const Rect& parent_rect, GUIRect& rect)
	{
		float l = parent_rect.x + parent_rect.w * rect.left.relative + rect.left.points;
		float r = parent_rect.x + parent_rect.w * rect.right.relative + rect.right.points;
		float t = parent_rect.y + parent_rect.h * rect.top.relative + rect.top.points;
		float b = parent_rect.y + parent_rect.h * rect.bottom.relative + rect.bottom.points;

		return { l, t, r - l, b - t };
	}


	Rect getRect(EntityRef entity) const override
	{
		return getRectOnCanvas(entity, m_canvas_size);
	}


	Rect getRectOnCanvas(EntityPtr entity, const Vec2& canvas_size) const override
	{
		if (!entity.isValid()) return { 0, 0, canvas_size.x, canvas_size.y };
		int idx = m_rects.find((EntityRef)entity);
		if (idx < 0) return { 0, 0, canvas_size.x, canvas_size.y };
		EntityPtr parent = m_universe.getParent((EntityRef)entity);
		Rect parent_rect = getRectOnCanvas(parent, canvas_size);
		GUIRect* gui = m_rects[(EntityRef)entity];
		float l = parent_rect.x + parent_rect.w * gui->left.relative + gui->left.points;
		float r = parent_rect.x + parent_rect.w * gui->right.relative + gui->right.points;
		float t = parent_rect.y + parent_rect.h * gui->top.relative + gui->top.points;
		float b = parent_rect.y + parent_rect.h * gui->bottom.relative + gui->bottom.points;

		return { l, t, r - l, b - t };
	}

	void setRectClip(EntityRef entity, bool enable) override { m_rects[entity]->flags.set(GUIRect::IS_CLIP, enable); }
	bool getRectClip(EntityRef entity) override { return m_rects[entity]->flags.isSet(GUIRect::IS_CLIP); }
	void enableRect(EntityRef entity, bool enable) override { m_rects[entity]->flags.set(GUIRect::IS_ENABLED, enable); }
	bool isRectEnabled(EntityRef entity) override { return m_rects[entity]->flags.isSet(GUIRect::IS_ENABLED); }
	float getRectLeftPoints(EntityRef entity) override { return m_rects[entity]->left.points; }
	void setRectLeftPoints(EntityRef entity, float value) override { m_rects[entity]->left.points = value; }
	float getRectLeftRelative(EntityRef entity) override { return m_rects[entity]->left.relative; }
	void setRectLeftRelative(EntityRef entity, float value) override { m_rects[entity]->left.relative = value; }

	float getRectRightPoints(EntityRef entity) override { return m_rects[entity]->right.points; }
	void setRectRightPoints(EntityRef entity, float value) override { m_rects[entity]->right.points = value; }
	float getRectRightRelative(EntityRef entity) override { return m_rects[entity]->right.relative; }
	void setRectRightRelative(EntityRef entity, float value) override { m_rects[entity]->right.relative = value; }

	float getRectTopPoints(EntityRef entity) override { return m_rects[entity]->top.points; }
	void setRectTopPoints(EntityRef entity, float value) override { m_rects[entity]->top.points = value; }
	float getRectTopRelative(EntityRef entity) override { return m_rects[entity]->top.relative; }
	void setRectTopRelative(EntityRef entity, float value) override { m_rects[entity]->top.relative = value; }

	float getRectBottomPoints(EntityRef entity) override { return m_rects[entity]->bottom.points; }
	void setRectBottomPoints(EntityRef entity, float value) override { m_rects[entity]->bottom.points = value; }
	float getRectBottomRelative(EntityRef entity) override { return m_rects[entity]->bottom.relative; }
	void setRectBottomRelative(EntityRef entity, float value) override { m_rects[entity]->bottom.relative = value; }


	void setTextFontSize(EntityRef entity, int value) override
	{
		GUIText* gui_text = m_rects[entity]->text;
		gui_text->setFontSize(value);
	}
	
	
	int getTextFontSize(EntityRef entity) override
	{
		GUIText* gui_text = m_rects[entity]->text;
		return gui_text->getFontSize();
	}
	
	
	Vec4 getTextColorRGBA(EntityRef entity) override
	{
		GUIText* gui_text = m_rects[entity]->text;
		return ABGRu32ToRGBAVec4(gui_text->color);
	}


	void setTextColorRGBA(EntityRef entity, const Vec4& color) override
	{
		GUIText* gui_text = m_rects[entity]->text;
		gui_text->color = RGBAVec4ToABGRu32(color);
	}


	Path getTextFontPath(EntityRef entity) override
	{
		GUIText* gui_text = m_rects[entity]->text;
		return gui_text->getFontResource() == nullptr ? Path() : gui_text->getFontResource()->getPath();
	}


	void setTextFontPath(EntityRef entity, const Path& path) override
	{
		GUIText* gui_text = m_rects[entity]->text;
		FontResource* res = path.isValid() ? m_font_manager->getOwner().load<FontResource>(path) : nullptr;
		gui_text->setFontResource(res);
	}


	TextHAlign getTextHAlign(EntityRef entity) override
	{
		GUIText* gui_text = m_rects[entity]->text;
		return gui_text->horizontal_align;
		
	}


	void setTextHAlign(EntityRef entity, TextHAlign value) override
	{
		GUIText* gui_text = m_rects[entity]->text;
		gui_text->horizontal_align = value;
	}


	void setText(EntityRef entity, const char* value) override
	{
		GUIText* gui_text = m_rects[entity]->text;
		gui_text->text = value;
	}


	const char* getText(EntityRef entity) override
	{
		GUIText* text = m_rects[entity]->text;
		return text->text.c_str();
	}


	void serializeRect(ISerializer& serializer, EntityRef entity)
	{
		const GUIRect& rect = *m_rects[entity];
		
		serializer.write("flags", rect.flags.base);
		serializer.write("top_pts", rect.top.points);
		serializer.write("top_rel", rect.top.relative);

		serializer.write("right_pts", rect.right.points);
		serializer.write("right_rel", rect.right.relative);

		serializer.write("bottom_pts", rect.bottom.points);
		serializer.write("bottom_rel", rect.bottom.relative);

		serializer.write("left_pts", rect.left.points);
		serializer.write("left_rel", rect.left.relative);
	}


	void deserializeRect(IDeserializer& serializer, EntityRef entity, int /*scene_version*/)
	{
		int idx = m_rects.find(entity);
		GUIRect* rect;
		if (idx >= 0)
		{
			rect = m_rects.at(idx);
		}
		else
		{
			rect = LUMIX_NEW(m_allocator, GUIRect);
			m_rects.insert(entity, rect);
		}
		rect->entity = entity;
		serializer.read(&rect->flags.base);
		serializer.read(&rect->top.points);
		serializer.read(&rect->top.relative);

		serializer.read(&rect->right.points);
		serializer.read(&rect->right.relative);

		serializer.read(&rect->bottom.points);
		serializer.read(&rect->bottom.relative);

		serializer.read(&rect->left.points);
		serializer.read(&rect->left.relative);
		
		m_root = findRoot();
		
		m_universe.onComponentCreated(entity, GUI_RECT_TYPE, this);
	}


	void serializeRenderTarget(ISerializer& serializer, EntityRef entity)
	{
	}


	void deserializeRenderTarget(IDeserializer& serializer, EntityRef entity, int /*scene_version*/)
	{
		int idx = m_rects.find(entity);
		if (idx < 0)
		{
			GUIRect* rect = LUMIX_NEW(m_allocator, GUIRect);
			rect->entity = entity;
			idx = m_rects.insert(entity, rect);
		}
		GUIRect& rect = *m_rects.at(idx);
		rect.render_target = &EMPTY_RENDER_TARGET;
		m_universe.onComponentCreated(entity, GUI_RENDER_TARGET_TYPE, this);
	}


	void serializeButton(ISerializer& serializer, EntityRef entity)
	{
		const GUIButton& button = m_buttons[entity];
		serializer.write("normal_color", button.normal_color);
		serializer.write("hovered_color", button.hovered_color);
	}

	
	void deserializeButton(IDeserializer& serializer, EntityRef entity, int /*scene_version*/)
	{
		GUIButton& button = m_buttons.emplace(entity);
		serializer.read(&button.normal_color);
		serializer.read(&button.hovered_color);
		m_universe.onComponentCreated(entity, GUI_BUTTON_TYPE, this);
	}


	void serializeInputField(ISerializer& serializer, EntityRef entity)
	{
	}


	void deserializeInputField(IDeserializer& serializer, EntityRef entity, int /*scene_version*/)
	{
		int idx = m_rects.find(entity);
		if (idx < 0)
		{
			GUIRect* rect = LUMIX_NEW(m_allocator, GUIRect);
			rect->entity = entity;
			idx = m_rects.insert(entity, rect);
		}
		GUIRect& rect = *m_rects.at(idx);
		rect.input_field = LUMIX_NEW(m_allocator, GUIInputField);

		m_universe.onComponentCreated(entity, GUI_INPUT_FIELD_TYPE, this);
	}


	void serializeImage(ISerializer& serializer, EntityRef entity)
	{
		const GUIRect& rect = *m_rects[entity];
		serializer.write("sprite", rect.image->sprite ? rect.image->sprite->getPath().c_str() : "");
		serializer.write("color", rect.image->color);
		serializer.write("flags", rect.image->flags.base);
	}


	void deserializeImage(IDeserializer& serializer, EntityRef entity, int /*scene_version*/)
	{
		int idx = m_rects.find(entity);
		if (idx < 0)
		{
			GUIRect* rect = LUMIX_NEW(m_allocator, GUIRect);
			rect->entity = entity;
			idx = m_rects.insert(entity, rect);
		}
		GUIRect& rect = *m_rects.at(idx);
		rect.image = LUMIX_NEW(m_allocator, GUIImage);
		
		char tmp[MAX_PATH_LENGTH];
		serializer.read(tmp, lengthOf(tmp));
		if (tmp[0] == '\0')
		{
			rect.image->sprite = nullptr;
		}
		else
		{
			ResourceManagerHub& manager = m_system.getEngine().getResourceManager();
			rect.image->sprite = manager.load<Sprite>(Path(tmp));
		}

		serializer.read(&rect.image->color);
		serializer.read(&rect.image->flags.base);
		
		m_universe.onComponentCreated(entity, GUI_IMAGE_TYPE, this);
	}


	void serializeText(ISerializer& serializer, EntityRef entity)
	{
		const GUIRect& rect = *m_rects[entity];
		serializer.write("font", rect.text->getFontResource() ? rect.text->getFontResource()->getPath().c_str() : "");
		serializer.write("align", (int)rect.text->horizontal_align);
		serializer.write("color", rect.text->color);
		serializer.write("font_size", rect.text->getFontSize());
		serializer.write("text", rect.text->text.c_str());
	}


	void deserializeText(IDeserializer& serializer, EntityRef entity, int /*scene_version*/)
	{
		int idx = m_rects.find(entity);
		if (idx < 0)
		{
			GUIRect* rect = LUMIX_NEW(m_allocator, GUIRect);
			rect->entity = entity;
			idx = m_rects.insert(entity, rect);
		}
		GUIRect& rect = *m_rects.at(idx);
		rect.text = LUMIX_NEW(m_allocator, GUIText)(m_allocator);

		char tmp[MAX_PATH_LENGTH];
		serializer.read(tmp, lengthOf(tmp));
		serializer.read((int*)&rect.text->horizontal_align);
		serializer.read(&rect.text->color);
		int font_size;
		serializer.read(&font_size);
		rect.text->setFontSize(font_size);
		serializer.read(&rect.text->text);
		FontResource* res = tmp[0] ? m_font_manager->getOwner().load<FontResource>(Path(tmp)) : nullptr;
		rect.text->setFontResource(res);

		m_universe.onComponentCreated(entity, GUI_TEXT_TYPE, this);
	}


	void clear() override
	{
		for (GUIRect* rect : m_rects)
		{
			LUMIX_DELETE(m_allocator, rect->input_field);
			LUMIX_DELETE(m_allocator, rect->image);
			LUMIX_DELETE(m_allocator, rect->text);
			LUMIX_DELETE(m_allocator, rect);
		}
		m_rects.clear();
		m_buttons.clear();
	}


	void hoverOut(const GUIRect& rect)
	{
		int idx = m_buttons.find(rect.entity);
		if (idx < 0) return;

		const GUIButton& button = m_buttons.at(idx);

		if (rect.image) rect.image->color = button.normal_color;
		if (rect.text) rect.text->color = button.normal_color;

		m_rect_hovered_out.invoke(rect.entity);
	}


	void hover(const GUIRect& rect)
	{
		int idx = m_buttons.find(rect.entity);
		if (idx < 0) return;

		const GUIButton& button = m_buttons.at(idx);

		if (rect.image) rect.image->color = button.hovered_color;
		if (rect.text) rect.text->color = button.hovered_color;

		m_rect_hovered.invoke(rect.entity);
	}


	void handleMouseAxisEvent(const Rect& parent_rect, GUIRect& rect, const Vec2& mouse_pos, const Vec2& prev_mouse_pos)
	{
		if (!rect.flags.isSet(GUIRect::IS_ENABLED)) return;

		const Rect& r = getRectOnCanvas(parent_rect, rect);

		bool is = contains(r, mouse_pos);
		bool was = contains(r, prev_mouse_pos);
		if (is != was && m_buttons.find(rect.entity) >= 0)
		{
			is ? hover(rect) : hoverOut(rect);
		}

		for (EntityPtr e = m_universe.getFirstChild(rect.entity); e.isValid(); e = m_universe.getNextSibling((EntityRef)e))
		{
			int idx = m_rects.find((EntityRef)e);
			if (idx < 0) continue;
			handleMouseAxisEvent(r, *m_rects.at(idx), mouse_pos, prev_mouse_pos);
		}
	}


	static bool contains(const Rect& rect, const Vec2& pos)
	{
		return pos.x >= rect.x && pos.y >= rect.y && pos.x <= rect.x + rect.w && pos.y <= rect.y + rect.h;
	}


	bool isButtonDown(EntityRef e) const
	{
		for(int i = 0, c = m_buttons_down_count; i < c; ++i)
		{
			if (m_buttons_down[i] == e) return true;
		}
		return false;
	}


	void handleMouseButtonEvent(const Rect& parent_rect, GUIRect& rect, const InputSystem::Event& event)
	{
		if (!rect.flags.isSet(GUIRect::IS_ENABLED)) return;
		bool is_up = event.data.button.state == InputSystem::ButtonEvent::UP;

		Vec2 pos(event.data.button.x_abs, event.data.button.y_abs);
		const Rect& r = getRectOnCanvas(parent_rect, rect);
		
		if (contains(r, pos) && contains(r, m_mouse_down_pos))
		{
			if (m_buttons.find(rect.entity) >= 0)
			{
				if (is_up && isButtonDown(rect.entity))
				{
					m_focused_entity = INVALID_ENTITY;
					m_button_clicked.invoke(rect.entity);
				}
				if (!is_up)
				{
					if (m_buttons_down_count < lengthOf(m_buttons_down))
					{
						m_buttons_down[m_buttons_down_count] = rect.entity;
						++m_buttons_down_count;
					}
					else
					{
						logError("GUI") << "Too many buttons pressed at once";
					}
				}
			}
			
			if (rect.input_field && is_up)
			{
				m_focused_entity = rect.entity;
				if (rect.text)
				{
					rect.input_field->cursor = rect.text->text.length();
					rect.input_field->anim = 0;
				}
			}
		}

		for (EntityPtr e = m_universe.getFirstChild(rect.entity); e.isValid(); e = m_universe.getNextSibling((EntityRef)e))
		{
			int idx = m_rects.find((EntityRef)e);
			if (idx < 0) continue;
			handleMouseButtonEvent(r, *m_rects.at(idx), event);
		}
	}


	GUIRect* getInput(EntityPtr e)
	{
		if (!e.isValid()) return nullptr;

		int rect_idx = m_rects.find((EntityRef)e);
		if (rect_idx < 0) return nullptr;

		GUIRect* rect = m_rects.at(rect_idx);
		if (!rect->text) return nullptr;
		if (!rect->input_field) return nullptr;

		return rect;
	}


	void handleTextInput(const InputSystem::Event& event)
	{
		/*const GUIRect* rect = getInput(m_focused_entity);
		if (!rect) return;
		rect->text->text.insert(rect->input_field->cursor, event.data.text.text);
		rect->input_field->cursor += stringLength(event.data.text.text);*/
		// TODO
		ASSERT(false);
	}


	void handleKeyboardButtonEvent(const InputSystem::Event& event)
	{
		const GUIRect* rect = getInput(m_focused_entity);
		if (!rect) return;
		if (event.data.button.state != InputSystem::ButtonEvent::DOWN) return;

		rect->input_field->anim = 0;

		switch ((OS::Keycode)event.data.button.key_id)
		{
		case OS::Keycode::HOME: rect->input_field->cursor = 0; break;
			case OS::Keycode::END: rect->input_field->cursor = rect->text->text.length(); break;
			case OS::Keycode::BACKSPACE:
				if (rect->text->text.length() > 0 && rect->input_field->cursor > 0)
				{
					rect->text->text.eraseAt(rect->input_field->cursor - 1);
					--rect->input_field->cursor;
				}
				break;
			case OS::Keycode::DEL:
				if (rect->input_field->cursor < rect->text->text.length())
				{
					rect->text->text.eraseAt(rect->input_field->cursor);
				}
				break;
			case OS::Keycode::LEFT:
				if (rect->input_field->cursor > 0) --rect->input_field->cursor;
				break;
			case OS::Keycode::RIGHT:
				if (rect->input_field->cursor < rect->text->text.length()) ++rect->input_field->cursor;
				break;
		}
	}


	void handleInput()
	{
		if (!m_root) return;
		InputSystem& input = m_system.getEngine().getInputSystem();
		const InputSystem::Event* events = input.getEvents();
		int events_count = input.getEventsCount();
		for (int i = 0; i < events_count; ++i)
		{
			const InputSystem::Event& event = events[i];
			switch (event.type)
			{
				case InputSystem::Event::TEXT_INPUT:
					handleTextInput(event);
					break;
				case InputSystem::Event::AXIS:
					if (event.device->type == InputSystem::Device::MOUSE)
					{
						Vec2 pos(event.data.axis.x_abs, event.data.axis.y_abs);
						Vec2 old_pos = pos - Vec2(event.data.axis.x, event.data.axis.y);
						handleMouseAxisEvent({0, 0,  m_canvas_size.x, m_canvas_size.y }, *m_root, pos, old_pos);
					}
					break;
				case InputSystem::Event::BUTTON:
					if (event.device->type == InputSystem::Device::MOUSE)
					{
						if (event.data.button.state == InputSystem::ButtonEvent::DOWN)
						{
							m_mouse_down_pos.x = event.data.button.x_abs;
							m_mouse_down_pos.y = event.data.button.y_abs;
						}
						handleMouseButtonEvent({ 0, 0, m_canvas_size.x, m_canvas_size.y }, *m_root, event);
						if (event.data.button.state == InputSystem::ButtonEvent::UP) m_buttons_down_count = 0;
					}
					else if (event.device->type == InputSystem::Device::KEYBOARD)
					{
						handleKeyboardButtonEvent(event);
					}
					break;
			}
		}
	}


	void blinkCursor(float time_delta)
	{
		GUIRect* rect = getInput(m_focused_entity);
		if (!rect) return;

		rect->input_field->anim += time_delta;
		rect->input_field->anim = fmodf(rect->input_field->anim, CURSOR_BLINK_PERIOD);
	}


	void update(float time_delta, bool paused) override
	{
		if (paused) return;

		handleInput();
		blinkCursor(time_delta);
	}


	void createRect(EntityRef entity)
	{
		int idx = m_rects.find(entity);
		GUIRect* rect;
		if (idx >= 0)
		{
			rect = m_rects.at(idx);
		}
		else
		{
			rect = LUMIX_NEW(m_allocator, GUIRect);
			m_rects.insert(entity, rect);
		}
		rect->entity = entity;
		rect->flags.set(GUIRect::IS_VALID);
		rect->flags.set(GUIRect::IS_ENABLED);
		m_universe.onComponentCreated(entity, GUI_RECT_TYPE, this);
		m_root = findRoot();
	}


	void createText(EntityRef entity)
	{
		int idx = m_rects.find(entity);
		if (idx < 0)
		{
			createRect(entity);
			idx = m_rects.find(entity);
		}
		GUIRect& rect = *m_rects.at(idx);
		rect.text = LUMIX_NEW(m_allocator, GUIText)(m_allocator);

		m_universe.onComponentCreated(entity, GUI_TEXT_TYPE, this);
	}


	void createRenderTarget(EntityRef entity)
	{
		int idx = m_rects.find(entity);
		if (idx < 0)
		{
			createRect(entity);
			idx = m_rects.find(entity);
		}
		m_rects.at(idx)->render_target = &EMPTY_RENDER_TARGET;
		m_universe.onComponentCreated(entity, GUI_RENDER_TARGET_TYPE, this);
	}


	void createButton(EntityRef entity)
	{
		int idx = m_rects.find(entity);
		if (idx < 0)
		{
			createRect(entity);
			idx = m_rects.find(entity);
		}
		GUIImage* image = m_rects.at(idx)->image;
		GUIButton& button = m_buttons.emplace(entity);
		if (image)
		{
			button.hovered_color = image->color;
			button.normal_color = image->color;
		}
		m_universe.onComponentCreated(entity, GUI_BUTTON_TYPE, this);
	}


	void createInputField(EntityRef entity)
	{
		int idx = m_rects.find(entity);
		if (idx < 0)
		{
			createRect(entity);
			idx = m_rects.find(entity);
		}
		GUIRect& rect = *m_rects.at(idx);
		rect.input_field = LUMIX_NEW(m_allocator, GUIInputField);

		m_universe.onComponentCreated(entity, GUI_INPUT_FIELD_TYPE, this);
	}


	void createImage(EntityRef entity)
	{
		int idx = m_rects.find(entity);
		if (idx < 0)
		{
			createRect(entity);
			idx = m_rects.find(entity);
		}
		GUIRect& rect = *m_rects.at(idx);
		rect.image = LUMIX_NEW(m_allocator, GUIImage);
		rect.image->flags.set(GUIImage::IS_ENABLED);

		m_universe.onComponentCreated(entity, GUI_IMAGE_TYPE, this);
	}


	GUIRect* findRoot()
	{
		if (m_rects.size() == 0) return nullptr;
		for (int i = 0, n = m_rects.size(); i < n; ++i)
		{
			GUIRect& rect = *m_rects.at(i);
			if (!rect.flags.isSet(GUIRect::IS_VALID)) continue;
			EntityRef e = m_rects.getKey(i);
			EntityPtr parent = m_universe.getParent(e);
			if (!parent.isValid()) return &rect;
			if (m_rects.find((EntityRef)parent) < 0) return &rect;
		}
		return nullptr;
	}


	void destroyRect(EntityRef entity)
	{
		GUIRect* rect = m_rects[entity];
		rect->flags.set(GUIRect::IS_VALID, false);
		if (rect->image == nullptr && rect->text == nullptr && rect->input_field == nullptr)
		{
			LUMIX_DELETE(m_allocator, rect);
			m_rects.erase(entity);
			
		}
		if (rect == m_root)
		{
			m_root = findRoot();
		}
		m_universe.onComponentDestroyed(entity, GUI_RECT_TYPE, this);
	}


	void destroyButton(EntityRef entity)
	{
		m_buttons.erase(entity);
		m_universe.onComponentDestroyed(entity, GUI_BUTTON_TYPE, this);
	}


	void destroyRenderTarget(EntityRef entity)
	{
		GUIRect* rect = m_rects[entity];
		rect->render_target = nullptr;
		m_universe.onComponentDestroyed(entity, GUI_RENDER_TARGET_TYPE, this);
	}


	void destroyInputField(EntityRef entity)
	{
		GUIRect* rect = m_rects[entity];
		LUMIX_DELETE(m_allocator, rect->input_field);
		rect->input_field = nullptr;
		m_universe.onComponentDestroyed(entity, GUI_INPUT_FIELD_TYPE, this);
	}


	void destroyImage(EntityRef entity)
	{
		GUIRect* rect = m_rects[entity];
		LUMIX_DELETE(m_allocator, rect->image);
		rect->image = nullptr;
		m_universe.onComponentDestroyed(entity, GUI_IMAGE_TYPE, this);
	}


	void destroyText(EntityRef entity)
	{
		GUIRect* rect = m_rects[entity];
		LUMIX_DELETE(m_allocator, rect->text);
		rect->text = nullptr;
		m_universe.onComponentDestroyed(entity, GUI_TEXT_TYPE, this);
	}


	void serialize(OutputMemoryStream& serializer) override
	{
		serializer.write(m_rects.size());
		for (GUIRect* rect : m_rects)
		{
			serializer.write(rect->flags);
			serializer.write(rect->entity);
			serializer.write(rect->top);
			serializer.write(rect->right);
			serializer.write(rect->bottom);
			serializer.write(rect->left);

			serializer.write(rect->image != nullptr);
			if (rect->image)
			{
				serializer.writeString(rect->image->sprite ? rect->image->sprite->getPath().c_str() : "");
				serializer.write(rect->image->color);
				serializer.write(rect->image->flags.base);
			}

			serializer.write(rect->input_field != nullptr);

			serializer.write(rect->text != nullptr);
			if (rect->text)
			{
				serializer.writeString(rect->text->getFontResource() ? rect->text->getFontResource()->getPath().c_str() : "");
				serializer.write(rect->text->horizontal_align);
				serializer.write(rect->text->color);
				serializer.write(rect->text->getFontSize());
				serializer.write(rect->text->text);
			}
		}

		serializer.write(m_buttons.size());
		for (int i = 0, c = m_buttons.size(); i < c; ++i)
		{
			serializer.write(m_buttons.getKey(i));
			const GUIButton& button = m_buttons.at(i);
			serializer.write(button);
		}

	}


	void deserialize(InputMemoryStream& serializer) override
	{
		clear();
		int count = serializer.read<int>();
		for (int i = 0; i < count; ++i)
		{
			GUIRect* rect = LUMIX_NEW(m_allocator, GUIRect);
			serializer.read(rect->flags);
			serializer.read(rect->entity);
			serializer.read(rect->top);
			serializer.read(rect->right);
			serializer.read(rect->bottom);
			serializer.read(rect->left);
			m_rects.insert(rect->entity, rect);
			if (rect->flags.isSet(GUIRect::IS_VALID))
			{
				m_universe.onComponentCreated(rect->entity, GUI_RECT_TYPE, this);
			}

			char tmp[MAX_PATH_LENGTH];
			bool has_image = serializer.read<bool>();
			if (has_image)
			{
				rect->image = LUMIX_NEW(m_allocator, GUIImage);
				serializer.readString(tmp, lengthOf(tmp));
				if (tmp[0] == '\0')
				{
					rect->image->sprite = nullptr;
				}
				else
				{
					ResourceManagerHub& manager = m_system.getEngine().getResourceManager();
					rect->image->sprite = manager.load<Sprite>(Path(tmp));
				}
				serializer.read(rect->image->color);
				serializer.read(rect->image->flags.base);
				m_universe.onComponentCreated(rect->entity, GUI_IMAGE_TYPE, this);

			}
			bool has_input_field = serializer.read<bool>();
			if (has_input_field)
			{
				rect->input_field = LUMIX_NEW(m_allocator, GUIInputField);
				m_universe.onComponentCreated(rect->entity, GUI_INPUT_FIELD_TYPE, this);

			}
			bool has_text = serializer.read<bool>();
			if (has_text)
			{
				rect->text = LUMIX_NEW(m_allocator, GUIText)(m_allocator);
				GUIText& text = *rect->text;
				serializer.readString(tmp, lengthOf(tmp));
				serializer.read(text.horizontal_align);
				serializer.read(text.color);
				int font_size;
				serializer.read(font_size);
				text.setFontSize(font_size);
				serializer.read(text.text);
				FontResource* res = tmp[0] == 0 ? nullptr : m_font_manager->getOwner().load<FontResource>(Path(tmp));
				text.setFontResource(res);
				m_universe.onComponentCreated(rect->entity, GUI_TEXT_TYPE, this);
			}
		}
		count = serializer.read<int>();
		for (int i = 0; i < count; ++i)
		{
			EntityRef e;
			serializer.read(e);
			GUIButton& button = m_buttons.emplace(e);
			serializer.read(button);
		}
		m_root = findRoot();
	}
	

	void setRenderTarget(EntityRef entity, ffr::TextureHandle* texture_handle) override
	{
		m_rects[entity]->render_target = texture_handle;
	}

	
	DelegateList<void(EntityRef)>& buttonClicked() override
	{
		return m_button_clicked;
	}


	DelegateList<void(EntityRef)>& rectHovered() override
	{
		return m_rect_hovered;
	}


	DelegateList<void(EntityRef)>& rectHoveredOut() override
	{
		return m_rect_hovered_out;
	}


	Universe& getUniverse() override { return m_universe; }
	IPlugin& getPlugin() const override { return m_system; }

	IAllocator& m_allocator;
	Universe& m_universe;
	GUISystem& m_system;
	
	AssociativeArray<EntityRef, GUIRect*> m_rects;
	AssociativeArray<EntityRef, GUIButton> m_buttons;
	EntityRef m_buttons_down[16];
	int m_buttons_down_count;
	EntityPtr m_focused_entity = INVALID_ENTITY;
	GUIRect* m_root = nullptr;
	FontManager* m_font_manager = nullptr;
	Vec2 m_canvas_size;
	Vec2 m_mouse_down_pos;
	DelegateList<void(EntityRef)> m_button_clicked;
	DelegateList<void(EntityRef)> m_rect_hovered;
	DelegateList<void(EntityRef)> m_rect_hovered_out;
};


GUIScene* GUIScene::createInstance(GUISystem& system,
	Universe& universe,
	IAllocator& allocator)
{
	return LUMIX_NEW(allocator, GUISceneImpl)(system, universe, allocator);
}


void GUIScene::destroyInstance(GUIScene* scene)
{
	LUMIX_DELETE(static_cast<GUISceneImpl*>(scene)->m_allocator, scene);
}


} // namespace Lumix
