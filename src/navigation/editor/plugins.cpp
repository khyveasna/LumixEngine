#include "editor/property_grid.h"
#include "editor/render_interface.h"
#include "editor/studio_app.h"
#include "editor/utils.h"
#include "editor/world_editor.h"
#include "engine/crc32.h"
#include "engine/reflection.h"
#include "engine/universe/universe.h"
#include "navigation/navigation_scene.h"
#include <DetourCrowd.h>


using namespace Lumix;


namespace
{


static const ComponentType NAVMESH_AGENT_TYPE = Reflection::getComponentType("navmesh_agent");
static const ComponentType NAVMESH_ZONE_TYPE = Reflection::getComponentType("navmesh_zone");


struct PropertyGridPlugin : PropertyGrid::IPlugin {
	PropertyGridPlugin(StudioApp& app) : m_app(app) {}

	void onAgentGUI(EntityRef entity) {
		auto* scene = static_cast<NavigationScene*>(m_app.getWorldEditor().getUniverse()->getScene(crc32("navigation")));
		static bool debug_draw_path = false;
		const dtCrowdAgent* agent = scene->getDetourAgent(entity);
		if (agent) {
			ImGui::LabelText("Desired speed", "%f", agent->desiredSpeed);
			ImGui::LabelText("Corners", "%d", agent->ncorners);
			if (agent->ncorners > 0) {
				Vec3 pos = *(Vec3*)agent->npos;
				Vec3 corner = *(Vec3*)agent->targetPos;

				ImGui::LabelText("Target distance", "%f", (pos - corner).length());
			}

			static const char* STATES[] = { "Invalid", "Walking", "Offmesh" };
			if (agent->state < lengthOf(STATES)) ImGui::LabelText("State", "%s", STATES[agent->state]);
			static const char* TARGET_STATES[] = { "None", "Failed", "Valid", "Requesting", "Waiting for queue", "Waiting for path", "Velocity" };
			if (agent->targetState < lengthOf(TARGET_STATES)) ImGui::LabelText("Target state", "%s", TARGET_STATES[agent->targetState]);
		}

		ImGui::Checkbox("Draw path", &debug_draw_path);
		if (debug_draw_path) scene->debugDrawPath(entity);
	}

	void onGUI(PropertyGrid& grid, ComponentUID cmp) override {
		auto* scene = static_cast<NavigationScene*>(m_app.getWorldEditor().getUniverse()->getScene(crc32("navigation")));
		if(cmp.type == NAVMESH_AGENT_TYPE) { 
			onAgentGUI((EntityRef)cmp.entity);
			return;
		}

		if (cmp.type != NAVMESH_ZONE_TYPE) return;
		
		if (ImGui::Button("Generate")) {
			scene->generateNavmesh((EntityRef)cmp.entity);
		}
		ImGui::SameLine();
		if (ImGui::Button("Load")) {
			char path[MAX_PATH_LENGTH];
			if (OS::getOpenFilename(path, lengthOf(path), "Navmesh\0*.nav\0", nullptr)) {
				char rel[MAX_PATH_LENGTH];
				m_app.getWorldEditor().makeRelative(rel, lengthOf(rel), path);
				scene->load((EntityRef)cmp.entity, rel);
			}		
		}

		if(scene->isNavmeshReady((EntityRef)cmp.entity)) {
			ImGui::SameLine();
			if (ImGui::Button("Save")) {
				char path[MAX_PATH_LENGTH];
				if (OS::getSaveFilename(path, lengthOf(path), "Navmesh\0*.nav\0", "nav")) {
					char rel[MAX_PATH_LENGTH];
					m_app.getWorldEditor().makeRelative(rel, lengthOf(rel), path);
					scene->save((EntityRef)cmp.entity, rel);
				}
			}
		}
		ImGui::SameLine();
		if (ImGui::Button("Debug tile")) {
			const DVec3 camera_hit = m_app.getWorldEditor().getCameraRaycastHit();
			scene->generateTileAt((EntityRef)cmp.entity, camera_hit, true);
		}

		static bool debug_draw_navmesh = false;
		ImGui::Checkbox("Draw navmesh", &debug_draw_navmesh);
		if (debug_draw_navmesh) {
			static bool inner_boundaries = true;
			static bool outer_boundaries = true;
			static bool portals = true;
			ImGui::Checkbox("Inner boundaries", &inner_boundaries);
			ImGui::Checkbox("Outer boundaries", &outer_boundaries);
			ImGui::Checkbox("Portals", &portals);
			scene->debugDrawNavmesh((EntityRef)cmp.entity, m_app.getWorldEditor().getCameraRaycastHit(), inner_boundaries, outer_boundaries, portals);
		}

		if (scene->hasDebugDrawData((EntityRef)cmp.entity)) {
			static bool debug_draw_compact_heightfield = false;
			ImGui::Checkbox("Draw compact heightfield", &debug_draw_compact_heightfield);
			if (debug_draw_compact_heightfield) scene->debugDrawCompactHeightfield((EntityRef)cmp.entity);

			static bool debug_draw_heightfield = false;
			ImGui::Checkbox("Draw heightfield", &debug_draw_heightfield);
			if (debug_draw_heightfield) scene->debugDrawHeightfield((EntityRef)cmp.entity);

			static bool debug_draw_contours = false;
			ImGui::Checkbox("Draw contours", &debug_draw_contours);
			if (debug_draw_contours) scene->debugDrawContours((EntityRef)cmp.entity);
		}
		else {
			ImGui::Text("For more info press \"Debug tile\"");
		}
	}

