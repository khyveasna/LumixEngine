#include "editor/asset_browser.h"
#include "editor/asset_compiler.h"
#include "editor/settings.h"
#include "editor/studio_app.h"
#include "editor/utils.h"
#include "editor/world_editor.h"
#include "engine/crc32.h"
#include "engine/engine.h"
#include "engine/geometry.h"
#include "engine/log.h"
#include "engine/math.h"
#include "engine/os.h"
#include "engine/path.h"
#include "engine/plugin_manager.h"
#include "engine/reflection.h"
#include "engine/serializer.h"
#include "engine/universe/universe.h"
#include "gui/gui_scene.h"
#include "gui/sprite.h"
#include "imgui/imgui.h"
#include "renderer/draw2d.h"
#include "renderer/ffr/ffr.h"
#include "renderer/pipeline.h"
#include "renderer/renderer.h"
#include "renderer/texture.h"

using namespace Lumix;


namespace
{


static const ComponentType GUI_RECT_TYPE = Reflection::getComponentType("gui_rect");
static const ComponentType GUI_IMAGE_TYPE = Reflection::getComponentType("gui_image");
static const ComponentType GUI_TEXT_TYPE = Reflection::getComponentType("gui_text");
static const ComponentType GUI_BUTTON_TYPE = Reflection::getComponentType("gui_button");
static const ComponentType GUI_RENDER_TARGET_TYPE = Reflection::getComponentType("gui_render_target");


struct SpritePlugin final : public AssetBrowser::IPlugin
{
	SpritePlugin(StudioApp& app) 
		: app(app) 
	{
		app.getAssetCompiler().registerExtension("spr", Sprite::TYPE);
	}

	bool canCreateResource() const override { return true; }
	const char* getFileDialogFilter() const { return "Sprite\0*.spr\0"; }
	const char* getFileDialogExtensions() const { return "spr"; }
	const char* getDefaultExtension() const override { return "spr"; }

	bool createResource(const char* path) override {
		OS::OutputFile file;
		WorldEditor& editor = app.getWorldEditor();
		if (!file.open(path)) {
			logError("GUI") << "Failed to create " << path;
			return false;
		}

		file << "{ \"type\" : \"simple\" }";
		file.close();
		return true;
	}


	void onGUI(Resource* resource) override
	{
		Sprite* sprite = (Sprite*)resource;
		
		if (ImGui::Button("Save")) saveSprite(*sprite);
		ImGui::SameLine();
		if (ImGui::Button("Open in external editor")) app.getAssetBrowser().openInExternalEditor(sprite);

		char tmp[MAX_PATH_LENGTH];
		Texture* tex = sprite->getTexture();
		copyString(tmp, tex ? tex->getPath().c_str() : "");
		if (app.getAssetBrowser().resourceInput("Texture", "texture", tmp, lengthOf(tmp), Texture::TYPE))
		{
			sprite->setTexture(Path(tmp));
		}

		static const char* TYPES_STR[] = { "9 patch", "Simple" };
		if (ImGui::BeginCombo("Type", TYPES_STR[sprite->type]))
		{
			if (ImGui::Selectable("9 patch")) sprite->type = Sprite::Type::PATCH9;
			if (ImGui::Selectable("Simple")) sprite->type = Sprite::Type::SIMPLE;
			ImGui::EndCombo();
		}
		switch (sprite->type)
		{
			case Sprite::Type::PATCH9:
				ImGui::InputInt("Top", &sprite->top);
				ImGui::InputInt("Right", &sprite->right);
				ImGui::InputInt("Bottom", &sprite->bottom);
				ImGui::InputInt("Left", &sprite->left);
				patch9edit(sprite);
				break;
			case Sprite::Type::SIMPLE: break;
			default: ASSERT(false); break;
		}
	}



