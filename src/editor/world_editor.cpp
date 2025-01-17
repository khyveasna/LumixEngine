#include "world_editor.h"

#include "editor/editor_icon.h"
#include "editor/gizmo.h"
#include "editor/measure_tool.h"
#include "editor/prefab_system.h"
#include "engine/array.h"
#include "engine/associative_array.h"
#include "engine/command_line_parser.h"
#include "engine/crc32.h"
#include "engine/delegate_list.h"
#include "engine/engine.h"
#include "engine/file_system.h"
#include "engine/geometry.h"
#include "engine/iplugin.h"
#include "engine/log.h"
#include "engine/math.h"
#include "engine/os.h"
#include "engine/path.h"
#include "engine/path_utils.h"
#include "engine/plugin_manager.h"
#include "engine/profiler.h"
#include "engine/reflection.h"
#include "engine/resource.h"
#include "engine/resource_manager.h"
#include "engine/serializer.h"
#include "engine/stream.h"
#include "engine/universe/universe.h"
#include "ieditor_command.h"
#include "render_interface.h"


namespace Lumix
{


static const ComponentType MODEL_INSTANCE_TYPE = Reflection::getComponentType("model_instance");
static const ComponentType CAMERA_TYPE = Reflection::getComponentType("camera");


static void load(ComponentUID cmp, int index, InputMemoryStream& blob)
{
	int count = blob.read<int>();
	for (int i = 0; i < count; ++i)
	{
		u32 hash = blob.read<u32>();
		int size = blob.read<int>();
		const Reflection::PropertyBase* prop = Reflection::getProperty(cmp.type, hash);
		if (!prop)
		{
			blob.skip(size);
			continue;
		}

		prop->setValue(cmp, index, blob);
	}
}


struct BeginGroupCommand final : public IEditorCommand
{
	BeginGroupCommand() = default;
	explicit BeginGroupCommand(WorldEditor&) {}

	bool execute() override { return true; }
	void undo() override { ASSERT(false); }
	bool merge(IEditorCommand& command) override { ASSERT(false); return false; }
	const char* getType() override { return "begin_group"; }
};


struct EndGroupCommand final : public IEditorCommand
{
	EndGroupCommand() = default;
	EndGroupCommand(WorldEditor&) {}

	bool execute() override { return true; }
	void undo() override { ASSERT(false); }
	bool merge(IEditorCommand& command) override { ASSERT(false); return false; }
	const char* getType() override { return "end_group"; }

	u32 group_type;
};


class SetEntityNameCommand final : public IEditorCommand
{
public:
	SetEntityNameCommand(WorldEditor& editor, EntityRef entity, const char* name)
		: m_entity(entity)
		, m_new_name(name, editor.getAllocator())
		, m_old_name(editor.getUniverse()->getEntityName(entity),
					 editor.getAllocator())
		, m_editor(editor)
	{
	}


	bool execute() override
	{
		m_editor.getUniverse()->setEntityName((EntityRef)m_entity, m_new_name.c_str());
		return true;
	}


	void undo() override
	{
		m_editor.getUniverse()->setEntityName((EntityRef)m_entity, m_old_name.c_str());
	}


	const char* getType() override { return "set_entity_name"; }


	bool merge(IEditorCommand& command) override
	{
		ASSERT(command.getType() == getType());
		if (static_cast<SetEntityNameCommand&>(command).m_entity == m_entity)
		{
			static_cast<SetEntityNameCommand&>(command).m_new_name = m_new_name;
			return true;
		}
		else
		{
			return false;
		}
	}

private:
	WorldEditor& m_editor;
	EntityRef m_entity;
	String m_new_name;
	String m_old_name;
};


class MoveEntityCommand final : public IEditorCommand
{
public:
	explicit MoveEntityCommand(WorldEditor& editor)
		: m_new_positions(editor.getAllocator())
		, m_new_rotations(editor.getAllocator())
		, m_old_positions(editor.getAllocator())
		, m_old_rotations(editor.getAllocator())
		, m_entities(editor.getAllocator())
		, m_editor(editor)
	{
	}


	MoveEntityCommand(WorldEditor& editor,
		const EntityRef* entities,
		const DVec3* new_positions,
		const Quat* new_rotations,
		int count,
		IAllocator& allocator)
		: m_new_positions(allocator)
		, m_new_rotations(allocator)
		, m_old_positions(allocator)
		, m_old_rotations(allocator)
		, m_entities(allocator)
		, m_editor(editor)
	{
		ASSERT(count > 0);
		Universe* universe = m_editor.getUniverse();
		PrefabSystem& prefab_system = m_editor.getPrefabSystem();
		m_entities.reserve(count);
		m_new_positions.reserve(count);
		m_new_rotations.reserve(count);
		m_old_positions.reserve(count);
		m_old_rotations.reserve(count);
		for (int i = count - 1; i >= 0; --i)
		{
			u64 prefab = prefab_system.getPrefab(entities[i]);
			EntityPtr parent = universe->getParent(entities[i]);
			if (prefab != 0 && parent.isValid() && (prefab_system.getPrefab((EntityRef)parent) & 0xffffFFFF) == (prefab & 0xffffFFFF))
			{
				float scale = universe->getScale(entities[i]);
				Transform new_local_tr = universe->computeLocalTransform((EntityRef)parent, { new_positions[i], new_rotations[i], scale });
				EntityPtr instance = prefab_system.getFirstInstance(prefab);
				while (instance.isValid())
				{
					EntityRef instance_ref = (EntityRef)instance;
					m_entities.push(instance_ref);
					const EntityPtr inst_parent_ptr = universe->getParent(instance_ref);
					ASSERT(inst_parent_ptr.isValid());
					Transform new_tr = universe->getTransform((EntityRef)inst_parent_ptr);
					new_tr = new_tr * new_local_tr;
					m_new_positions.push(new_tr.pos);
					m_new_rotations.push(new_tr.rot);
					m_old_positions.push(universe->getPosition(instance_ref));
					m_old_rotations.push(universe->getRotation(instance_ref));
					instance = prefab_system.getNextInstance(instance_ref);
				}
			}
			else
			{
				m_entities.push(entities[i]);
				m_new_positions.push(new_positions[i]);
				m_new_rotations.push(new_rotations[i]);
				m_old_positions.push(universe->getPosition(entities[i]));
				m_old_rotations.push(universe->getRotation(entities[i]));
			}
		}
	}


	bool execute() override
	{
		Universe* universe = m_editor.getUniverse();
		for (int i = 0, c = m_entities.size(); i < c; ++i) {
			if(m_entities[i].isValid()) {
				const EntityRef entity = (EntityRef)m_entities[i];
				universe->setPosition(entity, m_new_positions[i]);
				universe->setRotation(entity, m_new_rotations[i]);
			}
		}
		return true;
	}


	void undo() override
	{
		Universe* universe = m_editor.getUniverse();
		for (int i = 0, c = m_entities.size(); i < c; ++i) {
			if(m_entities[i].isValid()) {
				EntityRef entity = (EntityRef)m_entities[i];
				universe->setPosition(entity, m_old_positions[i]);
				universe->setRotation(entity, m_old_rotations[i]);
			}
		}
	}


	const char* getType() override { return "move_entity"; }


	bool merge(IEditorCommand& command) override
	{
		ASSERT(command.getType() == getType());
		MoveEntityCommand& my_command = static_cast<MoveEntityCommand&>(command);
		if (my_command.m_entities.size() == m_entities.size())
		{
			for (int i = 0, c = m_entities.size(); i < c; ++i)
			{
				if (m_entities[i] != my_command.m_entities[i])
				{
					return false;
				}
			}
			for (int i = 0, c = m_entities.size(); i < c; ++i)
			{
				my_command.m_new_positions[i] = m_new_positions[i];
				my_command.m_new_rotations[i] = m_new_rotations[i];
			}
			return true;
		}
		else
		{
			return false;
		}
	}

private:
	WorldEditor& m_editor;
	Array<EntityPtr> m_entities;
	Array<DVec3> m_new_positions;
	Array<Quat> m_new_rotations;
	Array<DVec3> m_old_positions;
	Array<Quat> m_old_rotations;
};


class LocalMoveEntityCommand final : public IEditorCommand
{
public:
	explicit LocalMoveEntityCommand(WorldEditor& editor)
		: m_new_positions(editor.getAllocator())
		, m_old_positions(editor.getAllocator())
		, m_entities(editor.getAllocator())
		, m_editor(editor)
	{
	}


	LocalMoveEntityCommand(WorldEditor& editor,
		const EntityRef* entities,
		const DVec3* new_positions,
		int count,
		IAllocator& allocator)
		: m_new_positions(allocator)
		, m_old_positions(allocator)
		, m_entities(allocator)
		, m_editor(editor)
	{
		ASSERT(count > 0);
		Universe* universe = m_editor.getUniverse();
		PrefabSystem& prefab_system = m_editor.getPrefabSystem();
		m_entities.reserve(count);
		m_new_positions.reserve(count);
		m_old_positions.reserve(count);
		for (int i = count - 1; i >= 0; --i)
		{
			u64 prefab = prefab_system.getPrefab(entities[i]);
			EntityPtr parent = universe->getParent(entities[i]);
			if (prefab != 0 && parent.isValid() && (prefab_system.getPrefab((EntityRef)parent) & 0xffffFFFF) == (prefab & 0xffffFFFF))
			{
				EntityPtr instance = prefab_system.getFirstInstance(prefab);
				while (instance.isValid())
				{
					EntityRef e = (EntityRef)instance;
					m_entities.push(e);
					m_new_positions.push(new_positions[i]);
					m_old_positions.push(universe->getPosition(e));
					instance = prefab_system.getNextInstance(e);
				}
			}
			else
			{
				m_entities.push(entities[i]);
				m_new_positions.push(new_positions[i]);
				m_old_positions.push(universe->getPosition(entities[i]));
			}
		}
	}


	bool execute() override
	{
		Universe* universe = m_editor.getUniverse();
		for (int i = 0, c = m_entities.size(); i < c; ++i) {
			if (m_entities[i].isValid()) {
				EntityRef entity = (EntityRef)m_entities[i];
				universe->setLocalPosition(entity, m_new_positions[i]);
			}
		}
		return true;
	}


	void undo() override
	{
		Universe* universe = m_editor.getUniverse();
		for (int i = 0, c = m_entities.size(); i < c; ++i) {
			if (m_entities[i].isValid()) {
				EntityRef entity = (EntityRef)m_entities[i];
				universe->setLocalPosition(entity, m_old_positions[i]);
			}
		}
	}


	const char* getType() override { return "local_move_entity"; }


	bool merge(IEditorCommand& command) override
	{
		ASSERT(command.getType() == getType());
		LocalMoveEntityCommand& my_command = static_cast<LocalMoveEntityCommand&>(command);
		if (my_command.m_entities.size() == m_entities.size())
		{
			for (int i = 0, c = m_entities.size(); i < c; ++i)
			{
				if (m_entities[i] != my_command.m_entities[i])
				{
					return false;
				}
			}
			for (int i = 0, c = m_entities.size(); i < c; ++i)
			{
				my_command.m_new_positions[i] = m_new_positions[i];
			}
			return true;
		}
		else
		{
			return false;
		}
	}

private:
	WorldEditor& m_editor;
	Array<EntityPtr> m_entities;
	Array<DVec3> m_new_positions;
	Array<DVec3> m_old_positions;
};


class ScaleEntityCommand final : public IEditorCommand
{
public:
	explicit ScaleEntityCommand(WorldEditor& editor)
		: m_old_scales(editor.getAllocator())
		, m_new_scales(editor.getAllocator())
		, m_entities(editor.getAllocator())
		, m_editor(editor)
	{
	}


	ScaleEntityCommand(WorldEditor& editor,
		const EntityRef* entities,
		int count,
		float scale,
		IAllocator& allocator)
		: m_old_scales(allocator)
		, m_new_scales(allocator)
		, m_entities(allocator)
		, m_editor(editor)
	{
		Universe* universe = m_editor.getUniverse();
		for (int i = count - 1; i >= 0; --i)
		{
			m_entities.push(entities[i]);
			m_old_scales.push(universe->getScale(entities[i]));
			m_new_scales.push(scale);
		}
	}


