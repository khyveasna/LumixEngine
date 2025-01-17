#include "animation/animation.h"
#include "animation/animation_scene.h"
#include "animation/controller.h"
#include "animation/property_animation.h"
#include "animation/editor/animation_editor.h"
#include "editor/asset_browser.h"
#include "editor/asset_compiler.h"
#include "editor/property_grid.h"
#include "editor/studio_app.h"
#include "editor/world_editor.h"
#include "engine/crc32.h"
#include "engine/engine.h"
#include "engine/hash_map.h"
#include "engine/log.h"
#include "engine/os.h"
#include "engine/reflection.h"
#include "engine/serializer.h"
#include "engine/universe/universe.h"
#include "imgui/imgui.h"
#include "renderer/model.h"
#include "renderer/pose.h"
#include "renderer/render_scene.h"


using namespace Lumix;


static const ComponentType ANIMABLE_TYPE = Reflection::getComponentType("animable");
static const ComponentType PROPERTY_ANIMATOR_TYPE = Reflection::getComponentType("property_animator");
static const ComponentType CONTROLLER_TYPE = Reflection::getComponentType("anim_controller");
static const ComponentType RENDERABLE_TYPE = Reflection::getComponentType("model_instance");


namespace
{


struct AnimationAssetBrowserPlugin : AssetBrowser::IPlugin, AssetCompiler::IPlugin
{
	explicit AnimationAssetBrowserPlugin(StudioApp& app)
		: m_app(app)
	{
		app.getAssetCompiler().registerExtension("ani", Animation::TYPE);
	}


	bool compile(const Path& src) override
	{
		const char* dst_dir = m_app.getAssetCompiler().getCompiledDir();
		const u32 hash = crc32(src.c_str());

		const StaticString<MAX_PATH_LENGTH> dst(dst_dir, hash, ".res");

		FileSystem& fs = m_app.getWorldEditor().getEngine().getFileSystem();
		return fs.copyFile(src.c_str(), dst);
	}


	void onGUI(Resource* resource) override
	{
		auto* animation = static_cast<Animation*>(resource);
		ImGui::LabelText("FPS", "%d", animation->getFPS());
		ImGui::LabelText("Length", "%.3fs", animation->getLength());
		ImGui::LabelText("Frames", "%d", animation->getFrameCount());
	}


	void onResourceUnloaded(Resource* resource) override {}


	const char* getName() const override { return "Animation"; }


	ResourceType getResourceType() const override { return Animation::TYPE; }


	bool createTile(const char* in_path, const char* out_path, ResourceType type) override
	{
		FileSystem& fs = m_app.getWorldEditor().getEngine().getFileSystem();
		if (type == Animation::TYPE) return fs.copyFile("models/editor/tile_animation.dds", out_path);
		return false;
	}

	StudioApp& m_app;
};


struct PropertyAnimationAssetBrowserPlugin : AssetBrowser::IPlugin
{
	explicit PropertyAnimationAssetBrowserPlugin(StudioApp& app)
		: m_app(app)
	{
		app.getAssetCompiler().registerExtension("anp", PropertyAnimation::TYPE);
	}

	bool canCreateResource() const override { return true; }
	const char* getFileDialogFilter() const override { return "Property animation\0*.anp\0"; }
	const char* getFileDialogExtensions() const override { return "anp"; }
	const char* getDefaultExtension() const override { return "anp"; }

	bool createResource(const char* path) override
	{
		OS::OutputFile file;
		WorldEditor& editor = m_app.getWorldEditor();
		if (!file.open(path))
		{
			logError("Animation") << "Failed to create " << path;
			return false;
		}

		file << "[]";
		file.close();
		return true;
	}


