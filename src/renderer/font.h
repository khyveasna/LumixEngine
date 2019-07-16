#pragma once


#include "engine/hash_map.h"
#include "engine/delegate_list.h"
#include "engine/resource.h"
#include "engine/resource_manager.h"
#include "renderer/draw2d.h"


namespace Lumix
{


struct Font;
class Renderer;
class Texture;


struct Glyph {
	u32 codepoint;
	float u0, v0, u1, v1;
	float x0, y0, x1, y1;
	float advance_x;
};


LUMIX_RENDERER_API Vec2 measureTextA(const Font& font, const char* str);
LUMIX_RENDERER_API const Glyph* findGlyph(const Font& font, u32 codepoint);


class LUMIX_RENDERER_API FontResource final : public Resource
{
public:
	FontResource(const Path& path, ResourceManager& manager, IAllocator& allocator);

	ResourceType getType() const override { return TYPE; }

	void unload() override { file_data.free(); }
	bool load(u64 size, const u8* mem) override;
	Font* addRef(int font_size);
	void removeRef(Font& font);

	Array<u8> file_data;
	static const ResourceType TYPE;
};


class LUMIX_RENDERER_API FontManager final : public ResourceManager
{
friend class FontResource;
public:
	FontManager(Renderer& renderer, IAllocator& allocator);
	~FontManager();

	Font* getDefaultFont() const { return m_default_font; }
	Texture* getAtlasTexture() const { return m_atlas_texture; }

private:
	Resource* createResource(const Path& path) override;
	void destroyResource(Resource& resource) override;
	void updateFontTexture();
	bool build();

private:
	IAllocator& m_allocator;
	Renderer& m_renderer;
	Font* m_default_font;
	Texture* m_atlas_texture;
	Array<Font*> m_fonts;
};


} // namespace Lumix