	ScaleEntityCommand(WorldEditor& editor,
		const EntityRef* entities,
		const float* scales,
		int count,
		IAllocator& allocator)
		: m_old_scales(allocator)
		, m_new_scales(allocator)
		, m_entities(allocator)
		, m_editor(editor)
	{
		Universe* universe = m_editor.getUniverse();
		for (int i = count - 1; i >= 0; --i)
		{
			m_entities.push(entities[i]);
			m_old_scales.push(universe->getScale(entities[i]));
			m_new_scales.push(scales[i]);
		}
	}


	bool execute() override
	{
		Universe* universe = m_editor.getUniverse();
		for (int i = 0, c = m_entities.size(); i < c; ++i) {
			if (m_entities[i].isValid()) {
				EntityRef entity = (EntityRef)m_entities[i];
				universe->setScale(entity, m_new_scales[i]);
			}
		}
		return true;
	}


	void undo() override
	{
		Universe* universe = m_editor.getUniverse();
		for (int i = 0, c = m_entities.size(); i < c; ++i) {
			if (m_entities[i].isValid()) {
				EntityRef entity = (EntityRef)m_entities[i];
				universe->setScale(entity, m_old_scales[i]);
			}
		}
	}


	const char* getType() override { return "scale_entity"; }


	bool merge(IEditorCommand& command) override
	{
		ASSERT(command.getType() == getType());
		auto& my_command = static_cast<ScaleEntityCommand&>(command);
		if (my_command.m_entities.size() == m_entities.size())
		{
			for (int i = 0, c = m_entities.size(); i < c; ++i)
			{
				if (m_entities[i] != my_command.m_entities[i])
				{
					return false;
				}
				if (m_new_scales[i] != my_command.m_new_scales[i])
				{
					return false;
				}
			}
			return true;
		}
		else
		{
			return false;
		}
	}

private:
	WorldEditor& m_editor;
	Array<EntityPtr> m_entities;
	Array<float> m_new_scales;
	Array<float> m_old_scales;
};


struct GatherResourcesVisitor : Reflection::ISimpleComponentVisitor
{
	void visitProperty(const Reflection::PropertyBase& prop) override {}

	void visit(const Reflection::IArrayProperty& prop) override
	{
		int count = prop.getCount(cmp);
		for (int i = 0; i < count; ++i)
		{
			index = i;
			prop.visit(*this);
		}
		index = -1;
	}

	void visit(const Reflection::Property<Path>& prop) override
	{
		visitProperty(prop);
		auto* attr = Reflection::getAttribute(prop, Reflection::IAttribute::RESOURCE);
		if (!attr) return;
		auto* resource_attr = (Reflection::ResourceAttribute*)attr;

		OutputMemoryStream tmp(editor->getAllocator());
		prop.getValue(cmp, index, tmp);
		Path path((const char*)tmp.getData());
		Resource* resource = resource_manager->load(resource_attr->type, path);
		if(resource) resources->push(resource);
	}

	ResourceManagerHub* resource_manager;
	ComponentUID cmp;
	int index = -1;
	WorldEditor* editor;
	Array<Resource*>* resources;
};


struct SaveVisitor : Reflection::ISimpleComponentVisitor
{
	void begin(const Reflection::ComponentBase& cmp) override
	{
		stream->write(cmp.getPropertyCount());
	}

	void visitProperty(const Reflection::PropertyBase& prop) override
	{
		stream->write(crc32(prop.name));
		int size = (int)stream->getPos();
		stream->write(size);
		prop.getValue(cmp, index, *stream);
		*(int*)((u8*)stream->getData() + size) = (int)stream->getPos() - size - sizeof(int);
	}

	ComponentUID cmp;
	OutputMemoryStream* stream;
	int index = -1;
};


class RemoveArrayPropertyItemCommand final : public IEditorCommand
{

public:
	explicit RemoveArrayPropertyItemCommand(WorldEditor& editor)
		: m_old_values(editor.getAllocator())
		, m_editor(editor)
	{
	}

	RemoveArrayPropertyItemCommand(WorldEditor& editor,
		const ComponentUID& component,
		int index,
		const Reflection::IArrayProperty& property)
		: m_component(component)
		, m_index(index)
		, m_property(&property)
		, m_old_values(editor.getAllocator())
		, m_editor(editor)
	{
		SaveVisitor save;
		save.cmp = m_component;
		save.stream = &m_old_values;
		save.index = m_index;
		m_property->visit(save);
	}


	bool execute() override
	{
		m_property->removeItem(m_component, m_index);
		return true;
	}


	void undo() override
	{
		m_property->addItem(m_component, m_index);
		InputMemoryStream old_values(m_old_values);
		load(m_component, m_index, old_values);
	}


	const char* getType() override { return "remove_array_property_item"; }


	bool merge(IEditorCommand&) override { return false; }

private:
	WorldEditor& m_editor;
	ComponentUID m_component;
	int m_index;
	const Reflection::IArrayProperty *m_property;
	OutputMemoryStream m_old_values;
};


class AddArrayPropertyItemCommand final : public IEditorCommand
{

public:
	explicit AddArrayPropertyItemCommand(WorldEditor& editor)
		: m_editor(editor)
	{
	}

	AddArrayPropertyItemCommand(WorldEditor& editor,
		const ComponentUID& component,
		const Reflection::IArrayProperty& property)
		: m_component(component)
		, m_index(-1)
		, m_property(&property)
		, m_editor(editor)
	{
	}


	bool execute() override
	{
		m_property->addItem(m_component, -1);
		m_index = m_property->getCount(m_component) - 1;
		return true;
	}


	void undo() override
	{
		m_property->removeItem(m_component, m_index);
	}


	const char* getType() override { return "add_array_property_item"; }


	bool merge(IEditorCommand&) override { return false; }

private:
	ComponentUID m_component;
	int m_index;
	const Reflection::IArrayProperty *m_property;
	WorldEditor& m_editor;
};


class SetPropertyCommand final : public IEditorCommand
{
public:
	explicit SetPropertyCommand(WorldEditor& editor)
		: m_editor(editor)
		, m_entities(editor.getAllocator())
		, m_new_value(editor.getAllocator())
		, m_old_value(editor.getAllocator())
	{
	}


	SetPropertyCommand(WorldEditor& editor,
		const EntityRef* entities,
		int count,
		ComponentType component_type,
		int index,
		const Reflection::PropertyBase& property,
		const void* data,
		int size)
		: m_component_type(component_type)
		, m_entities(editor.getAllocator())
		, m_property(&property)
		, m_editor(editor)
		, m_new_value(editor.getAllocator())
		, m_old_value(editor.getAllocator())
	{
		auto& prefab_system = editor.getPrefabSystem();
		m_entities.reserve(count);

		for (int i = 0; i < count; ++i)
		{
			if (!m_editor.getUniverse()->getComponent(entities[i], m_component_type).isValid()) continue;
			u64 prefab = prefab_system.getPrefab(entities[i]);
			if (prefab == 0)
			{
				ComponentUID component = m_editor.getUniverse()->getComponent(entities[i], component_type);
				m_property->getValue(component, index, m_old_value);
				m_entities.push(entities[i]);
			}
			else
			{
				EntityPtr instance = prefab_system.getFirstInstance(prefab);
				while(instance.isValid())
				{
					EntityRef inst_ref = (EntityRef)instance;
					ComponentUID component = m_editor.getUniverse()->getComponent(inst_ref, component_type);
					m_property->getValue(component, index, m_old_value);
					m_entities.push(inst_ref);
					instance = prefab_system.getNextInstance(inst_ref);
				}
			}
		}

		m_index = index;
		m_new_value.write(data, size);
	}


	bool execute() override
	{
		InputMemoryStream blob(m_new_value);
		for (EntityPtr entity : m_entities) {
			if(entity.isValid()) {
				ComponentUID component = m_editor.getUniverse()->getComponent((EntityRef)entity, m_component_type);
				blob.rewind();
				m_property->setValue(component, m_index, blob);
			}
		}
		return true;
	}


	void undo() override
	{
		InputMemoryStream blob(m_old_value);
		for (EntityPtr entity : m_entities) {
			if (entity.isValid()) {
				ComponentUID component = m_editor.getUniverse()->getComponent((EntityRef)entity, m_component_type);
				m_property->setValue(component, m_index, blob);
			}
		}
	}


	const char* getType() override { return "set_property_values"; }


	bool merge(IEditorCommand& command) override
	{
		ASSERT(command.getType() == getType());
		SetPropertyCommand& src = static_cast<SetPropertyCommand&>(command);
		if (m_component_type == src.m_component_type &&
			m_entities.size() == src.m_entities.size() &&
			src.m_property == m_property &&
			m_index == src.m_index)
		{
			for (int i = 0, c = m_entities.size(); i < c; ++i)
			{
				if (m_entities[i] != src.m_entities[i]) return false;
			}

			src.m_new_value = m_new_value;
			return true;
		}
		return false;
	}

private:
	WorldEditor& m_editor;
	ComponentType m_component_type;
	Array<EntityPtr> m_entities;
	OutputMemoryStream m_new_value;
	OutputMemoryStream m_old_value;
	int m_index;
	const Reflection::PropertyBase* m_property;
};

class PasteEntityCommand;


struct WorldEditorImpl final : public WorldEditor
{
	friend class PasteEntityCommand;
private:
	struct AddComponentCommand final : public IEditorCommand
	{
		AddComponentCommand(WorldEditorImpl& editor,
							const Array<EntityRef>& entities,
							ComponentType type)
			: m_editor(editor)
			, m_entities(editor.getAllocator())
		{
			m_type = type;
			m_entities.reserve(entities.size());
			Universe* universe = m_editor.getUniverse();
			for (EntityRef e : entities) {
				if (!universe->getComponent(e, type).isValid()) {
					u64 prefab = editor.getPrefabSystem().getPrefab(e);
					if (prefab == 0) {
						m_entities.push(e);
					}
					else {
						EntityPtr instance = editor.getPrefabSystem().getFirstInstance(prefab);
						while (instance.isValid()) {
							const EntityRef e = (EntityRef)instance;
							m_entities.push(e);
							instance = editor.getPrefabSystem().getNextInstance(e);
						}
					}
				}
			}
		}


		bool merge(IEditorCommand&) override { return false; }


		const char* getType() override { return "add_component"; }


		bool execute() override
		{
			bool ret = false;
			Universe* universe = m_editor.getUniverse();

			for (EntityRef e : m_entities) {
				ASSERT(!universe->hasComponent(e, m_type));
				universe->createComponent(m_type, e);
				if (universe->hasComponent(e, m_type)) {
					ret = true;
				}
				else {
					logError("Editor") << "Failed to create component on entity " << e.index;
				}
			}
			return ret;
		}


		void undo() override
		{
			Universe* universe = m_editor.getUniverse();
			for (EntityRef e : m_entities) {
				if (universe->hasComponent(e, m_type)) {
					universe->destroyComponent(e, m_type);
					ASSERT(!universe->hasComponent(e, m_type));
				}
			}
		}


	private:
		ComponentType m_type;
		Array<EntityRef> m_entities;
		WorldEditorImpl& m_editor;
	};


	class MakeParentCommand final : public IEditorCommand
	{
	public:
		explicit MakeParentCommand(WorldEditor& editor)
			: m_editor(static_cast<WorldEditorImpl&>(editor))
		{
		}


		MakeParentCommand(WorldEditorImpl& editor, EntityPtr parent, EntityRef child)
			: m_editor(static_cast<WorldEditorImpl&>(editor))
			, m_parent(parent)
			, m_child(child)
		{
		}


		bool merge(IEditorCommand& cmd) override { 
			
			auto& c = (MakeParentCommand&)cmd;
			if (c.m_child != m_child) return false;
			c.m_parent = m_parent;
			return true;
		}


		const char* getType() override { return "make_parent"; }


		bool execute() override
		{
			if(m_child.isValid()) {
				const EntityRef e = (EntityRef)m_child;
				m_old_parent = m_editor.getUniverse()->getParent(e);
				m_editor.getUniverse()->setParent(m_parent, e);
			}
			return true;
		}


		void undo() override
		{
			if(m_child.isValid()) {
				m_editor.getUniverse()->setParent(m_old_parent, (EntityRef)m_child);
			}
		}

	private:
		WorldEditor& m_editor;
		EntityPtr m_parent;
		EntityPtr m_old_parent;
		EntityPtr m_child;
	};


	class DestroyEntitiesCommand final : public IEditorCommand
	{
	public:
		explicit DestroyEntitiesCommand(WorldEditor& editor)
			: m_editor(static_cast<WorldEditorImpl&>(editor))
			, m_entities(editor.getAllocator())
			, m_transformations(editor.getAllocator())
			, m_old_values(editor.getAllocator())
			, m_resources(editor.getAllocator())
		{
		}