	void ShowAddCurveMenu(PropertyAnimation* animation)
	{
		WorldEditor& editor = m_app.getWorldEditor();
		auto& selected_entities = editor.getSelectedEntities();
		if (selected_entities.empty()) return;
			
		if (!ImGui::BeginMenu("Add curve")) return;

		Universe* universe = editor.getUniverse();
		;
		for (ComponentUID cmp = universe->getFirstComponent(selected_entities[0]); cmp.isValid(); cmp = universe->getNextComponent(cmp))
		{
			const char* cmp_type_name = m_app.getComponentTypeName(cmp.type);
			if (!ImGui::BeginMenu(cmp_type_name)) continue;

			const Reflection::ComponentBase* component = Reflection::getComponent(cmp.type);
			struct : Reflection::ISimpleComponentVisitor
			{
				void visitProperty(const Reflection::PropertyBase& prop) override {}
				void visit(const Reflection::Property<float>& prop) override
				{
					int idx = animation->curves.find([&](PropertyAnimation::Curve& rhs) {
						return rhs.cmp_type == cmp.type && rhs.property == &prop;
					});
					if (idx < 0 &&ImGui::MenuItem(prop.name))
					{
						PropertyAnimation::Curve& curve = animation->addCurve();
						curve.cmp_type = cmp.type;
						curve.property = &prop;
						curve.frames.push(0);
						curve.frames.push(animation->curves.size() > 1 ? animation->curves[0].frames.back() : 1);
						curve.values.push(0);
						curve.values.push(0);
					}
				}
				PropertyAnimation* animation;
				ComponentUID cmp;
			} visitor;

			visitor.animation = animation;
			visitor.cmp = cmp;
			component->visit(visitor);

			ImGui::EndMenu();
		}

		ImGui::EndMenu();
	}


	void savePropertyAnimation(PropertyAnimation& anim)
	{
		if (OutputMemoryStream* file = m_app.getAssetBrowser().beginSaveResource(anim))
		{
			bool success = true;
			struct : ISaveEntityGUIDMap {
				EntityGUID get(EntityPtr entity) override { ASSERT(false); return INVALID_ENTITY_GUID; }
			} dummy_map;
			TextSerializer serializer(*file, dummy_map);
			if (!anim.save(serializer))
			{
				success = false;
				logError("Editor") << "Could not save file " << anim.getPath().c_str();
			}
			m_app.getAssetBrowser().endSaveResource(anim, *file, success);
		}
	}


	void onGUI(Resource* resource) override
	{
		auto* animation = static_cast<PropertyAnimation*>(resource);
		if (!animation->isReady()) return;

		if (ImGui::Button("Save")) savePropertyAnimation(*animation);
		ImGui::SameLine();
		if (ImGui::Button("Open in external editor")) m_app.getAssetBrowser().openInExternalEditor(animation);
			
		ShowAddCurveMenu(animation);

		if (!animation->curves.empty())
		{
			int frames = animation->curves[0].frames.back();
			if (ImGui::InputInt("Frames", &frames))
			{
				for (auto& curve : animation->curves)
				{
					curve.frames.back() = frames;
				}
			}
		}

		for (int i = 0, n = animation->curves.size(); i < n; ++i)
		{
			PropertyAnimation::Curve& curve = animation->curves[i];
			const char* cmp_name = m_app.getComponentTypeName(curve.cmp_type);
			StaticString<64> tmp(cmp_name, " - ", curve.property->name);
			if (ImGui::Selectable(tmp, m_selected_curve == i)) m_selected_curve = i;
		}

		if (m_selected_curve >= animation->curves.size()) m_selected_curve = -1;
		if (m_selected_curve < 0) return;

		ImGui::PushItemWidth(ImGui::GetContentRegionAvailWidth() - 20);
		static ImVec2 size(-1, 200);
			
		PropertyAnimation::Curve& curve = animation->curves[m_selected_curve];
		ImVec2 points[16];
		ASSERT(curve.frames.size() < lengthOf(points));
		for (int i = 0; i < curve.frames.size(); ++i)
		{
			points[i].x = (float)curve.frames[i];
			points[i].y = curve.values[i];
		}
		int new_count;
		int last_frame = curve.frames.back();
		int flags = (int)ImGui::CurveEditorFlags::NO_TANGENTS | (int)ImGui::CurveEditorFlags::SHOW_GRID;
		if (m_fit_curve_in_editor)
		{
			flags |= (int)ImGui::CurveEditorFlags::RESET;
			m_fit_curve_in_editor = false;
		}
		int changed = ImGui::CurveEditor("curve", (float*)points, curve.frames.size(), size, flags, &new_count, &m_selected_point);
		if (changed >= 0)
		{
			curve.frames[changed] = int(points[changed].x + 0.5f);
			curve.values[changed] = points[changed].y;
			curve.frames.back() = last_frame;
			curve.frames[0] = 0;
		}
		if (new_count != curve.frames.size())
		{
			curve.frames.resize(new_count);
			curve.values.resize(new_count);
			for (int i = 0; i < new_count; ++i)
			{
				curve.frames[i] = int(points[i].x + 0.5f);
				curve.values[i] = points[i].y;
			}
		}

		ImGui::PopItemWidth();

		if (ImGui::BeginPopupContextItem("curve"))
		{
			if (ImGui::Selectable("Fit data")) m_fit_curve_in_editor = true;

			ImGui::EndPopup();
		}

		if (m_selected_point >= 0 && m_selected_point < curve.frames.size())
		{
			ImGui::InputInt("Frame", &curve.frames[m_selected_point]);
			ImGui::InputFloat("Value", &curve.values[m_selected_point]);
		}

		ImGui::HSplitter("sizer", &size);
	}


