#include "utils.h"
#include "engine/math.h"
#include "engine/path.h"
#include "engine/path_utils.h"
#include "engine/reflection.h"
#include "editor/render_interface.h"
#include "editor/world_editor.h"
#include "imgui/imgui.h"
#include "engine/universe/universe.h"


namespace Lumix
{


Action::Action(const char* label_short, const char* label_long, const char* name)
	: label_long(label_long)
	, label_short(label_short)
	, name(name)
	, plugin(nullptr)
	, is_global(true)
	, icon(nullptr)
{
	this->label_short = label_short;
	this->label_long = label_long;
	this->name = name;
	shortcut[0] = shortcut[1] = shortcut[2] = OS::Keycode::INVALID;
	is_selected.bind<falseConst>();
}


Action::Action(const char* label_short,
	const char* label_long,
	const char* name,
	OS::Keycode shortcut0,
	OS::Keycode shortcut1,
	OS::Keycode shortcut2)
	: label_long(label_long)
	, label_short(label_short)
	, name(name)
	, plugin(nullptr)
	, is_global(true)
	, icon(nullptr)
{
	shortcut[0] = shortcut0;
	shortcut[1] = shortcut1;
	shortcut[2] = shortcut2;
	is_selected.bind<falseConst>();
}


bool Action::toolbarButton()
{
	if (!icon) return false;

	ImVec4 col_active = ImGui::GetStyle().Colors[ImGuiCol_ButtonActive];
	ImVec4 bg_color = is_selected.invoke() ? col_active : ImVec4(0, 0, 0, 0);
	int* t = (int*)icon; 
	if (ImGui::ToolbarButton((void*)(uintptr_t)*t, bg_color, label_long))
	{
		func.invoke();
		return true;
	}
	return false;
}


void Action::getIconPath(char* path, int max_size)
{
	copyString(path, max_size, "editor/icons/icon_"); 
		
	char tmp[1024];
	const char* c = name;
	char* out = tmp;
	while (*c)
	{
		if (*c >= 'A' && *c <= 'Z') *out = *c - ('A' - 'a');
		else if (*c >= 'a' && *c <= 'z') *out = *c;
		else *out = '_';
		++out;
		++c;
	}
	*out = 0;

	catString(path, max_size, tmp);
	catString(path, max_size, ".dds");
}


bool Action::isRequested()
{
	if (ImGui::IsAnyItemActive()) return false;

	bool* keys_down = ImGui::GetIO().KeysDown;
	float* keys_down_duration = ImGui::GetIO().KeysDownDuration;
	if (shortcut[0] == OS::Keycode::INVALID) return false;

	for (int i = 0; i < lengthOf(shortcut) + 1; ++i)
	{
		if (i == lengthOf(shortcut) || shortcut[i] == OS::Keycode::INVALID)
		{
			return true;
		}

		if (!keys_down[(int)shortcut[i]] || keys_down_duration[(int)shortcut[i]] > 0) return false;
	}
	return false;
}



bool Action::isActive()
{
	if (ImGui::IsAnyItemFocused()) return false;
	if (shortcut[0] == OS::Keycode::INVALID) return false;

	for (int i = 0; i < lengthOf(shortcut) + 1; ++i) {
		if (i == lengthOf(shortcut) || shortcut[i] == OS::Keycode::INVALID) {
			return true;
		}
		if (!OS::isKeyDown(shortcut[i])) return false;
	}
	return false;
}


void getEntityListDisplayName(WorldEditor& editor, char* buf, int max_size, EntityPtr entity)
{
	if (!entity.isValid())
	{
		*buf = '\0';
		return;
	}

	EntityRef e = (EntityRef)entity;
	const char* name = editor.getUniverse()->getEntityName(e);
	static const auto MODEL_INSTANCE_TYPE = Reflection::getComponentType("model_instance");
	if (editor.getUniverse()->hasComponent(e, MODEL_INSTANCE_TYPE))
	{
		auto* render_interface = editor.getRenderInterface();
		auto path = render_interface->getModelInstancePath(e);
		if (path.isValid())
		{
			const char* c = path.c_str();
			while (*c && *c != ':') ++c;
			if (*c == ':') {
				copyString(buf, minimum(max_size, int(c - path.c_str() + 1)), path.c_str());
				return;
			}

			char basename[MAX_PATH_LENGTH];
			copyString(buf, max_size, path.c_str());
			PathUtils::getBasename(basename, MAX_PATH_LENGTH, path.c_str());
			if (name && name[0] != '\0')
				copyString(buf, max_size, name);
			else
				toCString(entity.index, buf, max_size);

			catString(buf, max_size, " - ");
			catString(buf, max_size, basename);
			return;
		}
	}

	if (name && name[0] != '\0')
	{
		copyString(buf, max_size, name);
	}
	else
	{
		toCString(entity.index, buf, max_size);
	}
}


} // namespace Lumix