	void patch9edit(Sprite* sprite)
	{
		Texture* texture = sprite->getTexture();

		if (sprite->type != Sprite::Type::PATCH9 || !texture || !texture->isReady()) return;
		ImVec2 size;
		size.x = minimum(ImGui::GetContentRegionAvailWidth(), texture->width * 2.0f);
		size.y = size.x / texture->width * texture->height;
		float scale = size.x / texture->width;
		ImGui::Dummy(size);

		ImDrawList* draw = ImGui::GetWindowDrawList();
		ImVec2 a = ImGui::GetItemRectMin();
		ImVec2 b = ImGui::GetItemRectMax();
		draw->AddImage(&texture->handle, a, b);

		auto drawHandle = [&](const char* id, const ImVec2& a, const ImVec2& b, int* value, bool vertical) {
			const float SIZE = 5;
			ImVec2 rect_pos((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
			if (vertical)
			{
				rect_pos.x = a.x + (sprite->left + sprite->right) * 0.5f * scale;
			}
			else
			{
				rect_pos.y = a.y + (sprite->top + sprite->bottom) * 0.5f * scale;
			}

			ImGui::SetCursorScreenPos({ rect_pos.x - SIZE, rect_pos.y - SIZE });
			ImGui::InvisibleButton(id, { SIZE * 2, SIZE * 2 });
			bool changed = false;
			if (ImGui::IsItemActive())
			{
				static int start_drag_value;
				if (ImGui::IsMouseDragging())
				{
					ImVec2 drag = ImGui::GetMouseDragDelta();
					if (vertical)
					{
						*value = int(start_drag_value + drag.y / scale);
					}
					else
					{
						*value = int(start_drag_value + drag.x / scale);
					}
				}
				else if (ImGui::IsMouseClicked(0))
				{
					start_drag_value = *value;
				}
				changed = true;
			}


			bool is_hovered = ImGui::IsItemHovered();
			draw->AddLine(a, b, 0xffff00ff);
			draw->AddRectFilled(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), is_hovered ? 0xffffffff : 0x77ffFFff);
			draw->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), 0xff777777);

			return changed;
		};

		ImVec2 cp = ImGui::GetCursorScreenPos();
		drawHandle("left", { a.x + sprite->left * scale, a.y }, { a.x + sprite->left * scale, b.y }, &sprite->left, false);
		drawHandle("right", { a.x + sprite->right * scale, a.y }, { a.x + sprite->right * scale, b.y }, &sprite->right, false);
		drawHandle("top", { a.x, a.y + sprite->top * scale }, { b.x, a.y + sprite->top * scale }, &sprite->top, true);
		drawHandle("bottom", { a.x, a.y + sprite->bottom * scale }, { b.x, a.y + sprite->bottom * scale }, &sprite->bottom, true);
		ImGui::SetCursorScreenPos(cp);
	}


	void saveSprite(Sprite& sprite)
	{
		if (OutputMemoryStream* file = app.getAssetBrowser().beginSaveResource(sprite))
		{
			struct : ISaveEntityGUIDMap {
				EntityGUID get(EntityPtr entity) override { return INVALID_ENTITY_GUID; }
			} dummy_map;
			TextSerializer serializer(*file, dummy_map);
			bool success = true;
			if (!sprite.save(serializer))
			{
				success = false;
				logError("Editor") << "Could not save file " << sprite.getPath().c_str();
			}
			app.getAssetBrowser().endSaveResource(sprite, *file, success);
		}
	}


	void onResourceUnloaded(Resource* resource) override {}
	const char* getName() const override { return "Sprite"; }
	ResourceType getResourceType() const override { return Sprite::TYPE; }


	StudioApp& app;
};


class GUIEditor final : public StudioApp::GUIPlugin
{
enum class EdgeMask
{
	LEFT = 1 << 0,
	RIGHT = 1 << 1,
	TOP = 1 << 2,
	BOTTOM = 1 << 3,
	CENTER_HORIZONTAL = 1 << 4,
	CENTER_VERTICAL = 1 << 5,
	ALL = LEFT | RIGHT | TOP | BOTTOM,
	HORIZONTAL = LEFT | RIGHT,
	VERTICAL = TOP | BOTTOM
};

public:
	GUIEditor(StudioApp& app)
		: m_app(app)
	{
		IAllocator& allocator = app.getWorldEditor().getAllocator();

		Action* action = LUMIX_NEW(allocator, Action)("GUI Editor", "Toggle gui editor", "gui_editor");
		action->func.bind<GUIEditor, &GUIEditor::onAction>(this);
		action->is_selected.bind<GUIEditor, &GUIEditor::isOpen>(this);
		app.addWindowAction(action);

		m_editor = &app.getWorldEditor();
		Renderer& renderer = *static_cast<Renderer*>(m_editor->getEngine().getPluginManager().getPlugin("renderer"));
		PipelineResource* pres = m_editor->getEngine().getResourceManager().load<PipelineResource>(Path("pipelines/gui_editor.pln"));
		m_pipeline = Pipeline::create(renderer, pres, "", allocator);

		m_editor->universeCreated().bind<GUIEditor, &GUIEditor::onUniverseChanged>(this);
		m_editor->universeDestroyed().bind<GUIEditor, &GUIEditor::onUniverseChanged>(this);
	}