	void onResourceUnloaded(Resource* resource) override {}
	const char* getName() const override { return "Property animation"; }
	ResourceType getResourceType() const override { return PropertyAnimation::TYPE; }


	int m_selected_point = -1;
	int m_selected_curve = -1;
	bool m_fit_curve_in_editor = false;
	StudioApp& m_app;
};


struct AnimControllerAssetBrowserPlugin : AssetBrowser::IPlugin, AssetCompiler::IPlugin
{
	explicit AnimControllerAssetBrowserPlugin(StudioApp& app)
		: m_app(app)
	{
		app.getAssetCompiler().registerExtension("act", Anim::ControllerResource::TYPE);
	}

	bool compile(const Path& src)  {
		const char* dst_dir = m_app.getAssetCompiler().getCompiledDir();
		const u32 hash = crc32(src.c_str());

		const StaticString<MAX_PATH_LENGTH> dst(dst_dir, hash, ".res");

		FileSystem& fs = m_app.getWorldEditor().getEngine().getFileSystem();
		return fs.copyFile(src.c_str(), dst);
	}

	void onGUI(Resource* resource) override {}


	void onResourceUnloaded(Resource* resource) override {}
	const char* getName() const override { return "Animation Controller"; }
	ResourceType getResourceType() const override { return Anim::ControllerResource::TYPE; }


	bool createTile(const char* in_path, const char* out_path, ResourceType type) override
	{
		FileSystem& fs = m_app.getWorldEditor().getEngine().getFileSystem();
		if (type == Anim::ControllerResource::TYPE) return fs.copyFile("models/editor/tile_animation_graph.dds", out_path);
		return false;
	}


	StudioApp& m_app;
};


struct AnimablePropertyGridPlugin : PropertyGrid::IPlugin
{
	explicit AnimablePropertyGridPlugin(StudioApp& app)
		: m_app(app)
	{
		m_is_playing = false;
	}


	void onGUI(PropertyGrid& grid, ComponentUID cmp) override
	{
		if (cmp.type != ANIMABLE_TYPE) return;

		const EntityRef entity = (EntityRef)cmp.entity;
		auto* scene = static_cast<AnimationScene*>(cmp.scene);
		auto* animation = scene->getAnimableAnimation(entity);
		if (!animation) return;
		if (!animation->isReady()) return;

		ImGui::Checkbox("Preview", &m_is_playing);
		float time = scene->getAnimable(entity).time;
		if (ImGui::SliderFloat("Time", &time, 0, animation->getLength()))
		{
			scene->getAnimable(entity).time = time;
			scene->updateAnimable(entity, 0);
		}

		if (m_is_playing)
		{
			float time_delta = m_app.getWorldEditor().getEngine().getLastTimeDelta();
			scene->updateAnimable(entity, time_delta);
		}

		if (ImGui::CollapsingHeader("Transformation"))
		{
			auto* render_scene = (RenderScene*)scene->getUniverse().getScene(RENDERABLE_TYPE);
			if (scene->getUniverse().hasComponent(entity, RENDERABLE_TYPE))
			{
				const Pose* pose = render_scene->lockPose(entity);
				Model* model = render_scene->getModelInstanceModel(entity);
				if (pose && model)
				{
					ImGui::Columns(3);
					for (int i = 0; i < pose->count; ++i)
					{
						ImGui::Text("%s", model->getBone(i).name.c_str());
						ImGui::NextColumn();
						ImGui::Text("%f; %f; %f", pose->positions[i].x, pose->positions[i].y, pose->positions[i].z);
						ImGui::NextColumn();
						ImGui::Text("%f; %f; %f; %f", pose->rotations[i].x, pose->rotations[i].y, pose->rotations[i].z, pose->rotations[i].w);
						ImGui::NextColumn();
					}
					ImGui::Columns();
				}
				if (pose) render_scene->unlockPose(entity, false);
			}
		}
	}