		DestroyEntitiesCommand(WorldEditorImpl& editor, const EntityRef* entities, int count)
			: m_editor(editor)
			, m_entities(editor.getAllocator())
			, m_transformations(editor.getAllocator())
			, m_old_values(editor.getAllocator())
			, m_resources(editor.getAllocator())
		{
			m_entities.reserve(count);
			for (int i = 0; i < count; ++i)
			{
				m_entities.push(entities[i]);
				pushChildren(entities[i]);
			}
			m_entities.removeDuplicates();
			m_transformations.reserve(m_entities.size());
		}


		~DestroyEntitiesCommand()
		{
			for (Resource* resource : m_resources)
			{
				resource->getResourceManager().unload(*resource);
			}
		}


		void pushChildren(EntityRef entity)
		{
			Universe* universe = m_editor.getUniverse();
			for (EntityPtr e = universe->getFirstChild(entity); e.isValid(); e = universe->getNextSibling((EntityRef)e))
			{
				m_entities.push((EntityRef)e);
				pushChildren((EntityRef)e);
			}
		}


		bool execute() override
		{
			Universe* universe = m_editor.getUniverse();
			m_transformations.clear();
			m_old_values.clear();
			ResourceManagerHub& resource_manager = m_editor.getEngine().getResourceManager();
			for (int i = 0; i < m_entities.size(); ++i)
			{
				m_transformations.emplace(universe->getTransform(m_entities[i]));
				int count = 0;
				for (ComponentUID cmp = universe->getFirstComponent(m_entities[i]);
					cmp.isValid();
					cmp = universe->getNextComponent(cmp))
				{
					++count;
				}
				EntityGUID guid = m_editor.m_entity_map.get(m_entities[i]);
				m_old_values.write(guid.value);
				m_old_values.writeString(universe->getEntityName(m_entities[i]));
				EntityPtr parent = universe->getParent(m_entities[i]);
				m_old_values.write(parent);
				if (parent.isValid())
				{
					Transform local_tr = universe->getLocalTransform(m_entities[i]);
					m_old_values.write(local_tr);
				}
				for (EntityPtr child = universe->getFirstChild(m_entities[i]); child.isValid(); child = universe->getNextSibling((EntityRef)child))
				{
					m_old_values.write(child);
					Transform local_tr = universe->getLocalTransform((EntityRef)child);
					m_old_values.write(local_tr);
				}
				m_old_values.write(INVALID_ENTITY);

				m_old_values.write(count);
				for (ComponentUID cmp = universe->getFirstComponent(m_entities[i]);
					cmp.isValid();
					cmp = universe->getNextComponent(cmp))
				{
					m_old_values.write(cmp.type);
					const Reflection::ComponentBase* cmp_desc = Reflection::getComponent(cmp.type);

					GatherResourcesVisitor gather;
					gather.cmp = cmp;
					gather.editor = &m_editor;
					gather.resources = &m_resources;
					gather.resource_manager = &resource_manager;
					cmp_desc->visit(gather);

					SaveVisitor save;
					save.cmp = cmp;
					save.stream = &m_old_values;
					cmp_desc->visit(save);
				}
				u64 prefab = m_editor.getPrefabSystem().getPrefab(m_entities[i]);
				m_old_values.write(prefab);
			}
			for (EntityRef e : m_entities)
			{
				universe->destroyEntity(e);
				m_editor.m_entity_map.erase(e);
			}
			return true;
		}


		bool merge(IEditorCommand&) override { return false; }


		void undo() override
		{
			Universe* universe = m_editor.getUniverse();
			InputMemoryStream blob(m_old_values);
			for (int i = 0; i < m_entities.size(); ++i)
			{
				universe->emplaceEntity(m_entities[i]);
			}
			for (int i = 0; i < m_entities.size(); ++i)
			{
				EntityRef new_entity = m_entities[i];
				universe->setTransform(new_entity, m_transformations[i]);
				int cmps_count;
				EntityGUID guid;
				blob.read(guid.value);
				m_editor.m_entity_map.insert(guid, new_entity);
				char name[Universe::ENTITY_NAME_MAX_LENGTH];
				blob.readString(name, lengthOf(name));
				universe->setEntityName(new_entity, name);
				EntityPtr parent;
				blob.read(parent);
				if (parent.isValid())
				{
					Transform local_tr;
					blob.read(local_tr);
					universe->setParent(parent, new_entity);
					universe->setLocalTransform(new_entity, local_tr);
				}
				EntityPtr child;
				for(blob.read(child); child.isValid(); blob.read(child))
				{
					Transform local_tr;
					blob.read(local_tr);
					universe->setParent(new_entity, (EntityRef)child);
					universe->setLocalTransform((EntityRef)child, local_tr);
				}

				blob.read(cmps_count);
				for (int j = 0; j < cmps_count; ++j)
				{
					ComponentType cmp_type;
					blob.read(cmp_type);
					ComponentUID new_component;
					IScene* scene = universe->getScene(cmp_type);
					ASSERT(scene);
					universe->createComponent(cmp_type, new_entity);
					new_component.entity = new_entity;
					new_component.scene = scene;
					new_component.type = cmp_type;
					
					::Lumix::load(new_component, -1, blob);
				}
				u64 tpl;
				blob.read(tpl);
				if (tpl) m_editor.getPrefabSystem().setPrefab(new_entity, tpl);
			}
		}


		const char* getType() override { return "destroy_entities"; }


	private:
		WorldEditorImpl& m_editor;
		Array<EntityRef> m_entities;
		Array<Transform> m_transformations;
		OutputMemoryStream m_old_values;
		Array<Resource*> m_resources;
	};


	class DestroyComponentCommand final : public IEditorCommand
	{
	public:
		explicit DestroyComponentCommand(WorldEditor& editor)
			: m_editor(static_cast<WorldEditorImpl&>(editor))
			, m_old_values(editor.getAllocator())
			, m_entities(editor.getAllocator())
			, m_cmp_type(INVALID_COMPONENT_TYPE)
			, m_resources(editor.getAllocator())
		{
		}


		DestroyComponentCommand(WorldEditorImpl& editor, const EntityRef* entities, int count, ComponentType cmp_type)
			: m_cmp_type(cmp_type)
			, m_editor(editor)
			, m_old_values(editor.getAllocator())
			, m_entities(editor.getAllocator())
			, m_resources(editor.getAllocator())
		{
			m_entities.reserve(count);
			PrefabSystem& prefab_system = editor.getPrefabSystem();
			for (int i = 0; i < count; ++i) {
				if (!m_editor.getUniverse()->getComponent(entities[i], m_cmp_type).isValid()) continue;
				const u64 prefab = prefab_system.getPrefab(entities[i]);
				if (prefab == 0) {
					m_entities.push(entities[i]);
				}
				else {
					EntityPtr instance = prefab_system.getFirstInstance(prefab);
					while(instance.isValid()) {
						m_entities.push((EntityRef)instance);
						instance = prefab_system.getNextInstance((EntityRef)instance);
					}
				}
			}
		}


		~DestroyComponentCommand()
		{
			for (Resource* resource : m_resources) {
				resource->getResourceManager().unload(*resource);
			}
		}


		void undo() override
		{
			ComponentUID cmp;
			Universe* universe = m_editor.getUniverse();
			cmp.scene = universe->getScene(m_cmp_type);
			cmp.type = m_cmp_type;
			ASSERT(cmp.scene);
			InputMemoryStream blob(m_old_values);
			for (EntityRef entity : m_entities)
			{
				cmp.entity = entity;
				universe->createComponent(cmp.type, entity);
				::Lumix::load(cmp, -1, blob);
			}
		}


		bool merge(IEditorCommand&) override { return false; }


		const char* getType() override { return "destroy_components"; }


		bool execute() override
		{
			ASSERT(!m_entities.empty());
			const Reflection::ComponentBase* cmp_desc = Reflection::getComponent(m_cmp_type);
			ComponentUID cmp;
			cmp.type = m_cmp_type;
			cmp.scene = m_editor.getUniverse()->getScene(m_cmp_type);
			ASSERT(cmp.scene);

			ResourceManagerHub& resource_manager = m_editor.getEngine().getResourceManager();

			for (EntityRef entity : m_entities) {
				cmp.entity = entity;
				SaveVisitor save;
				save.cmp = cmp;
				save.stream = &m_old_values;
				cmp_desc->visit(save);

				GatherResourcesVisitor gather;
				gather.cmp = cmp;
				gather.editor = &m_editor;
				gather.resources = &m_resources;
				gather.resource_manager = &resource_manager;
				cmp_desc->visit(gather);

				m_editor.getUniverse()->destroyComponent(entity, m_cmp_type);
			}
			return true;
		}

	private:
		Array<EntityRef> m_entities;
		ComponentType m_cmp_type;
		WorldEditorImpl& m_editor;
		OutputMemoryStream m_old_values;
		Array<Resource*> m_resources;
	};


	struct AddEntityCommand final : public IEditorCommand
	{
		AddEntityCommand(WorldEditorImpl& editor, const DVec3& position)
			: m_editor(editor)
			, m_position(position)
		{
			m_entity = INVALID_ENTITY;
		}


		bool execute() override
		{
			if (m_entity.isValid()) {
				m_editor.getUniverse()->emplaceEntity((EntityRef)m_entity);
				m_editor.getUniverse()->setPosition((EntityRef)m_entity, m_position);
			}
			else {
				m_entity = m_editor.getUniverse()->createEntity(m_position, Quat(0, 0, 0, 1));
			}
			const EntityRef e = (EntityRef)m_entity;
			((WorldEditorImpl&)m_editor).m_entity_map.create(e);
			m_editor.selectEntities(&e, 1, false);
			return true;
		}


		void undo() override
		{
			ASSERT(m_entity.isValid());

			const EntityRef e = (EntityRef)m_entity;
			m_editor.getUniverse()->destroyEntity(e);
			m_editor.m_entity_map.erase(e);
		}


		bool merge(IEditorCommand&) override { return false; }
		const char* getType() override { return "add_entity"; }
		EntityPtr getEntity() const { return m_entity; }


	private:
		WorldEditorImpl& m_editor;
		EntityPtr m_entity;
		DVec3 m_position;
	};

public:
	IAllocator& getAllocator() override { return m_allocator; }


	const Viewport& getViewport() const override { return m_viewport; }
	void setViewport(const Viewport& viewport) override { m_viewport = viewport; }


	Universe* getUniverse() override
	{
		return m_universe; 
	}


	Engine& getEngine() override { return *m_engine; }


	void showGizmos()
	{
		if (m_selected_entities.empty()) return;

		Universe* universe = getUniverse();

		if (m_selected_entities.size() > 1)
		{
			AABB aabb = m_render_interface->getEntityAABB(*universe, m_selected_entities[0], m_viewport.pos);
			for (int i = 1; i < m_selected_entities.size(); ++i)
			{
				AABB entity_aabb = m_render_interface->getEntityAABB(*universe, m_selected_entities[i], m_viewport.pos);
				aabb.merge(entity_aabb);
			}

			m_render_interface->addDebugCube(m_viewport.pos + aabb.min, m_viewport.pos + aabb.max, 0xffffff00);
			return;
		}

		for (ComponentUID cmp = universe->getFirstComponent(m_selected_entities[0]);
			cmp.isValid();
			cmp = universe->getNextComponent(cmp))
		{
			for (auto* plugin : m_plugins)
			{
				if (plugin->showGizmo(cmp)) break;
			}
		}
	}


	void createEditorLines()
	{
		PROFILE_FUNCTION();
		showGizmos();
		m_measure_tool->createEditorLines(*m_render_interface);
	}


	void updateGoTo()
	{
		if (!m_go_to_parameters.m_is_active) return;

		float t = easeInOut(m_go_to_parameters.m_t);
		m_go_to_parameters.m_t += m_engine->getLastTimeDelta() * m_go_to_parameters.m_speed;
		DVec3 pos = m_go_to_parameters.m_from * (1 - t) + m_go_to_parameters.m_to * t;
		Quat rot;
		nlerp(m_go_to_parameters.m_from_rot, m_go_to_parameters.m_to_rot, &rot, t);
		if (m_go_to_parameters.m_t >= 1)
		{
			pos = m_go_to_parameters.m_to;
			m_go_to_parameters.m_is_active = false;
		}
		m_viewport.pos = pos;
		m_viewport.rot = rot;
	}