	~GUIEditor()
	{
		m_editor->universeCreated().unbind<GUIEditor, &GUIEditor::onUniverseChanged>(this);
		m_editor->universeDestroyed().unbind<GUIEditor, &GUIEditor::onUniverseChanged>(this);
		Pipeline::destroy(m_pipeline);
	}


private:
	enum class MouseMode
	{
		NONE,
		RESIZE,
		MOVE
	};

	void onSettingsLoaded() override {
		m_is_window_open = m_app.getSettings().getValue("is_gui_editor_open", false);
	}
	void onBeforeSettingsSaved() override {
		m_app.getSettings().setValue("is_gui_editor_open", m_is_window_open);
	}

	void onAction() { m_is_window_open = !m_is_window_open; }
	bool isOpen() const { return m_is_window_open; }


	void onUniverseChanged()
	{
		Universe* universe = m_editor->getUniverse();
		if (!universe)
		{
			m_pipeline->setScene(nullptr);
			return;
		}
		RenderScene* scene = (RenderScene*)universe->getScene(crc32("renderer"));
		m_pipeline->setScene(scene);
	}


	MouseMode drawGizmo(Draw2D& draw, GUIScene& scene, const Vec2& canvas_size, const ImVec2& mouse_canvas_pos)
	{
		auto& selected_entities = m_editor->getSelectedEntities();
		if (selected_entities.size() != 1) return MouseMode::NONE;

		EntityRef e = selected_entities[0];
		if (!scene.hasGUI(e)) return MouseMode::NONE;

		GUIScene::Rect& rect = scene.getRectOnCanvas(e, canvas_size);
		Vec2 bottom_right = { rect.x + rect.w, rect.y + rect.h };
		draw.addRect({ rect.x, rect.y }, bottom_right, {0xff, 0xf0, 0x0f, 0xff}, 1);
		Vec2 mid = { rect.x + rect.w * 0.5f, rect.y + rect.h * 0.5f };

		auto drawHandle = [&](const Vec2& pos, const ImVec2& mouse_pos) {
			const float SIZE = 5;
			float dx = pos.x - mouse_pos.x;
			float dy = pos.y - mouse_pos.y;
			bool is_hovered = abs(dx) < SIZE && abs(dy) < SIZE;
			
			draw.addRectFilled(pos - Vec2(SIZE, SIZE), pos + Vec2(SIZE, SIZE), is_hovered ? Color{0xff, 0xff, 0xff, 0xff} : Color{0xff, 0xff, 0xff, 0x77});
			draw.addRect(pos - Vec2(SIZE, SIZE), pos + Vec2(SIZE, SIZE), {0xff, 0xff, 0xff, 0x77}, 1);

			return is_hovered && ImGui::IsMouseClicked(0);
		};

		MouseMode ret = MouseMode::NONE;
		if (drawHandle(bottom_right, mouse_canvas_pos))
		{
			m_bottom_right_start_transform.x = scene.getRectRightPoints(e);
			m_bottom_right_start_transform.y = scene.getRectBottomPoints(e);
			ret = MouseMode::RESIZE;
		}
		if (drawHandle(mid, mouse_canvas_pos))
		{
			m_bottom_right_start_transform.x = scene.getRectRightPoints(e);
			m_bottom_right_start_transform.y = scene.getRectBottomPoints(e);
			m_top_left_start_move.y = scene.getRectTopPoints(e);
			m_top_left_start_move.x = scene.getRectLeftPoints(e);
			ret = MouseMode::MOVE;
		}
		return ret;
	}
		

	static Vec2 toLumix(const ImVec2& value)
	{
		return { value.x, value.y };
	}