	StudioApp& m_app;
};


struct GizmoPlugin final : public WorldEditor::Plugin {
	GizmoPlugin(WorldEditor& editor) : m_editor(editor) {}

	bool showGizmo(ComponentUID cmp) override {
		if(cmp.type != NAVMESH_ZONE_TYPE) return false;

		auto* scene = static_cast<NavigationScene*>(cmp.scene);
		Universe& universe = scene->getUniverse();
		
		RenderInterface* ri = m_editor.getRenderInterface();
		if (!ri) return false;

		const NavmeshZone& zone = scene->getZone((EntityRef)cmp.entity);
		const Transform tr = universe.getTransform((EntityRef)cmp.entity);

		const Vec3 x = tr.rot.rotate(Vec3(zone.extents.x, 0, 0));
		const Vec3 y = tr.rot.rotate(Vec3(0, zone.extents.y, 0));
		const Vec3 z = tr.rot.rotate(Vec3(0, 0, zone.extents.z));

		ri->addDebugCube(tr.pos, z, y, x, 0xffff0000);

		return true; 
	}

	WorldEditor& m_editor;
};


struct StudioAppPlugin : StudioApp::IPlugin
{
	StudioAppPlugin(StudioApp& app)
		: m_app(app)
	{}
	
	void init() override {
		IAllocator& allocator = m_app.getWorldEditor().getAllocator();

		m_zone_pg_plugin = LUMIX_NEW(allocator, PropertyGridPlugin)(m_app);
		m_app.getPropertyGrid().addPlugin(*m_zone_pg_plugin);

		m_app.registerComponent("navmesh_agent", "Navmesh / Agent");
		m_app.registerComponent("navmesh_zone", "Navmesh / Zone");

		WorldEditor& editor = m_app.getWorldEditor();
		m_gizmo_plugin = LUMIX_NEW(allocator, GizmoPlugin)(editor);
		editor.addPlugin(*m_gizmo_plugin);
	}

	~StudioAppPlugin() {
		IAllocator& allocator = m_app.getWorldEditor().getAllocator();
		
		m_app.getWorldEditor().removePlugin(*m_gizmo_plugin);

		LUMIX_DELETE(allocator, m_gizmo_plugin);
		m_app.getPropertyGrid().removePlugin(*m_zone_pg_plugin);
		LUMIX_DELETE(allocator, m_zone_pg_plugin);
	}

	const char* getName() const override {
		return "navigation";
	}

	StudioApp& m_app;
	PropertyGridPlugin* m_zone_pg_plugin;
	GizmoPlugin* m_gizmo_plugin;
};


} // anonymous


LUMIX_STUDIO_ENTRY(navigation)
{
	IAllocator& allocator = app.getWorldEditor().getAllocator();
	return LUMIX_NEW(allocator, StudioAppPlugin)(app);
}