	void inputFrame() override
	{
		m_mouse_rel_x = m_mouse_rel_y = 0;
		for (auto& i : m_is_mouse_click) i = false;
	}


	void previewSnapVertex()
	{
		if (m_snap_mode != SnapMode::VERTEX) return;

		DVec3 origin;
		Vec3 dir;
		m_viewport.getRay(m_mouse_pos, origin, dir);
		const RayHit hit = m_render_interface->castRay(origin, dir, INVALID_ENTITY);
		//if (m_gizmo->isActive()) return;
		if (!hit.is_hit) return;

		const DVec3 snap_pos = getClosestVertex(hit);
		m_render_interface->addDebugCross(snap_pos, 1, 0xfff00fff);
		// TODO
	}


	void update() override
	{
		PROFILE_FUNCTION();

		// TODO do not allow user interaction (e.g. saving universe) while queue is not empty
		while (!m_command_queue.empty()) {
			if (!m_command_queue[0]->isReady()) break;

			IEditorCommand* cmd = m_command_queue[0];
			m_command_queue.erase(0);
			doExecute(cmd);
		}

		updateGoTo();
		previewSnapVertex();

		if (!m_selected_entities.empty())
		{
			m_gizmo->add(m_selected_entities[0]);
		}

		if (m_is_mouse_down[(int)OS::MouseButton::LEFT] && m_mouse_mode == MouseMode::SELECT)
		{
			m_render_interface->addRect2D(m_rect_selection_start, m_mouse_pos, 0xfffffFFF);
			m_render_interface->addRect2D(m_rect_selection_start - Vec2(1, 1), m_mouse_pos + Vec2(1, 1), 0xff000000);
		}

		createEditorLines();
		m_gizmo->update(m_viewport);
	}


	void updateEngine() override
	{
		ASSERT(m_universe);
		m_engine->update(*m_universe);
	}


	~WorldEditorImpl()
	{
		destroyUniverse();

		Gizmo::destroy(*m_gizmo);
		m_gizmo = nullptr;

		removePlugin(*m_measure_tool);
		LUMIX_DELETE(m_allocator, m_measure_tool);
		ASSERT(m_plugins.empty());

		EditorIcons::destroy(*m_editor_icons);
		PrefabSystem::destroy(m_prefab_system);

		LUMIX_DELETE(m_allocator, m_render_interface);
	}


	bool isMouseClick(OS::MouseButton button) const override
	{
		return m_is_mouse_click[(int)button];
	}


	bool isMouseDown(OS::MouseButton button) const override
	{
		return m_is_mouse_down[(int)button];
	}


	void snapEntities(const DVec3& hit_pos)
	{
		Array<DVec3> positions(m_allocator);
		Array<Quat> rotations(m_allocator);
		if(m_gizmo->isTranslateMode())
		{
			for(auto e : m_selected_entities)
			{
				positions.push(hit_pos);
				rotations.push(m_universe->getRotation(e));
			}
		}
		else
		{
			for(auto e : m_selected_entities)
			{
				const DVec3 pos = m_universe->getPosition(e);
				Vec3 dir = (pos - hit_pos).toFloat();
				dir.normalize();
				Matrix mtx = Matrix::IDENTITY;
				Vec3 y(0, 1, 0);
				if(dotProduct(y, dir) > 0.99f)
				{
					y.set(1, 0, 0);
				}
				Vec3 x = crossProduct(y, dir);
				x.normalize();
				y = crossProduct(dir, x);
				y.normalize();
				mtx.setXVector(x);
				mtx.setYVector(y);
				mtx.setZVector(dir);

				positions.push(pos);
				rotations.emplace(mtx.getRotation());
			}
		}
		MoveEntityCommand* cmd = LUMIX_NEW(m_allocator, MoveEntityCommand)(*this,
			&m_selected_entities[0],
			&positions[0],
			&rotations[0],
			positions.size(),
			m_allocator);
		executeCommand(cmd);
	}


	DVec3 getClosestVertex(const RayHit& hit)
	{
		ASSERT(hit.is_hit);
		return m_render_interface->getClosestVertex(m_universe, (EntityRef)hit.entity, hit.pos);
	}


	void onMouseDown(int x, int y, OS::MouseButton button) override
	{
		m_is_mouse_click[(int)button] = true;
		m_is_mouse_down[(int)button] = true;
		if(button == OS::MouseButton::MIDDLE)
		{
			m_mouse_mode = MouseMode::PAN;
		}
		else if (button == OS::MouseButton::RIGHT)
		{
			m_mouse_mode = MouseMode::NAVIGATE;
		}
		else if (button == OS::MouseButton::LEFT)
		{
			DVec3 origin;
			Vec3 dir;
			m_viewport.getRay({(float)x, (float)y}, origin, dir);
			const RayHit hit = m_render_interface->castRay(origin, dir, INVALID_ENTITY);
			if (m_gizmo->isActive()) return;

			for (int i = 0; i < m_plugins.size(); ++i)
			{
				if (m_plugins[i]->onMouseDown(hit, x, y))
				{
					m_mouse_handling_plugin = m_plugins[i];
					m_mouse_mode = MouseMode::CUSTOM;
					return;
				}
			}
			m_mouse_mode = MouseMode::SELECT;
			m_rect_selection_start = {(float)x, (float)y};
		}
	}


	void addPlugin(Plugin& plugin) override { m_plugins.push(&plugin); }


	void removePlugin(Plugin& plugin) override
	{
		m_plugins.eraseItemFast(&plugin);
	}



	void onMouseMove(int x, int y, int relx, int rely) override
	{
		PROFILE_FUNCTION();
		m_mouse_pos.set((float)x, (float)y);
		m_mouse_rel_x = (float)relx;
		m_mouse_rel_y = (float)rely;

		static const float MOUSE_MULTIPLIER = 1 / 200.0f;

		switch (m_mouse_mode)
		{
			case MouseMode::CUSTOM:
			{
				if (m_mouse_handling_plugin)
				{
					m_mouse_handling_plugin->onMouseMove(x, y, relx, rely);
				}
			}
			break;
			case MouseMode::NAVIGATE: rotateCamera(relx, rely); break;
			case MouseMode::PAN: panCamera(relx * MOUSE_MULTIPLIER, rely * MOUSE_MULTIPLIER); break;
			case MouseMode::NONE:
			case MouseMode::SELECT:
				break;
		}
	}


	void rectSelect()
	{
		Array<EntityRef> entities(m_allocator);

		Vec2 min = m_rect_selection_start;
		Vec2 max = m_mouse_pos;
		if (min.x > max.x) swap(min.x, max.x);
		if (min.y > max.y) swap(min.y, max.y);
		const ShiftedFrustum frustum = m_viewport.getFrustum(min, max);
		m_render_interface->getRenderables(entities, frustum);
		selectEntities(entities.empty() ? nullptr : &entities[0], entities.size(), false);
	}


	void onMouseUp(int x, int y, OS::MouseButton button) override
	{
		m_mouse_pos = {(float)x, (float)y};
		if (m_mouse_mode == MouseMode::SELECT)
		{
			if (m_rect_selection_start.x != m_mouse_pos.x || m_rect_selection_start.y != m_mouse_pos.y)
			{
				rectSelect();
			}
			else
			{
				DVec3 origin;
				Vec3 dir;
				m_viewport.getRay(m_mouse_pos, origin, dir);
				auto hit = m_render_interface->castRay(origin, dir, INVALID_ENTITY);

				if (m_snap_mode != SnapMode::NONE && !m_selected_entities.empty() && hit.is_hit)
				{
					DVec3 snap_pos = origin + dir * hit.t;
					if (m_snap_mode == SnapMode::VERTEX) snap_pos = getClosestVertex(hit);
					const Quat rot = m_universe->getRotation(m_selected_entities[0]);
					const Vec3 offset = rot.rotate(m_gizmo->getOffset());
					snapEntities(snap_pos - offset);
				}
				else
				{
					auto icon_hit = m_editor_icons->raycast(origin, dir);
					if (icon_hit.entity != INVALID_ENTITY)
					{
						if(icon_hit.entity.isValid()) {
							EntityRef e = (EntityRef)icon_hit.entity;
							selectEntities(&e, 1, true);
						}
					}
					else if (hit.is_hit)
					{
						if(hit.entity.isValid()) {
							EntityRef entity = (EntityRef)hit.entity;
							selectEntities(&entity, 1, true);
						}
					}
				}
			}
		}

		m_is_mouse_down[(int)button] = false;
		if (m_mouse_handling_plugin)
		{
			m_mouse_handling_plugin->onMouseUp(x, y, button);
			m_mouse_handling_plugin = nullptr;
		}
		m_mouse_mode = MouseMode::NONE;
	}


	Vec2 getMousePos() const override { return m_mouse_pos; }
	float getMouseRelX() const override { return m_mouse_rel_x; }
	float getMouseRelY() const override { return m_mouse_rel_y; }


	bool isUniverseChanged() const override { return m_is_universe_changed; }


	void saveUniverse(const char* basename, bool save_path) override
	{
		logInfo("Editor") << "Saving universe " << basename << "...";
		
		StaticString<MAX_PATH_LENGTH> dir(m_engine->getFileSystem().getBasePath(), "universes/");
		OS::makePath(dir);
		StaticString<MAX_PATH_LENGTH> path(dir, basename, ".unv");
		OS::OutputFile file;
		if (file.open(path)) {
			save(file);
			file.close();
		}
		else {
			logError("Editor") << "Failed to save universe " << basename;
		}

		serialize(basename);
		m_is_universe_changed = false;

		if (save_path) m_universe->setName(basename);
	}


	// TODO split
	struct EntityGUIDMap : public ILoadEntityGUIDMap, public ISaveEntityGUIDMap
	{
		explicit EntityGUIDMap(IAllocator& allocator)
			: guid_to_entity(allocator)
			, entity_to_guid(allocator)
			, is_random(true)
		{
		}


		void clear()
		{
			nonrandom_guid = 0;
			entity_to_guid.clear();
			guid_to_entity.clear();
		}


		void create(EntityRef entity)
		{
			EntityGUID guid = { is_random ? randGUID() : ++nonrandom_guid };
			insert(guid, entity);
		}


		void erase(EntityRef entity)
		{
			EntityGUID guid = entity_to_guid[entity.index];
			if (!isValid(guid)) return;
			entity_to_guid[entity.index] = INVALID_ENTITY_GUID;
			guid_to_entity.erase(guid.value);
		}


		void insert(EntityGUID guid, EntityRef entity)
		{
			guid_to_entity.insert(guid.value, entity);
			while (entity.index >= entity_to_guid.size())
			{
				entity_to_guid.push(INVALID_ENTITY_GUID);
			}
			entity_to_guid[entity.index] = guid;
		}


		EntityPtr get(EntityGUID guid) override
		{
			auto iter = guid_to_entity.find(guid.value);
			if (iter.isValid()) return iter.value();
			return INVALID_ENTITY;
		}


		EntityGUID get(EntityPtr entity) override
		{
			if (!entity.isValid()) return INVALID_ENTITY_GUID;
			if (entity.index >= entity_to_guid.size()) return INVALID_ENTITY_GUID;
			return entity_to_guid[entity.index];
		}


		bool has(EntityGUID guid) const
		{
			auto iter = guid_to_entity.find(guid.value);
			return iter.isValid();
		}


		HashMap<u64, EntityRef> guid_to_entity;
		Array<EntityGUID> entity_to_guid;
		u64 nonrandom_guid = 0;
		bool is_random = true;
	};