	struct CopyPositionBufferItem
	{
		const Reflection::PropertyBase* prop = nullptr;
		float value;

		void set(GUIScene* scene, EntityRef e, const char* prop_name)
		{
			prop = Reflection::getProperty(GUI_RECT_TYPE, prop_name);
			OutputMemoryStream blob(&value, sizeof(value));
			prop->getValue({ e, GUI_RECT_TYPE, scene }, -1, blob);
		}
	} m_copy_position_buffer[8];
	
	int m_copy_position_buffer_count = 0;

	void copy(EntityRef e, u8 mask)
	{
		GUIScene* scene = (GUIScene*)m_editor->getUniverse()->getScene(crc32("gui"));
		m_copy_position_buffer_count = 0;

		if (mask & (u8)EdgeMask::TOP)
		{
			m_copy_position_buffer[m_copy_position_buffer_count].set(scene, e, "Top Points");
			m_copy_position_buffer[m_copy_position_buffer_count+1].set(scene, e, "Top Relative");
			m_copy_position_buffer_count += 2;
		}

		if (mask & (u8)EdgeMask::BOTTOM)
		{
			m_copy_position_buffer[m_copy_position_buffer_count].set(scene, e, "Bottom Points");
			m_copy_position_buffer[m_copy_position_buffer_count + 1].set(scene, e, "Bottom Relative");
			m_copy_position_buffer_count += 2;
		}

		if (mask & (u8)EdgeMask::LEFT)
		{
			m_copy_position_buffer[m_copy_position_buffer_count].set(scene, e, "Left Points");
			m_copy_position_buffer[m_copy_position_buffer_count + 1].set(scene, e, "Left Relative");
			m_copy_position_buffer_count += 2;
		}

		if (mask & (u8)EdgeMask::RIGHT)
		{
			m_copy_position_buffer[m_copy_position_buffer_count].set(scene, e, "Right Points");
			m_copy_position_buffer[m_copy_position_buffer_count + 1].set(scene, e, "Right Relative");
			m_copy_position_buffer_count += 2;
		}
	}


	void paste(EntityRef e)
	{
		m_editor->beginCommandGroup(crc32("gui_editor_paste"));
		GUIScene* scene = (GUIScene*)m_editor->getUniverse()->getScene(crc32("gui"));
		for (int i = 0; i < m_copy_position_buffer_count; ++i)
		{
			CopyPositionBufferItem& item = m_copy_position_buffer[i];
			m_editor->setProperty(GUI_RECT_TYPE, -1, *item.prop, &e, 1, &item.value, sizeof(item.value));
		}
		m_editor->endCommandGroup();
	}