	StudioApp& m_app;
	bool m_is_playing;
};


struct StudioAppPlugin : StudioApp::IPlugin
{
	explicit StudioAppPlugin(StudioApp& app)
		: m_app(app)
	{
	}


	const char* getName() const override { return "animation"; }


	void init() override
	{
		m_app.registerComponentWithResource("property_animator", "Animation / Property animator", PropertyAnimation::TYPE, *Reflection::getProperty(PROPERTY_ANIMATOR_TYPE, "Animation"));
		m_app.registerComponentWithResource("animable", "Animation / Animable", Animation::TYPE, *Reflection::getProperty(ANIMABLE_TYPE, "Animation"));
		m_app.registerComponentWithResource("anim_controller", "Animation / Controller", Anim::ControllerResource::TYPE, *Reflection::getProperty(CONTROLLER_TYPE, "Source"));
		m_app.registerComponent("shared_anim_controller", "Animation / Shared controller");

		IAllocator& allocator = m_app.getWorldEditor().getAllocator();
		m_animtion_plugin = LUMIX_NEW(allocator, AnimationAssetBrowserPlugin)(m_app);
		m_prop_anim_plugin = LUMIX_NEW(allocator, PropertyAnimationAssetBrowserPlugin)(m_app);
		m_anim_ctrl_plugin = LUMIX_NEW(allocator, AnimControllerAssetBrowserPlugin)(m_app);
		
		const char* exts[] = { "ani", nullptr };
		m_app.getAssetCompiler().addPlugin(*m_animtion_plugin, exts);
		const char* act_exts[] = { "act", nullptr };
		m_app.getAssetCompiler().addPlugin(*m_anim_ctrl_plugin, act_exts);

		AssetBrowser& asset_browser = m_app.getAssetBrowser();
		asset_browser.addPlugin(*m_animtion_plugin);
		asset_browser.addPlugin(*m_prop_anim_plugin);
		asset_browser.addPlugin(*m_anim_ctrl_plugin);

		m_animable_plugin = LUMIX_NEW(allocator, AnimablePropertyGridPlugin)(m_app);
		m_app.getPropertyGrid().addPlugin(*m_animable_plugin);
		
		m_anim_editor = AnimEditor::IAnimationEditor::create(allocator, m_app);
		m_app.addPlugin(*m_anim_editor);
	}


	~StudioAppPlugin()
	{
		m_app.getAssetCompiler().removePlugin(*m_animtion_plugin);
		m_app.getAssetCompiler().removePlugin(*m_anim_ctrl_plugin);

		AssetBrowser& asset_browser = m_app.getAssetBrowser();
		asset_browser.removePlugin(*m_animtion_plugin);
		asset_browser.removePlugin(*m_prop_anim_plugin);
		asset_browser.removePlugin(*m_anim_ctrl_plugin);

		IAllocator& allocator = m_app.getWorldEditor().getAllocator();
		LUMIX_DELETE(allocator, m_animtion_plugin);
		LUMIX_DELETE(allocator, m_prop_anim_plugin);
		LUMIX_DELETE(allocator, m_anim_ctrl_plugin);

		m_app.getPropertyGrid().removePlugin(*m_animable_plugin);
		LUMIX_DELETE(allocator, m_animable_plugin);

		m_app.removePlugin(*m_anim_editor);
		LUMIX_DELETE(allocator, m_anim_editor);
	}


	StudioApp& m_app;
	AnimablePropertyGridPlugin* m_animable_plugin;
	AnimationAssetBrowserPlugin* m_animtion_plugin;
	PropertyAnimationAssetBrowserPlugin* m_prop_anim_plugin;
	AnimControllerAssetBrowserPlugin* m_anim_ctrl_plugin;
	AnimEditor::IAnimationEditor* m_anim_editor;
};


} // anonymous namespace


LUMIX_STUDIO_ENTRY(animation)
{
	IAllocator& allocator = app.getWorldEditor().getAllocator();
	return LUMIX_NEW(allocator, StudioAppPlugin)(app);
}