	bool deserialize(Universe& universe
		, const char* basedir
		, const char* basename
		, PrefabSystem& prefab_system
		, EntityGUIDMap& entity_map
		, IAllocator& allocator) const
	{
		PROFILE_FUNCTION();
		
		entity_map.clear();
		StaticString<MAX_PATH_LENGTH> scn_dir(basedir, "/", basename, "/scenes/");
		OS::FileIterator* scn_file_iter = m_engine->getFileSystem().createFileIterator(scn_dir);
		Array<u8> data(allocator);
		FileSystem& fs = m_engine->getFileSystem();
		OS::InputFile file;
		auto loadFile = [&file, &data, &entity_map, &fs](const char* filepath, auto callback) {
			if (fs.open(filepath, &file))
			{
				if (file.size() > 0)
				{
					data.resize((int)file.size());
					file.read(&data[0], data.size());
					InputMemoryStream blob(&data[0], data.size());
					TextDeserializer deserializer(blob, entity_map);
					callback(deserializer);
				}
				file.close();
			}
		};
		OS::FileInfo info;
		int versions[ComponentType::MAX_TYPES_COUNT];
		while (OS::getNextFile(scn_file_iter, &info))
		{
			if (info.is_directory) continue;
			if (info.filename[0] == '.') continue;

			StaticString<MAX_PATH_LENGTH> filepath(scn_dir, info.filename);
			char plugin_name[64];
			PathUtils::getBasename(plugin_name, lengthOf(plugin_name), filepath);
			IScene* scene = universe.getScene(crc32(plugin_name));
			if (!scene)
			{
				logError("Editor") << "Could not open " << filepath << " since there is not plugin " << plugin_name;
				return false;
			}

			loadFile(filepath, [scene, &versions, &universe](TextDeserializer& deserializer) {
				int version;
				deserializer.read(&version);
				for (int i = 0; i < ComponentType::MAX_TYPES_COUNT; ++i)
				{
					ComponentType cmp_type = {i};
					if (universe.getScene(cmp_type) == scene)
					{
						versions[i] = version;
					}
				}
				scene->deserialize(deserializer);
			});
		}
		OS::destroyFileIterator(scn_file_iter);
		
		StaticString<MAX_PATH_LENGTH> dir(basedir, "/", basename, "/");
		auto file_iter = m_engine->getFileSystem().createFileIterator(dir);
		while (OS::getNextFile(file_iter, &info))
		{
			if (info.is_directory) continue;
			if (info.filename[0] == '.') continue;

			StaticString<MAX_PATH_LENGTH> filepath(dir, info.filename);
			char tmp[32];
			PathUtils::getBasename(tmp, lengthOf(tmp), filepath);
			EntityGUID guid;
			fromCString(tmp, lengthOf(tmp), &guid.value);
			EntityRef entity = universe.createEntity({0, 0, 0}, {0, 0, 0, 1});
			entity_map.insert(guid, entity);
		}
		OS::destroyFileIterator(file_iter);
		
		file_iter = m_engine->getFileSystem().createFileIterator(dir);
		while (OS::getNextFile(file_iter, &info))
		{
			if (info.is_directory) continue;
			if (info.filename[0] == '.') continue;

			StaticString<MAX_PATH_LENGTH> filepath(dir, info.filename);
			char tmp[32];
			PathUtils::getBasename(tmp, lengthOf(tmp), filepath);
			EntityGUID guid;
			fromCString(tmp, lengthOf(tmp), &guid.value);
			loadFile(filepath, [&versions, &entity_map, &universe, guid](TextDeserializer& deserializer) {
				char name[64];
				deserializer.read(name, lengthOf(name));
				RigidTransform tr;
				deserializer.read(&tr);
				float scale;
				deserializer.read(&scale);

				const EntityPtr e = entity_map.get(guid);
				const EntityRef entity = (EntityRef)e;

				EntityPtr parent;
				deserializer.read(&parent);
				if (parent.isValid()) universe.setParent(parent, entity);

				if(name[0]) universe.setEntityName(entity, name);
				universe.setTransformKeepChildren(entity, {tr.pos, tr.rot, scale});
				u32 cmp_type_hash;
				deserializer.read(&cmp_type_hash);
				while (cmp_type_hash != 0)
				{
					ComponentType cmp_type = Reflection::getComponentTypeFromHash(cmp_type_hash);
					universe.deserializeComponent(deserializer, entity, cmp_type, versions[cmp_type.index]);
					deserializer.read(&cmp_type_hash);
				}
			});
		}
		OS::destroyFileIterator(file_iter);

		StaticString<MAX_PATH_LENGTH> filepath(basedir, "/", basename, "/systems/templates.sys");
		loadFile(filepath, [&](TextDeserializer& deserializer) {
			prefab_system.deserialize(deserializer);
			for (int i = 0, c = prefab_system.getMaxEntityIndex(); i < c; ++i)
			{
				u64 prefab = prefab_system.getPrefab({i});
				if (prefab != 0) entity_map.create({i});
			}
		});
		return &universe;
	}

	
	void serialize(const char* basename)
	{
		StaticString<MAX_PATH_LENGTH> dir(m_engine->getFileSystem().getBasePath(), "universes/", basename, "/");
		OS::makePath(dir);
		OS::makePath(dir + "probes/");
		OS::makePath(dir + "scenes/");
		OS::makePath(dir + "systems/");

		OS::OutputFile file;
		OutputMemoryStream blob(m_allocator);
		TextSerializer serializer(blob, m_entity_map);
		auto saveFile = [&file, &blob](const char* path) {
			if (file.open(path))
			{
				file.write(blob.getData(), blob.getPos());
				file.close();
			}
			else {
				logError("Editor") << "Failed to save " << path;
			}
		};
		for (IScene* scene : m_universe->getScenes())
		{
			blob.clear();
			serializer.write("version", scene->getVersion());
			scene->serialize(serializer);
			StaticString<MAX_PATH_LENGTH> scene_file_path(dir, "scenes/", scene->getPlugin().getName(), ".scn");
			saveFile(scene_file_path);
		}

		blob.clear();
		m_prefab_system->serialize(serializer);
		StaticString<MAX_PATH_LENGTH> system_file_path(dir, "systems/templates.sys");
		saveFile(system_file_path);

		for (EntityPtr entity = m_universe->getFirstEntity(); entity.isValid(); entity = m_universe->getNextEntity((EntityRef)entity))
		{
			const EntityRef e = (EntityRef)entity;
			if (m_prefab_system->getPrefab(e) != 0) continue;
			blob.clear();
			serializer.write("name", m_universe->getEntityName(e));
			serializer.write("transform", m_universe->getTransform(e).getRigidPart());
			serializer.write("scale", m_universe->getScale(e));
			EntityPtr parent = m_universe->getParent(e);
			serializer.write("parent", parent);
			EntityGUID guid = m_entity_map.get(entity);
			StaticString<MAX_PATH_LENGTH> entity_file_path(dir, guid.value, ".ent");
			for (ComponentUID cmp = m_universe->getFirstComponent(e); cmp.entity.isValid();
				 cmp = m_universe->getNextComponent(cmp))
			{
				const char* cmp_name = Reflection::getComponentTypeID(cmp.type.index);
				u32 type_hash = Reflection::getComponentTypeHash(cmp.type);
				serializer.write(cmp_name, type_hash);
				m_universe->serializeComponent(serializer, cmp.type, (EntityRef)cmp.entity);
			}
			serializer.write("cmp_end", (u32)0);
			saveFile(entity_file_path);
		}
		clearUniverseDir(dir);
	}


	void clearUniverseDir(const char* dir)
	{
		OS::FileInfo info;
		OS::FileIterator* file_iter = m_engine->getFileSystem().createFileIterator(dir);
		while (OS::getNextFile(file_iter, &info))
		{
			if (info.is_directory) continue;
			if (info.filename[0] == '.') continue;

			char basename[64];
			PathUtils::getBasename(basename, lengthOf(basename), info.filename);
			EntityGUID guid;
			fromCString(basename, lengthOf(basename), &guid.value);
			if (!m_entity_map.has(guid))
			{
				StaticString<MAX_PATH_LENGTH> filepath(dir, info.filename);
				OS::deleteFile(filepath);
			}
		}
		OS::destroyFileIterator(file_iter);
	}


	void save(IOutputStream& file)
	{
		while (m_engine->getFileSystem().hasWork()) m_engine->getFileSystem().updateAsyncTransactions();

		ASSERT(m_universe);

		OutputMemoryStream blob(m_allocator);
		blob.reserve(64 * 1024);

		Header header = {0xffffFFFF, (int)SerializedVersion::LATEST, 0, 0};
		blob.write(header);
		int hashed_offset = sizeof(header);

		header.engine_hash = m_engine->serialize(*m_universe, blob);
		m_prefab_system->serialize(blob);
		header.hash = crc32((const u8*)blob.getData() + hashed_offset, (int)blob.getPos() - hashed_offset);
		*(Header*)blob.getData() = header;
		file.write(blob.getData(), blob.getPos());

		logInfo("editor") << "Universe saved";
	}


	void setRenderInterface(class RenderInterface* interface) override
	{
		m_render_interface = interface;
		m_editor_icons->setRenderInterface(m_render_interface);
		createUniverse();
	}


	RenderInterface* getRenderInterface() override
	{
		return m_render_interface;
	}


	void setCustomPivot() override
	{
		if (m_selected_entities.empty()) return;

		DVec3 origin;
		Vec3 dir;		
		m_viewport.getRay(m_mouse_pos, origin, dir);
		auto hit = m_render_interface->castRay(origin, dir, INVALID_ENTITY);
		if (!hit.is_hit || hit.entity != m_selected_entities[0]) return;

		const DVec3 snap_pos = getClosestVertex(hit);

		const Transform tr = m_universe->getTransform(m_selected_entities[0]);
		m_gizmo->setOffset(tr.rot.conjugated() * (snap_pos - tr.pos).toFloat());
	}


	void snapDown() override
	{
		if (m_selected_entities.empty()) return;

		Array<DVec3> new_positions(m_allocator);
		Universe* universe = getUniverse();

		for (int i = 0; i < m_selected_entities.size(); ++i)
		{
			EntityRef entity = m_selected_entities[i];

			DVec3 origin = universe->getPosition(entity);
			auto hit = m_render_interface->castRay(origin, Vec3(0, -1, 0), m_selected_entities[i]);
			if (hit.is_hit)
			{
				new_positions.push(origin + Vec3(0, -hit.t, 0));
			}
			else
			{
				hit = m_render_interface->castRay(origin, Vec3(0, 1, 0), m_selected_entities[i]);
				if (hit.is_hit)
				{
					new_positions.push(origin + Vec3(0, hit.t, 0));
				}
				else
				{
					new_positions.push(universe->getPosition(m_selected_entities[i]));
				}
			}
		}
		setEntitiesPositions(&m_selected_entities[0], &new_positions[0], new_positions.size());
	}


	void makeParent(EntityPtr parent, EntityRef child) override
	{
		MakeParentCommand* command = LUMIX_NEW(m_allocator, MakeParentCommand)(*this, parent, child);
		executeCommand(command);
	}


	void destroyEntities(const EntityRef* entities, int count) override
	{
		DestroyEntitiesCommand* command = LUMIX_NEW(m_allocator, DestroyEntitiesCommand)(*this, entities, count);
		executeCommand(command);
	}


	void createEntityGUID(EntityRef entity) override
	{
		m_entity_map.create(entity);
	}


	void destroyEntityGUID(EntityRef entity) override
	{
		m_entity_map.erase(entity);
	}


	EntityGUID getEntityGUID(EntityRef entity) override
	{
		return m_entity_map.get(entity);
	}


	void makeAbsolute(char* absolute, int max_size, const char* relative) const override
	{
		bool is_absolute = relative[0] == '\\' || relative[0] == '/';
		is_absolute = is_absolute || (relative[0] != 0 && relative[1] == ':');

		if (is_absolute)
		{
			copyString(absolute, max_size, relative);
			return;
		}

		copyString(absolute, max_size, m_engine->getFileSystem().getBasePath());
		catString(absolute, max_size, relative);
	}


	void makeRelative(char* relative, int max_size, const char* absolute) const override
	{
		const char* base_path = m_engine->getFileSystem().getBasePath();
		if (startsWith(absolute, base_path)) {
			copyString(relative, max_size, absolute + stringLength(base_path));
			return;
		}
		copyString(relative, max_size, absolute);
	}


	EntityRef addEntity() override
	{
		return addEntityAt(m_viewport.w >> 1, m_viewport.h >> 1);
	}


	EntityRef addEntityAt(int camera_x, int camera_y) override
	{
		DVec3 origin;
		Vec3 dir;

		m_viewport.getRay({(float)camera_x, (float)camera_y}, origin, dir);
		auto hit = m_render_interface->castRay(origin, dir, INVALID_ENTITY);
		DVec3 pos;
		if (hit.is_hit) {
			pos = origin + dir * hit.t;
		}
		else {
			pos = m_viewport.pos + m_viewport.rot.rotate(Vec3(0, 0, -2));
		}
		AddEntityCommand* command = LUMIX_NEW(m_allocator, AddEntityCommand)(*this, pos);
		executeCommand(command);

		return (EntityRef)command->getEntity();
	}