	void onWindowGUI() override
	{
		if (!m_is_window_open) return;
		if (ImGui::Begin("GUIEditor", &m_is_window_open))
		{
			ImVec2 mouse_canvas_pos = ImGui::GetMousePos();
			mouse_canvas_pos.x -= ImGui::GetCursorScreenPos().x;
			mouse_canvas_pos.y -= ImGui::GetCursorScreenPos().y;
			
			ImVec2 size = ImGui::GetContentRegionAvail();
			if (!m_pipeline->isReady() || size.x == 0 || size.y == 0) {
				ImGui::End();
				return;
			}
			GUIScene* scene = (GUIScene*)m_editor->getUniverse()->getScene(crc32("gui"));
			scene->render(*m_pipeline, { size.x, size.y });
			
			MouseMode new_mode = drawGizmo(m_pipeline->getDraw2D(), *scene, { size.x, size.y }, mouse_canvas_pos);
			if (m_mouse_mode == MouseMode::NONE) m_mouse_mode = new_mode;
			if (ImGui::IsMouseReleased(0)) m_mouse_mode = MouseMode::NONE;
			
			if (m_editor->getSelectedEntities().size() == 1)
			{
				EntityRef e = m_editor->getSelectedEntities()[0];
				switch (m_mouse_mode)
				{
					case MouseMode::RESIZE:
					{
						float b = m_bottom_right_start_transform.y + ImGui::GetMouseDragDelta(0).y;
						scene->setRectBottomPoints(e, b);
						float r = m_bottom_right_start_transform.x + ImGui::GetMouseDragDelta(0).x;
						scene->setRectRightPoints(e, r);
					}
					break;
					case MouseMode::MOVE:
					{
						float b = m_bottom_right_start_transform.y + ImGui::GetMouseDragDelta(0).y;
						scene->setRectBottomPoints(e, b);
						float r = m_bottom_right_start_transform.x + ImGui::GetMouseDragDelta(0).x;
						scene->setRectRightPoints(e, r);

						float t = m_top_left_start_move.y + ImGui::GetMouseDragDelta(0).y;
						scene->setRectTopPoints(e, t);
						float l = m_top_left_start_move.x + ImGui::GetMouseDragDelta(0).x;
						scene->setRectLeftPoints(e, l);
					}
					break;
				}
			}

			Viewport vp = {};
			vp.w = (int)size.x;
			vp.h = (int)size.y;
			m_pipeline->setViewport(vp);
			
			if (m_pipeline->render(true)) {
				m_texture_handle = m_pipeline->getOutput();

				if(m_texture_handle.isValid()) {
					const ImTextureID img = (ImTextureID)(uintptr)m_texture_handle.value;
					if (ffr::isOriginBottomLeft()) {
						ImGui::Image(img, size, ImVec2(0, 1), ImVec2(1, 0));
					}
					else {
						ImGui::Image(img, size);
					}
				}
			}

			if (ImGui::IsMouseClicked(0) && ImGui::IsItemHovered() && m_mouse_mode == MouseMode::NONE)
			{
				EntityPtr e = scene->getRectAt(toLumix(mouse_canvas_pos), toLumix(size));
				if (e.isValid()) {
					EntityRef r = (EntityRef)e;
					m_editor->selectEntities(&r, 1, false);
				}
			}

			bool has_rect = false;
			if (m_editor->getSelectedEntities().size() == 1)
			{
				has_rect = m_editor->getUniverse()->hasComponent(m_editor->getSelectedEntities()[0], GUI_RECT_TYPE);
			}
			if (has_rect && ImGui::BeginPopupContextItem("context"))
			{
				EntityRef e = m_editor->getSelectedEntities()[0];
				if (ImGui::BeginMenu("Make relative"))
				{
					if (ImGui::MenuItem("All")) makeRelative(e, toLumix(size), (u8)EdgeMask::ALL);
					if (ImGui::MenuItem("Top")) makeRelative(e, toLumix(size), (u8)EdgeMask::TOP);
					if (ImGui::MenuItem("Right")) makeRelative(e, toLumix(size), (u8)EdgeMask::RIGHT);
					if (ImGui::MenuItem("Bottom")) makeRelative(e, toLumix(size), (u8)EdgeMask::BOTTOM);
					if (ImGui::MenuItem("Left")) makeRelative(e, toLumix(size), (u8)EdgeMask::LEFT);
					
					ImGui::EndMenu();
				}
				if (ImGui::BeginMenu("Make absolute"))
				{
					if (ImGui::MenuItem("All")) makeAbsolute(e, toLumix(size), (u8)EdgeMask::ALL);
					if (ImGui::MenuItem("Top")) makeAbsolute(e, toLumix(size), (u8)EdgeMask::TOP);
					if (ImGui::MenuItem("Right")) makeAbsolute(e, toLumix(size), (u8)EdgeMask::RIGHT);
					if (ImGui::MenuItem("Bottom")) makeAbsolute(e, toLumix(size), (u8)EdgeMask::BOTTOM);
					if (ImGui::MenuItem("Left")) makeAbsolute(e, toLumix(size), (u8)EdgeMask::LEFT);
					ImGui::EndMenu();
				}
				if (ImGui::BeginMenu("Expand"))
				{
					if (ImGui::MenuItem("All")) expand(e, (u8)EdgeMask::ALL);
					if (ImGui::MenuItem("Top")) expand(e, (u8)EdgeMask::TOP);
					if (ImGui::MenuItem("Right")) expand(e, (u8)EdgeMask::RIGHT);
					if (ImGui::MenuItem("Bottom")) expand(e, (u8)EdgeMask::BOTTOM);
					if (ImGui::MenuItem("Left")) expand(e, (u8)EdgeMask::LEFT);
					if (ImGui::MenuItem("Horizontal")) expand(e, (u8)EdgeMask::HORIZONTAL);
					if (ImGui::MenuItem("Vertical")) expand(e, (u8)EdgeMask::VERTICAL);
					ImGui::EndMenu();
				}
				if (ImGui::BeginMenu("Align"))
				{
					if (ImGui::MenuItem("Top")) align(e, (u8)EdgeMask::TOP);
					if (ImGui::MenuItem("Right")) align(e, (u8)EdgeMask::RIGHT);
					if (ImGui::MenuItem("Bottom")) align(e, (u8)EdgeMask::BOTTOM);
					if (ImGui::MenuItem("Left")) align(e, (u8)EdgeMask::LEFT);
					if (ImGui::MenuItem("Center horizontal")) align(e, (u8)EdgeMask::CENTER_HORIZONTAL);
					if (ImGui::MenuItem("Center vertical")) align(e, (u8)EdgeMask::CENTER_VERTICAL);
					ImGui::EndMenu();
				}
				if (ImGui::BeginMenu("Copy position"))
				{
					if (ImGui::MenuItem("All")) copy(e, (u8)EdgeMask::ALL);
					if (ImGui::MenuItem("Top")) copy(e, (u8)EdgeMask::TOP);
					if (ImGui::MenuItem("Right")) copy(e, (u8)EdgeMask::RIGHT);
					if (ImGui::MenuItem("Bottom")) copy(e, (u8)EdgeMask::BOTTOM);
					if (ImGui::MenuItem("Left")) copy(e, (u8)EdgeMask::LEFT);
					if (ImGui::MenuItem("Horizontal")) copy(e, (u8)EdgeMask::HORIZONTAL);
					if (ImGui::MenuItem("Vertical")) copy(e, (u8)EdgeMask::VERTICAL);
					ImGui::EndMenu();
				}
				if (ImGui::MenuItem("Paste")) paste(e);

				if (ImGui::BeginMenu("Create child"))
				{
					if (ImGui::MenuItem("Button")) createChild(e, GUI_BUTTON_TYPE);
					if (ImGui::MenuItem("Image")) createChild(e, GUI_IMAGE_TYPE);
					if (ImGui::MenuItem("Rect")) createChild(e, GUI_RECT_TYPE);
					if (ImGui::MenuItem("Text")) createChild(e, GUI_TEXT_TYPE);
					if (ImGui::MenuItem("Render target")) createChild(e, GUI_RENDER_TARGET_TYPE);
					ImGui::EndMenu();
				}

				ImGui::EndPopup();
			}
		}

		ImGui::End();
	}


	void createChild(EntityRef entity, ComponentType child_type)
	{
		m_editor->beginCommandGroup(crc32("create_gui_rect_child"));
		EntityRef child = m_editor->addEntity();
		m_editor->makeParent(entity, child);
		m_editor->selectEntities(&child, 1, false);
		m_editor->addComponent(child_type);
		m_editor->endCommandGroup();
	}


	void setRectProperty(EntityRef e, const char* prop_name, float value)
	{
		const Reflection::PropertyBase* prop = Reflection::getProperty(GUI_RECT_TYPE, crc32(prop_name));
		ASSERT(prop);
		m_editor->setProperty(GUI_RECT_TYPE, -1, *prop, &e, 1, &value, sizeof(value));
	}


	void makeAbsolute(EntityRef entity, const Vec2& canvas_size, u8 mask)
	{
		GUIScene* scene = (GUIScene*)m_editor->getUniverse()->getScene(crc32("gui"));

		EntityRef parent = (EntityRef)scene->getUniverse().getParent(entity);
		GUIScene::Rect parent_rect = scene->getRectOnCanvas(parent, canvas_size);
		GUIScene::Rect child_rect = scene->getRectOnCanvas(entity, canvas_size);

		m_editor->beginCommandGroup(crc32("make_gui_rect_absolute"));

		if (mask & (u8)EdgeMask::TOP)
		{
			setRectProperty(entity, "Top Relative", 0);
			setRectProperty(entity, "Top Points", child_rect.y - parent_rect.y);
		}
		
		if (mask & (u8)EdgeMask::LEFT)
		{
			setRectProperty(entity, "Left Relative", 0);
			setRectProperty(entity, "Left Points", child_rect.x - parent_rect.x);
		}

		if (mask & (u8)EdgeMask::RIGHT)
		{
			setRectProperty(entity, "Right Relative", 0);
			setRectProperty(entity, "Right Points", child_rect.x + child_rect.w - parent_rect.x);
		}
		
		if (mask & (u8)EdgeMask::BOTTOM)
		{
			setRectProperty(entity, "Bottom Relative", 0);
			setRectProperty(entity, "Bottom Points", child_rect.y + child_rect.h - parent_rect.y);
		}

		m_editor->endCommandGroup();
	}