	DVec3 getCameraRaycastHit() override
	{
		const Vec2 center(float(m_viewport.w >> 1), float(m_viewport.h >> 1));

		DVec3 origin;
		Vec3 dir;
		m_viewport.getRay(center, origin, dir);
		auto hit = m_render_interface->castRay(origin, dir, INVALID_ENTITY);
		DVec3 pos;
		if (hit.is_hit) {
			pos = origin + dir * hit.t;
		}
		else {
			pos = m_viewport.pos + m_viewport.rot.rotate(Vec3(0, 0, -2));
		}
		return pos;
	}


	void setEntitiesScales(const EntityRef* entities, const float* scales, int count) override
	{
		if (count <= 0) return;

		IEditorCommand* command =
			LUMIX_NEW(m_allocator, ScaleEntityCommand)(*this, entities, scales, count, m_allocator);
		executeCommand(command);
	}


	void setEntitiesScale(const EntityRef* entities, int count, float scale) override
	{
		if (count <= 0) return;

		IEditorCommand* command =
			LUMIX_NEW(m_allocator, ScaleEntityCommand)(*this, entities, count, scale, m_allocator);
		executeCommand(command);
	}


	void setEntitiesRotations(const EntityRef* entities, const Quat* rotations, int count) override
	{
		ASSERT(entities && rotations);
		if (count <= 0) return;

		Universe* universe = getUniverse();
		Array<DVec3> positions(m_allocator);
		for (int i = 0; i < count; ++i)
		{
			positions.push(universe->getPosition(entities[i]));
		}
		IEditorCommand* command =
			LUMIX_NEW(m_allocator, MoveEntityCommand)(*this, entities, &positions[0], rotations, count, m_allocator);
		executeCommand(command);
	}


	void setEntitiesCoordinate(const EntityRef* entities, int count, double value, Coordinate coord) override
	{
		ASSERT(entities);
		if (count <= 0) return;

		Universe* universe = getUniverse();
		Array<Quat> rots(m_allocator);
		Array<DVec3> poss(m_allocator);
		rots.reserve(count);
		poss.reserve(count);
		for (int i = 0; i < count; ++i)
		{
			rots.push(universe->getRotation(entities[i]));
			poss.push(universe->getPosition(entities[i]));
			(&poss[i].x)[(int)coord] = value;
		}
		IEditorCommand* command =
			LUMIX_NEW(m_allocator, MoveEntityCommand)(*this, entities, &poss[0], &rots[0], count, m_allocator);
		executeCommand(command);
	}


	void setEntitiesLocalCoordinate(const EntityRef* entities, int count, double value, Coordinate coord) override
	{
		ASSERT(entities);
		if (count <= 0) return;

		Universe* universe = getUniverse();
		Array<DVec3> poss(m_allocator);
		poss.reserve(count);
		for (int i = 0; i < count; ++i)
		{
			poss.push(universe->getLocalTransform(entities[i]).pos);
			(&poss[i].x)[(int)coord] = value;
		}
		IEditorCommand* command =
			LUMIX_NEW(m_allocator, LocalMoveEntityCommand)(*this, entities, &poss[0], count, m_allocator);
		executeCommand(command);
	}


	void setEntitiesPositions(const EntityRef* entities, const DVec3* positions, int count) override
	{
		ASSERT(entities && positions);
		if (count <= 0) return;

		Universe* universe = getUniverse();
		Array<Quat> rots(m_allocator);
		for (int i = 0; i < count; ++i)
		{
			rots.push(universe->getRotation(entities[i]));
		}
		IEditorCommand* command =
			LUMIX_NEW(m_allocator, MoveEntityCommand)(*this, entities, positions, &rots[0], count, m_allocator);
		executeCommand(command);
	}

	void setEntitiesPositionsAndRotations(const EntityRef* entities,
		const DVec3* positions,
		const Quat* rotations,
		int count) override
	{
		if (count <= 0) return;
		IEditorCommand* command =
			LUMIX_NEW(m_allocator, MoveEntityCommand)(*this, entities, positions, rotations, count, m_allocator);
		executeCommand(command);
	}


	void setEntityName(EntityRef entity, const char* name) override
	{
		IEditorCommand* command = LUMIX_NEW(m_allocator, SetEntityNameCommand)(*this, entity, name);
		executeCommand(command);
	}


	void beginCommandGroup(u32 type) override
	{
		if(m_undo_index < m_undo_stack.size() - 1)
		{
			for(int i = m_undo_stack.size() - 1; i > m_undo_index; --i)
			{
				LUMIX_DELETE(m_allocator, m_undo_stack[i]);
			}
			m_undo_stack.resize(m_undo_index + 1);
		}

		if(m_undo_index >= 0)
		{
			static const u32 end_group_hash = crc32("end_group");
			if(crc32(m_undo_stack[m_undo_index]->getType()) == end_group_hash)
			{
				if(static_cast<EndGroupCommand*>(m_undo_stack[m_undo_index])->group_type == type)
				{
					LUMIX_DELETE(m_allocator, m_undo_stack[m_undo_index]);
					--m_undo_index;
					m_undo_stack.pop();
					return;
				}
			}
		}

		m_current_group_type = type;
		auto* cmd = LUMIX_NEW(m_allocator, BeginGroupCommand);
		m_undo_stack.push(cmd);
		++m_undo_index;
	}


	void endCommandGroup() override
	{
		if (m_undo_index < m_undo_stack.size() - 1)
		{
			for (int i = m_undo_stack.size() - 1; i > m_undo_index; --i)
			{
				LUMIX_DELETE(m_allocator, m_undo_stack[i]);
			}
			m_undo_stack.resize(m_undo_index + 1);
		}

		auto* cmd = LUMIX_NEW(m_allocator, EndGroupCommand);
		cmd->group_type = m_current_group_type;
		m_undo_stack.push(cmd);
		++m_undo_index;
	}


	void executeCommand(IEditorCommand* command) override
	{
		if (!m_command_queue.empty() || !command->isReady()) {
			m_command_queue.push(command);
			return;
		}

		doExecute(command);
	}


	void doExecute(IEditorCommand* command)
	{
		ASSERT(command->isReady());
		
		logInfo("Editor") << "Executing editor command " << command->getType() << "...";
		m_is_universe_changed = true;
		if (m_undo_index >= 0 && command->getType() == m_undo_stack[m_undo_index]->getType())
		{
			if (command->merge(*m_undo_stack[m_undo_index]))
			{
				m_undo_stack[m_undo_index]->execute();
				LUMIX_DELETE(m_allocator, command);
				return;
			}
		}

		if (command->execute())
		{
			if (m_undo_index < m_undo_stack.size() - 1)
			{
				for (int i = m_undo_stack.size() - 1; i > m_undo_index; --i)
				{
					LUMIX_DELETE(m_allocator, m_undo_stack[i]);
				}
				m_undo_stack.resize(m_undo_index + 1);
			}
			m_undo_stack.push(command);
			if (m_is_game_mode) ++m_game_mode_commands;
			++m_undo_index;
			return;
		}
		else {
			logError("Editor") << "Editor command failed";
		}
		LUMIX_DELETE(m_allocator, command);	
	}


	bool isGameMode() const override { return m_is_game_mode; }


	void toggleGameMode() override
	{
		ASSERT(m_universe);
		if (m_is_game_mode) {
			stopGameMode(true);
			return;
		}

		m_selected_entity_on_game_mode = m_selected_entities.empty() ? INVALID_ENTITY : m_selected_entities[0];
		m_game_mode_file.clear();
		save(m_game_mode_file);
		m_is_game_mode = true;
		beginCommandGroup(0);
		endCommandGroup();
		m_game_mode_commands = 2;
		m_engine->startGame(*m_universe);
	}


	void stopGameMode(bool reload)
	{
		for (int i = 0; i < m_game_mode_commands; ++i)
		{
			LUMIX_DELETE(m_allocator, m_undo_stack.back());
			m_undo_stack.pop();
			--m_undo_index;
		}

		ASSERT(m_universe);
		m_engine->getResourceManager().enableUnload(false);
		m_engine->stopGame(*m_universe);
		selectEntities(nullptr, 0, false);
		m_gizmo->clearEntities();
		m_editor_icons->clear();
		m_is_game_mode = false;
		if (reload)
		{
			m_universe_destroyed.invoke();
			StaticString<64> name(m_universe->getName());
			m_engine->destroyUniverse(*m_universe);
			
			m_universe = &m_engine->createUniverse(true);
			m_universe_created.invoke();
			m_universe->setName(name);
			m_universe->entityDestroyed().bind<WorldEditorImpl, &WorldEditorImpl::onEntityDestroyed>(this);
			m_selected_entities.clear();
            InputMemoryStream file(m_game_mode_file);
			load(file);
		}
		m_game_mode_file.clear();
		if(m_selected_entity_on_game_mode.isValid()) {
			EntityRef e = (EntityRef)m_selected_entity_on_game_mode;
			selectEntities(&e, 1, false);
		}
		m_engine->getResourceManager().enableUnload(true);
	}


	PrefabSystem& getPrefabSystem() override
	{
		return *m_prefab_system;
	}


	void copyEntities(const EntityRef* entities, int count, ISerializer& serializer) override
	{
		serializer.write("count", count);
		for (int i = 0; i < count; ++i)
		{
			EntityRef entity = entities[i];
			Transform tr = m_universe->getTransform(entity);
			serializer.write("transform", tr);
			serializer.write("parent", m_universe->getParent(entity));

			i32 cmp_count = 0;
			for (ComponentUID cmp = m_universe->getFirstComponent(entity); cmp.isValid();
				 cmp = m_universe->getNextComponent(cmp))
			{
				++cmp_count;
			}

			serializer.write("cmp_count", cmp_count);
			for (ComponentUID cmp = m_universe->getFirstComponent(entity);
				cmp.isValid();
				cmp = m_universe->getNextComponent(cmp))
			{
				u32 cmp_type = Reflection::getComponentTypeHash(cmp.type);
				serializer.write("cmp_type", cmp_type);
				const Reflection::ComponentBase* cmp_desc = Reflection::getComponent(cmp.type);
				
				m_universe->serializeComponent(serializer, cmp.type, (EntityRef)cmp.entity);
			}
		}
	}


	void copyEntities() override
	{
		if (m_selected_entities.empty()) return;

		m_copy_buffer.clear();

		struct : ISaveEntityGUIDMap {
			EntityGUID get(EntityPtr entity) override {
				if (!entity.isValid()) return INVALID_ENTITY_GUID;
				
				int idx = editor->m_selected_entities.indexOf((EntityRef)entity);
				if (idx >= 0) {
					return { (u64)idx };
				}
				return { ((u64)1 << 32) | (u64)entity.index };
			}

			WorldEditorImpl* editor;
		} map;
		map.editor = this;

		TextSerializer serializer(m_copy_buffer, map);

		Array<EntityRef> entities(m_allocator);
		entities = m_selected_entities;
		for (EntityRef e : entities) {
			for (EntityPtr child = m_universe->getFirstChild(e); 
				child.isValid(); 
				child = m_universe->getNextSibling((EntityRef)child)) 
			{
				if(entities.indexOf((EntityRef)child) < 0) entities.push((EntityRef)child);
			}
		}
		copyEntities(&entities[0], entities.size(), serializer);
	}


	bool canPasteEntities() const override
	{
		return m_copy_buffer.getPos() > 0;
	}


	void pasteEntities() override;
	void duplicateEntities() override;


	void cloneComponent(const ComponentUID& src, EntityRef entity) override
	{
		IScene* scene = m_universe->getScene(src.type);
		m_universe->createComponent(src.type, entity);
		ComponentUID clone(entity, src.type, scene);

		const Reflection::ComponentBase* cmp_desc = Reflection::getComponent(src.type);
		OutputMemoryStream stream(m_allocator);
		
		SaveVisitor save;
		save.stream = &stream;
		save.cmp = src;
		cmp_desc->visit(save);

		InputMemoryStream blob(stream);
		::Lumix::load(clone, -1, blob);
	}


	void destroyComponent(const EntityRef* entities, int count, ComponentType cmp_type) override
	{
		ASSERT(count > 0);
		IEditorCommand* command = LUMIX_NEW(m_allocator, DestroyComponentCommand)(*this, entities, count, cmp_type);
		executeCommand(command);
	}


	void addComponent(ComponentType cmp_type) override
	{
		if (!m_selected_entities.empty())
		{
			IEditorCommand* command = LUMIX_NEW(m_allocator, AddComponentCommand)(*this, m_selected_entities, cmp_type);
			executeCommand(command);
		}
	}