	void align(EntityRef entity, u8 mask)
	{
		GUIScene* scene = (GUIScene*)m_editor->getUniverse()->getScene(crc32("gui"));

		m_editor->beginCommandGroup(crc32("align_gui_rect"));

		float br = scene->getRectBottomRelative(entity);
		float bp = scene->getRectBottomPoints(entity);
		float tr = scene->getRectTopRelative(entity);
		float tp = scene->getRectTopPoints(entity);
		float rr = scene->getRectRightRelative(entity);
		float rp = scene->getRectRightPoints(entity);
		float lr = scene->getRectLeftRelative(entity);
		float lp = scene->getRectLeftPoints(entity);

		if (mask & (u8)EdgeMask::TOP)
		{
			setRectProperty(entity, "Bottom Relative", br - tr);
			setRectProperty(entity, "Bottom Points", bp - tp);
			setRectProperty(entity, "Top Relative", 0);
			setRectProperty(entity, "Top Points", 0);
		}

		if (mask & (u8)EdgeMask::LEFT)
		{
			setRectProperty(entity, "Right Relative", rr - lr);
			setRectProperty(entity, "Right Points", rp - lp);
			setRectProperty(entity, "Left Relative", 0);
			setRectProperty(entity, "Left Points", 0);
		}

		if (mask & (u8)EdgeMask::RIGHT)
		{
			setRectProperty(entity, "Left Relative", lr + 1 - rr);
			setRectProperty(entity, "Left Points", lp - rp);
			setRectProperty(entity, "Right Relative", 1);
			setRectProperty(entity, "Right Points", 0);
		}

		if (mask & (u8)EdgeMask::BOTTOM)
		{
			setRectProperty(entity, "Top Relative", tr + 1 - br);
			setRectProperty(entity, "Top Points", tp - bp);
			setRectProperty(entity, "Bottom Relative", 1);
			setRectProperty(entity, "Bottom Points", 0);
		}

		if (mask & (u8)EdgeMask::CENTER_VERTICAL)
		{
			setRectProperty(entity, "Top Relative", 0.5f - (br - tr) * 0.5f);
			setRectProperty(entity, "Top Points", -(bp - tp) * 0.5f);
			setRectProperty(entity, "Bottom Relative", 0.5f + (br - tr) * 0.5f);
			setRectProperty(entity, "Bottom Points", (bp - tp) * 0.5f);
		}

		if (mask & (u8)EdgeMask::CENTER_HORIZONTAL)
		{
			setRectProperty(entity, "Left Relative", 0.5f - (rr - lr) * 0.5f);
			setRectProperty(entity, "Left Points", -(rp - lp) * 0.5f);
			setRectProperty(entity, "Right Relative", 0.5f + (rr - lr) * 0.5f);
			setRectProperty(entity, "Right Points", (rp - lp) * 0.5f);
		}

		m_editor->endCommandGroup();
	}

	void expand(EntityRef entity, u8 mask)
	{
		GUIScene* scene = (GUIScene*)m_editor->getUniverse()->getScene(crc32("gui"));
		m_editor->beginCommandGroup(crc32("expand_gui_rect"));

		if (mask & (u8)EdgeMask::TOP)
		{
			setRectProperty(entity, "Top Points", 0);
			setRectProperty(entity, "Top Relative", 0);
		}

		if (mask & (u8)EdgeMask::RIGHT)
		{
			setRectProperty(entity, "Right Points", 0);
			setRectProperty(entity, "Right Relative", 1);
		}


		if (mask & (u8)EdgeMask::LEFT)
		{
			setRectProperty(entity, "Left Points", 0);
			setRectProperty(entity, "Left Relative", 0);
		}

		if (mask & (u8)EdgeMask::BOTTOM)
		{
			setRectProperty(entity, "Bottom Points", 0);
			setRectProperty(entity, "Bottom Relative", 1);
		}

		m_editor->endCommandGroup();
	}