	void copyViewTransform() override {
		if (m_selected_entities.empty()) return;
		const Universe* universe = getUniverse();

		setEntitiesPositionsAndRotations(m_selected_entities.begin(), &m_viewport.pos, &m_viewport.rot, 1);
	}

	void lookAtSelected() override
	{
		const Universe* universe = getUniverse();
		if (m_selected_entities.empty()) return;

		m_go_to_parameters.m_is_active = true;
		m_go_to_parameters.m_t = 0;
		m_go_to_parameters.m_from = m_viewport.pos;
		const Vec3 dir = m_viewport.rot.rotate(Vec3(0, 0, 1));
		m_go_to_parameters.m_to = universe->getPosition(m_selected_entities[0]) + dir * 10;
		const double len = (m_go_to_parameters.m_to - m_go_to_parameters.m_from).length();
		m_go_to_parameters.m_speed = maximum(100.0f / (len > 0 ? float(len) : 1), 2.0f);
		m_go_to_parameters.m_from_rot = m_go_to_parameters.m_to_rot = m_viewport.rot;
	}


	void loadUniverse(const char* basename) override
	{
		if (m_is_game_mode) stopGameMode(false);
		destroyUniverse();
		createUniverse();
		m_universe->setName(basename);
		logInfo("Editor") << "Loading universe " << basename << "...";
		if (!deserialize(*m_universe, "universes/", basename, *m_prefab_system, m_entity_map, m_allocator)) newUniverse();
		m_editor_icons->refresh();
	}


	void newUniverse() override
	{
		destroyUniverse();
		createUniverse();
		logInfo("Editor") << "Universe created.";
	}


	enum class SerializedVersion : int
	{

		LATEST
	};


	#pragma pack(1)
		struct Header
		{
			u32 magic;
			int version;
			u32 hash;
			u32 engine_hash;
		};
	#pragma pack()


	void load(IInputStream& file)
	{
		m_is_loading = true;
		ASSERT(file.getBuffer());
		Header header;
		if (file.size() < sizeof(header))
		{
			logError("Editor") << "Corrupted file.";
			newUniverse();
			m_is_loading = false;
			return;
		}

		OS::Timer timer;
		logInfo("Editor") << "Parsing universe...";
		InputMemoryStream blob(file.getBuffer(), (int)file.size());
		u32 hash = 0;
		blob.read(hash);
		header.version = -1;
		int hashed_offset = sizeof(hash);
		if (hash == 0xFFFFffff)
		{
			blob.rewind();
			blob.read(header);
			hashed_offset = sizeof(header);
			hash = header.hash;
		}
		else
		{
			u32 engine_hash = 0;
			blob.read(engine_hash);
		}
		if (crc32((const u8*)blob.getData() + hashed_offset, (int)blob.size() - hashed_offset) != hash)
		{
			logError("Editor") << "Corrupted file.";
			newUniverse();
			m_is_loading = false;
			return;
		}

		if (m_engine->deserialize(*m_universe, blob))
		{
			m_prefab_system->deserialize(blob);
			logInfo("Editor") << "Universe parsed in " << timer.getTimeSinceStart() << " seconds";
		}
		else
		{
			newUniverse();
		}
		m_is_loading = false;
	}


	template <typename T>
	static IEditorCommand* constructEditorCommand(WorldEditor& editor)
	{
		return LUMIX_NEW(editor.getAllocator(), T)(editor);
	}


	Gizmo& getGizmo() override { return *m_gizmo; }


	EditorIcons& getIcons() override
	{
		return *m_editor_icons;
	}


	WorldEditorImpl(const char* base_path, Engine& engine, IAllocator& allocator)
		: m_allocator(allocator)
		, m_entity_selected(m_allocator)
		, m_universe_destroyed(m_allocator)
		, m_universe_created(m_allocator)
		, m_selected_entities(m_allocator)
		, m_editor_icons(nullptr)
		, m_plugins(m_allocator)
		, m_undo_stack(m_allocator)
		, m_copy_buffer(m_allocator)
		, m_is_loading(false)
		, m_universe(nullptr)
		, m_is_orbit(false)
		, m_is_toggle_selection(false)
		, m_mouse_sensitivity(200, 200)
		, m_render_interface(nullptr)
		, m_selected_entity_on_game_mode(INVALID_ENTITY)
		, m_mouse_handling_plugin(nullptr)
		, m_is_game_mode(false)
		, m_snap_mode(SnapMode::NONE)
		, m_undo_index(-1)
		, m_engine(&engine)
		, m_entity_map(m_allocator)
		, m_is_guid_pseudorandom(false)
        , m_game_mode_file(m_allocator)
		, m_command_queue(m_allocator)
	{
		logInfo("Editor") << "Initializing editor...";
		m_viewport.is_ortho = false;
		m_viewport.pos = DVec3(0);
		m_viewport.rot.set(0, 0, 0, 1);
		m_viewport.w = -1;
		m_viewport.h = -1;
		m_viewport.fov = degreesToRadians(30.f);
		m_viewport.near = 0.1f;
		m_viewport.far = 100000.f;

		for (auto& i : m_is_mouse_down) i = false;
		for (auto& i : m_is_mouse_click) i = false;
		m_go_to_parameters.m_is_active = false;
		
		m_measure_tool = LUMIX_NEW(m_allocator, MeasureTool)();
		addPlugin(*m_measure_tool);

		const char* plugins[] = { ""
			#ifdef LUMIXENGINE_PLUGINS
				, LUMIXENGINE_PLUGINS
			#endif
		};

		PluginManager& plugin_manager = m_engine->getPluginManager();
		for (auto* plugin_name : plugins) {
			if (plugin_name[0] && !plugin_manager.load(plugin_name)) {
				logInfo("Editor") << plugin_name << " plugin has not been loaded";
			}
		}

		OS::InitWindowArgs create_win_args;
		create_win_args.name = "Lumix Studio";
		create_win_args.handle_file_drops = true;
		m_window = OS::createWindow(create_win_args);
		Engine::PlatformData platform_data = {};
		platform_data.window_handle = m_window;
		m_engine->setPlatformData(platform_data);

		plugin_manager.initPlugins();

		m_prefab_system = PrefabSystem::create(*this);

		m_gizmo = Gizmo::create(*this);
		m_editor_icons = EditorIcons::create(*this);

		char command_line[2048];
		OS::getCommandLine(command_line, lengthOf(command_line));
		CommandLineParser parser(command_line);
		while (parser.next())
		{
			if (parser.currentEquals("-pseudorandom_guid"))
			{
				m_is_guid_pseudorandom = true;
				break;
			}
		}
	}


	OS::WindowHandle getWindow() override
	{
		return m_window;
	}


	void navigate(float forward, float right, float up, float speed) override
	{
		const Quat rot = m_viewport.rot;

		right = m_is_orbit ? 0 : right;

		m_viewport.pos += rot.rotate(Vec3(0, 0, -1)) * forward * speed;
		m_viewport.pos += rot.rotate(Vec3(1, 0, 0)) * right * speed;
		m_viewport.pos += rot.rotate(Vec3(0, 1, 0)) * up * speed;
	}


	bool isEntitySelected(EntityRef entity) const override
	{
		return m_selected_entities.indexOf(entity) >= 0;
	}


	const Array<EntityRef>& getSelectedEntities() const override
	{
		return m_selected_entities;
	}


	void setSnapMode(bool enable, bool vertex_snap) override
	{
		m_snap_mode = enable ? (vertex_snap ? SnapMode::VERTEX : SnapMode::FREE) : SnapMode::NONE;
	}


	void setToggleSelection(bool is_toggle) override { m_is_toggle_selection = is_toggle; }


	void addArrayPropertyItem(const ComponentUID& cmp, const Reflection::IArrayProperty& property) override
	{
		if (cmp.isValid())
		{
			IEditorCommand* command =
				LUMIX_NEW(m_allocator, AddArrayPropertyItemCommand)(*this, cmp, property);
			executeCommand(command);
		}
	}


	void removeArrayPropertyItem(const ComponentUID& cmp, int index, const Reflection::IArrayProperty& property) override
	{
		if (cmp.isValid())
		{
			IEditorCommand* command =
				LUMIX_NEW(m_allocator, RemoveArrayPropertyItemCommand)(*this, cmp, index, property);
			executeCommand(command);
		}
	}


	void setProperty(ComponentType component_type,
		int index,
		const Reflection::PropertyBase& property,
		const EntityRef* entities,
		int count,
		const void* data,
		int size) override
	{
		IEditorCommand* command = LUMIX_NEW(m_allocator, SetPropertyCommand)(
			*this, entities, count, component_type, index, property, data, size);
		executeCommand(command);
	}


	bool isOrbitCamera() const override { return m_is_orbit; }


	void setOrbitCamera(bool enable) override
	{
		m_orbit_delta = Vec2(0, 0);
		m_is_orbit = enable;
	}


	void panCamera(float x, float y)
	{
		const Quat rot = m_viewport.rot;

		if(m_is_orbit) {
			m_orbit_delta.x += x;
			m_orbit_delta.y += y;
		}

		m_viewport.pos += rot.rotate(Vec3(x, 0, 0));
		m_viewport.pos += rot.rotate(Vec3(0, -y, 0));
	}


	Vec2 getMouseSensitivity() override
	{
		return m_mouse_sensitivity;
	}


	void setMouseSensitivity(float x, float y) override
	{
		m_mouse_sensitivity.x = 10000 / x;
		m_mouse_sensitivity.y = 10000 / y;
	}


	void rotateCamera(int x, int y)
	{
		const Universe* universe = getUniverse();
		DVec3 pos = m_viewport.pos;
		Quat rot = m_viewport.rot;
		const Quat old_rot = rot;

		float yaw = -signum(x) * (pow(abs((float)x / m_mouse_sensitivity.x), 1.2f));
		Quat yaw_rot(Vec3(0, 1, 0), yaw);
		rot = yaw_rot * rot;
		rot.normalize();

		Vec3 pitch_axis = rot.rotate(Vec3(1, 0, 0));
		float pitch = -signum(y) * (pow(abs((float)y / m_mouse_sensitivity.y), 1.2f));
		const Quat pitch_rot(pitch_axis, pitch);
		rot = pitch_rot * rot;
		rot.normalize();

		if (m_is_orbit && !m_selected_entities.empty())
		{
			const Vec3 dir = rot.rotate(Vec3(0, 0, 1));
			const DVec3 entity_pos = universe->getPosition(m_selected_entities[0]);
			DVec3 nondelta_pos = pos;

			nondelta_pos -= old_rot.rotate(Vec3(0, -1, 0)) * m_orbit_delta.y;
			nondelta_pos -= old_rot.rotate(Vec3(1, 0, 0)) * m_orbit_delta.x;

			const float dist = float((entity_pos - nondelta_pos).length());
			pos = entity_pos + dir * dist;
			pos += rot.rotate(Vec3(1, 0, 0)) * m_orbit_delta.x;
			pos += rot.rotate(Vec3(0, -1, 0)) * m_orbit_delta.y;
		}

		m_viewport.pos = pos;
		m_viewport.rot = rot;
	}


	void selectEntities(const EntityRef* entities, int count, bool toggle) override
	{
		if (!toggle || !m_is_toggle_selection)
		{
			m_gizmo->clearEntities();
			m_selected_entities.clear();
			for (int i = 0; i < count; ++i)
			{
				m_selected_entities.push(entities[i]);
			}
		}
		else
		{
			for (int i = 0; i < count; ++i)
			{
				int idx = m_selected_entities.indexOf(entities[i]);
				if (idx < 0)
				{
					m_selected_entities.push(entities[i]);
				}
				else
				{
					m_selected_entities.eraseFast(idx);
				}
			}
		}

		m_selected_entities.removeDuplicates();
		m_entity_selected.invoke(m_selected_entities);
	}


	void onEntityDestroyed(EntityRef entity)
	{
		m_selected_entities.eraseItemFast(entity);
	}


	void destroyUniverse()
	{
		if (m_is_game_mode) stopGameMode(false);

		ASSERT(m_universe);
		destroyUndoStack();
		m_universe_destroyed.invoke();
		m_editor_icons->clear();
		m_gizmo->clearEntities();
		selectEntities(nullptr, 0, false);
		m_engine->destroyUniverse(*m_universe);
		m_universe = nullptr;
	}


	DelegateList<void()>& universeCreated() override
	{
		return m_universe_created;
	}


	DelegateList<void(const Array<EntityRef>&)>& entitySelected() override
	{
		return m_entity_selected;
	}