	void makeRelative(EntityRef entity, const Vec2& canvas_size, u8 mask)
	{
		GUIScene* scene = (GUIScene*)m_editor->getUniverse()->getScene(crc32("gui"));
		
		EntityPtr parent = scene->getUniverse().getParent(entity);
		GUIScene::Rect parent_rect = scene->getRectOnCanvas(parent, canvas_size);
		GUIScene::Rect child_rect = scene->getRectOnCanvas(entity, canvas_size);

		m_editor->beginCommandGroup(crc32("make_gui_rect_relative"));
		
		if (mask & (u8)EdgeMask::TOP)
		{
			setRectProperty(entity, "Top Points", 0);
			setRectProperty(entity, "Top Relative", (child_rect.y - parent_rect.y) / parent_rect.h);
		}

		if (mask & (u8)EdgeMask::RIGHT)
		{
			setRectProperty(entity, "Right Points", 0);
			setRectProperty(entity, "Right Relative", (child_rect.x + child_rect.w - parent_rect.x) / parent_rect.w);
		}

		
		if (mask & (u8)EdgeMask::LEFT)
		{
			setRectProperty(entity, "Left Points", 0);
			setRectProperty(entity, "Left Relative", (child_rect.x - parent_rect.x) / parent_rect.w);
		}
			
		if (mask & (u8)EdgeMask::BOTTOM)
		{
			setRectProperty(entity, "Bottom Points", 0);
			setRectProperty(entity, "Bottom Relative", (child_rect.y + child_rect.h - parent_rect.y) / parent_rect.h);
		}

		m_editor->endCommandGroup();
	}


	bool hasFocus() override { return false; }
	void update(float) override {}
	const char* getName() const override { return "gui_editor"; }


	StudioApp& m_app;
	Pipeline* m_pipeline = nullptr;
	WorldEditor* m_editor = nullptr;
	bool m_is_window_open = false;
	ffr::TextureHandle m_texture_handle;
	MouseMode m_mouse_mode = MouseMode::NONE;
	Vec2 m_bottom_right_start_transform;
	Vec2 m_top_left_start_move;
};


struct StudioAppPlugin : StudioApp::IPlugin
{
	StudioAppPlugin(StudioApp& app)
		: m_app(app)
	{
	}


	const char* getName() const override { return "gui"; }
	bool dependsOn(IPlugin& plugin) const override { return equalStrings(plugin.getName(), "renderer"); }


	void init() override
	{
		m_app.registerComponent("gui_button", "GUI / Button");
		m_app.registerComponentWithResource("gui_image", "GUI / Image", Sprite::TYPE, *Reflection::getProperty(GUI_IMAGE_TYPE, "Sprite"));
		m_app.registerComponent("gui_input_field", "GUI / Input field");
		m_app.registerComponent("gui_rect", "GUI / Rect");
		m_app.registerComponent("gui_render_target", "GUI / Render target");
		m_app.registerComponent("gui_text", "GUI / Text");

		IAllocator& allocator = m_app.getWorldEditor().getAllocator();
		m_gui_editor = LUMIX_NEW(allocator, GUIEditor)(m_app);
		m_app.addPlugin(*m_gui_editor);

		m_sprite_plugin = LUMIX_NEW(allocator, SpritePlugin)(m_app);
		m_app.getAssetBrowser().addPlugin(*m_sprite_plugin);
	}


	~StudioAppPlugin()
	{
		IAllocator& allocator = m_app.getWorldEditor().getAllocator();
		m_app.removePlugin(*m_gui_editor);
		LUMIX_DELETE(allocator, m_gui_editor);

		m_app.getAssetBrowser().removePlugin(*m_sprite_plugin);
		LUMIX_DELETE(allocator, m_sprite_plugin);
	}


	StudioApp& m_app;
	GUIEditor* m_gui_editor;
	SpritePlugin* m_sprite_plugin;
};



} // anonymous namespace


LUMIX_STUDIO_ENTRY(gui)
{
	IAllocator& allocator = app.getWorldEditor().getAllocator();
	return LUMIX_NEW(allocator, StudioAppPlugin)(app);
}