	DelegateList<void()>& universeDestroyed() override
	{
		return m_universe_destroyed;
	}


	void destroyUndoStack()
	{
		m_undo_index = -1;
		for (int i = 0; i < m_undo_stack.size(); ++i)
		{
			LUMIX_DELETE(m_allocator, m_undo_stack[i]);
		}
		m_undo_stack.clear();
	}


	void createUniverse()
	{
		ASSERT(!m_universe);

		m_is_universe_changed = false;
		destroyUndoStack();
		m_universe = &m_engine->createUniverse(true);
		Universe* universe = m_universe;

		universe->entityDestroyed().bind<WorldEditorImpl, &WorldEditorImpl::onEntityDestroyed>(this);

		m_is_orbit = false;
		m_selected_entities.clear();
		m_universe_created.invoke();

		m_entity_map.is_random = !m_is_guid_pseudorandom;
		m_entity_map.clear();
	}


	bool canUndo() const override
	{
		return !m_is_game_mode && m_undo_index < m_undo_stack.size() && m_undo_index >= 0;
	}


	bool canRedo() const override
	{
		return !m_is_game_mode && m_undo_index + 1 < m_undo_stack.size();
	}


	void undo() override
	{
		if (m_is_game_mode) return;

		static const u32 end_group_hash = crc32("end_group");
		static const u32 begin_group_hash = crc32("begin_group");

		if (m_undo_index >= m_undo_stack.size() || m_undo_index < 0) return;

		if(crc32(m_undo_stack[m_undo_index]->getType()) == end_group_hash)
		{
			--m_undo_index;
			while(crc32(m_undo_stack[m_undo_index]->getType()) != begin_group_hash)
			{
				m_undo_stack[m_undo_index]->undo();
				--m_undo_index;
			}
			--m_undo_index;
		}
		else
		{
			m_undo_stack[m_undo_index]->undo();
			--m_undo_index;
		}
	}


	void redo() override
	{
		if (m_is_game_mode) return;

		static const u32 end_group_hash = crc32("end_group");
		static const u32 begin_group_hash = crc32("begin_group");

		if (m_undo_index + 1 >= m_undo_stack.size()) return;

		++m_undo_index;
		if(crc32(m_undo_stack[m_undo_index]->getType()) == begin_group_hash)
		{
			++m_undo_index;
			while(crc32(m_undo_stack[m_undo_index]->getType()) != end_group_hash)
			{
				m_undo_stack[m_undo_index]->execute();
				++m_undo_index;
			}
		}
		else
		{
			m_undo_stack[m_undo_index]->execute();
		}
	}


	MeasureTool* getMeasureTool() const override
	{
		return m_measure_tool;
	}


	double getMeasuredDistance() const override
	{
		return m_measure_tool->getDistance();
	}


	bool isMeasureToolActive() const override
	{
		return m_measure_tool->isEnabled();
	}


	void toggleMeasure() override
	{
		m_measure_tool->enable(!m_measure_tool->isEnabled());
	}


	void setTopView() override
	{
		m_go_to_parameters.m_is_active = true;
		m_go_to_parameters.m_t = 0;
		auto* universe = m_universe;
		m_go_to_parameters.m_from = m_viewport.pos;
		m_go_to_parameters.m_to = m_go_to_parameters.m_from;
		if (m_is_orbit && !m_selected_entities.empty())
		{
			m_go_to_parameters.m_to = universe->getPosition(m_selected_entities[0]) + Vec3(0, 10, 0);
		}
		m_go_to_parameters.m_speed = 2.0f;
		m_go_to_parameters.m_from_rot = m_viewport.rot;
		m_go_to_parameters.m_to_rot = Quat(Vec3(1, 0, 0), -PI * 0.5f);
	}


	void setFrontView() override
	{
		m_go_to_parameters.m_is_active = true;
		m_go_to_parameters.m_t = 0;
		auto* universe = m_universe;
		m_go_to_parameters.m_from = m_viewport.pos;
		m_go_to_parameters.m_to = m_go_to_parameters.m_from;
		if (m_is_orbit && !m_selected_entities.empty())
		{
			m_go_to_parameters.m_to = universe->getPosition(m_selected_entities[0]) + Vec3(0, 0, -10);
		}
		m_go_to_parameters.m_speed = 2.0f;
		m_go_to_parameters.m_from_rot = m_viewport.rot;
		m_go_to_parameters.m_to_rot = Quat(Vec3(0, 1, 0), PI);
	}


	void setSideView() override
	{
		m_go_to_parameters.m_is_active = true;
		m_go_to_parameters.m_t = 0;
		auto* universe = m_universe;
		m_go_to_parameters.m_from = m_viewport.pos;
		m_go_to_parameters.m_to = m_go_to_parameters.m_from;
		if (m_is_orbit && !m_selected_entities.empty())
		{
			m_go_to_parameters.m_to = universe->getPosition(m_selected_entities[0]) + Vec3(-10, 0, 0);
		}
		m_go_to_parameters.m_speed = 2.0f;
		m_go_to_parameters.m_from_rot = m_viewport.rot;
		m_go_to_parameters.m_to_rot = Quat(Vec3(0, 1, 0), -PI * 0.5f);
	}


	static int getEntitiesCount(Universe& universe)
	{
		int count = 0;
		for (EntityPtr e = universe.getFirstEntity(); e.isValid(); e = universe.getNextEntity((EntityRef)e)) ++count;
		return count;
	}


private:
	enum class MouseMode
	{
		NONE,
		SELECT,
		NAVIGATE,
		PAN,

		CUSTOM
	};

	enum class SnapMode
	{
		NONE,
		FREE,
		VERTEX
	};

	struct GoToParameters
	{
		bool m_is_active;
		DVec3 m_from;
		DVec3 m_to;
		Quat m_from_rot;
		Quat m_to_rot;
		float m_t;
		float m_speed;
	};

	IAllocator& m_allocator;
	GoToParameters m_go_to_parameters;
	Gizmo* m_gizmo;
	Array<EntityRef> m_selected_entities;
	MouseMode m_mouse_mode;
	Vec2 m_rect_selection_start;
	EditorIcons* m_editor_icons;
	Vec2 m_mouse_pos;
	float m_mouse_rel_x;
	float m_mouse_rel_y;
	Vec2 m_orbit_delta;
	Vec2 m_mouse_sensitivity;
	bool m_is_game_mode;
	int m_game_mode_commands;
	bool m_is_orbit;
	bool m_is_toggle_selection;
	SnapMode m_snap_mode;
	OutputMemoryStream m_game_mode_file;
	Engine* m_engine;
	OS::WindowHandle m_window;
	EntityPtr m_selected_entity_on_game_mode;
	DelegateList<void()> m_universe_destroyed;
	DelegateList<void()> m_universe_created;
	DelegateList<void(const Array<EntityRef>&)> m_entity_selected;
	bool m_is_mouse_down[(int)OS::MouseButton::EXTENDED];
	bool m_is_mouse_click[(int)OS::MouseButton::EXTENDED];

	Array<Plugin*> m_plugins;
	MeasureTool* m_measure_tool;
	Plugin* m_mouse_handling_plugin;
	PrefabSystem* m_prefab_system;
	Array<IEditorCommand*> m_undo_stack;
	Array<IEditorCommand*> m_command_queue;
	int m_undo_index;
	OutputMemoryStream m_copy_buffer;
	bool m_is_loading;
	Universe* m_universe;
	EntityGUIDMap m_entity_map;
	RenderInterface* m_render_interface;
	u32 m_current_group_type;
	bool m_is_universe_changed;
	bool m_is_guid_pseudorandom;
	Viewport m_viewport;
};


class PasteEntityCommand final : public IEditorCommand
{
public:
	explicit PasteEntityCommand(WorldEditor& editor)
		: m_copy_buffer(editor.getAllocator())
		, m_editor(editor)
		, m_entities(editor.getAllocator())
		, m_identity(false)
	{
	}


	PasteEntityCommand(WorldEditor& editor, const OutputMemoryStream& copy_buffer, bool identity = false)
		: m_copy_buffer(copy_buffer)
		, m_editor(editor)
		, m_position(editor.getCameraRaycastHit())
		, m_entities(editor.getAllocator())
		, m_identity(identity)
	{
	}


	PasteEntityCommand(WorldEditor& editor, const DVec3& pos, const OutputMemoryStream& copy_buffer, bool identity = false)
		: m_copy_buffer(copy_buffer)
		, m_editor(editor)
		, m_position(pos)
		, m_entities(editor.getAllocator())
		, m_identity(identity)
	{
	}


	bool execute() override
	{
		struct Map : ILoadEntityGUIDMap {
			Map(IAllocator& allocator) : entities(allocator) {}

			EntityPtr get(EntityGUID guid) override 
			{
				if (guid == INVALID_ENTITY_GUID) return INVALID_ENTITY;

				if (guid.value > 0xffFFffFF) return { (int)guid.value }; ;
				
				return entities[(int)guid.value];
			}

			Array<EntityRef> entities;
		} map(m_editor.getAllocator());
		InputMemoryStream input_blob(m_copy_buffer);
		TextDeserializer deserializer(input_blob, map);

		Universe& universe = *m_editor.getUniverse();
		int entity_count;
		deserializer.read(&entity_count);
		map.entities.resize(entity_count);
		bool is_redo = !m_entities.empty();
		for (int i = 0; i < entity_count; ++i)
		{
			if (is_redo)
			{
				map.entities[i] = m_entities[i];
				universe.emplaceEntity(m_entities[i]);
			}
			else
			{
				map.entities[i] = universe.createEntity(DVec3(0), Quat(0, 0, 0, 1));
			}
		}

		m_entities.reserve(entity_count);

		Transform base_tr;
		base_tr.pos = m_position;
		base_tr.scale = 1;
		base_tr.rot = Quat(0, 0, 0, 1);
		for (int i = 0; i < entity_count; ++i)
		{
			Transform tr;
			deserializer.read(&tr);
			EntityPtr parent;
			deserializer.read(&parent);

			if (!m_identity)
			{
				if (i == 0)
				{
					const Transform inv = tr.inverted();
					base_tr.rot = tr.rot;
					base_tr.scale = tr.scale;
					base_tr = base_tr * inv;
					tr.pos = m_position;
				}
				else
				{
					tr = base_tr * tr;
				}
			}

			const EntityRef new_entity = map.entities[i];
			((WorldEditorImpl&)m_editor).m_entity_map.create(new_entity);
			if (!is_redo) m_entities.push(new_entity);
			universe.setTransform(new_entity, tr);
			universe.setParent(parent, new_entity);
			i32 count;
			deserializer.read(&count);
			for (int j = 0; j < count; ++j)
			{
				u32 hash;
				deserializer.read(&hash);
				ComponentType type = Reflection::getComponentTypeFromHash(hash);
				const int scene_version = universe.getScene(type)->getVersion();
				universe.deserializeComponent(deserializer, new_entity, type, scene_version);
			}
		}
		return true;
	}


	void undo() override
	{
		for (auto entity : m_entities) {
			m_editor.getUniverse()->destroyEntity(entity);
			((WorldEditorImpl&)m_editor).m_entity_map.erase(entity);
		}
	}


	const char* getType() override { return "paste_entity"; }


	bool merge(IEditorCommand& command) override
	{
		ASSERT(command.getType() == getType());
		return false;
	}


	const Array<EntityRef>& getEntities() { return m_entities; }


private:
	OutputMemoryStream m_copy_buffer;
	WorldEditor& m_editor;
	DVec3 m_position;
	Array<EntityRef> m_entities;
	bool m_identity;
};


void WorldEditorImpl::pasteEntities()
{
	if (!canPasteEntities()) return;
	PasteEntityCommand* command = LUMIX_NEW(m_allocator, PasteEntityCommand)(*this, m_copy_buffer);
	executeCommand(command);
}


void WorldEditorImpl::duplicateEntities()
{
	copyEntities();

	PasteEntityCommand* command = LUMIX_NEW(m_allocator, PasteEntityCommand)(*this, m_copy_buffer, true);
	executeCommand(command);
}


WorldEditor* WorldEditor::create(const char* base_path, Engine& engine, IAllocator& allocator)
{
	return LUMIX_NEW(allocator, WorldEditorImpl)(base_path, engine, allocator);
}


void WorldEditor::destroy(WorldEditor* editor, IAllocator& allocator)
{
	LUMIX_DELETE(allocator, static_cast<WorldEditorImpl*>(editor));
}


} // namespace Lumix
