#include "universe.h"
#include "engine/crc32.h"
#include "engine/iplugin.h"
#include "engine/log.h"
#include "engine/math.h"
#include "engine/prefab.h"
#include "engine/reflection.h"
#include "engine/serializer.h"
#include "engine/universe/component.h"


namespace Lumix
{


static const int RESERVED_ENTITIES_COUNT = 5000;


Universe::~Universe() = default;


Universe::Universe(IAllocator& allocator)
	: m_allocator(allocator)
	, m_names(m_allocator)
	, m_entities(m_allocator)
	, m_component_added(m_allocator)
	, m_component_destroyed(m_allocator)
	, m_entity_created(m_allocator)
	, m_entity_destroyed(m_allocator)
	, m_entity_moved(m_allocator)
	, m_first_free_slot(-1)
	, m_scenes(m_allocator)
	, m_hierarchy(m_allocator)
	, m_transforms(m_allocator)
{
	m_entities.reserve(RESERVED_ENTITIES_COUNT);
	m_transforms.reserve(RESERVED_ENTITIES_COUNT);
}


IScene* Universe::getScene(ComponentType type) const
{
	return m_component_type_map[type.index].scene;
}


IScene* Universe::getScene(u32 hash) const
{
	for (auto* scene : m_scenes)
	{
		if (crc32(scene->getPlugin().getName()) == hash)
		{
			return scene;
		}
	}
	return nullptr;
}


Array<IScene*>& Universe::getScenes()
{
	return m_scenes;
}


void Universe::addScene(IScene* scene)
{
	m_scenes.push(scene);
}


void Universe::removeScene(IScene* scene)
{
	m_scenes.eraseItemFast(scene);
}


const DVec3& Universe::getPosition(EntityRef entity) const
{
	return m_transforms[entity.index].pos;
}


const Quat& Universe::getRotation(EntityRef entity) const
{
	return m_transforms[entity.index].rot;
}


void Universe::transformEntity(EntityRef entity, bool update_local)
{
	const int hierarchy_idx = m_entities[entity.index].hierarchy;
	entityTransformed().invoke(entity);
	if (hierarchy_idx >= 0) {
		Hierarchy& h = m_hierarchy[hierarchy_idx];
		const Transform my_transform = getTransform(entity);
		if (update_local && h.parent.isValid()) {
			const Transform parent_tr = getTransform((EntityRef)h.parent);
			h.local_transform = (parent_tr.inverted() * my_transform);
		}

		EntityPtr child = h.first_child;
		while (child.isValid())
		{
			const Hierarchy& child_h = m_hierarchy[m_entities[child.index].hierarchy];
			const Transform abs_tr = my_transform * child_h.local_transform;
			Transform& child_data = m_transforms[child.index];
			child_data = abs_tr;
			transformEntity((EntityRef)child, false);

			child = child_h.next_sibling;
		}
	}
}


void Universe::setRotation(EntityRef entity, const Quat& rot)
{
	m_transforms[entity.index].rot = rot;
	transformEntity(entity, true);
}


void Universe::setRotation(EntityRef entity, float x, float y, float z, float w)
{
	m_transforms[entity.index].rot.set(x, y, z, w);
	transformEntity(entity, true);
}


bool Universe::hasEntity(EntityRef entity) const
{
	return entity.index >= 0 && entity.index < m_entities.size() && m_entities[entity.index].valid;
}


void Universe::setTransformKeepChildren(EntityRef entity, const Transform& transform)
{
	Transform& tmp = m_transforms[entity.index];
	tmp = transform;
	
	int hierarchy_idx = m_entities[entity.index].hierarchy;
	entityTransformed().invoke(entity);
	if (hierarchy_idx >= 0)
	{
		Hierarchy& h = m_hierarchy[hierarchy_idx];
		Transform my_transform = getTransform(entity);
		if (h.parent.isValid())
		{
			Transform parent_tr = getTransform((EntityRef)h.parent);
			h.local_transform = parent_tr.inverted() * my_transform;
		}

		EntityPtr child = h.first_child;
		while (child.isValid())
		{
			Hierarchy& child_h = m_hierarchy[m_entities[child.index].hierarchy];

			child_h.local_transform = my_transform.inverted() * getTransform((EntityRef)child);
			child = child_h.next_sibling;
		}
	}
}


void Universe::setTransform(EntityRef entity, const Transform& transform)
{
	Transform& tmp = m_transforms[entity.index];
	tmp = transform;
	transformEntity(entity, true);
}


void Universe::setTransform(EntityRef entity, const RigidTransform& transform)
{
	auto& tmp = m_transforms[entity.index];
	tmp.pos = transform.pos;
	tmp.rot = transform.rot;
	transformEntity(entity, true);
}


void Universe::setTransform(EntityRef entity, const DVec3& pos, const Quat& rot, float scale)
{
	auto& tmp = m_transforms[entity.index];
	tmp.pos = pos;
	tmp.rot = rot;
	tmp.scale = scale;
	transformEntity(entity, true);
}


const Transform& Universe::getTransform(EntityRef entity) const
{
	return m_transforms[entity.index];
}


Matrix Universe::getRelativeMatrix(EntityRef entity, const DVec3& base_pos) const
{
	const Transform& transform = m_transforms[entity.index];
	Matrix mtx = transform.rot.toMatrix();
	mtx.setTranslation((transform.pos - base_pos).toFloat());
	mtx.multiply3x3(transform.scale);
	return mtx;
}


void Universe::setPosition(EntityRef entity, double x, double y, double z)
{
	setPosition(entity, DVec3(x, y, z));
}


void Universe::setPosition(EntityRef entity, const DVec3& pos)
{
	m_transforms[entity.index].pos = pos;
	transformEntity(entity, true);
}


void Universe::setEntityName(EntityRef entity, const char* name)
{
	int name_idx = m_entities[entity.index].name;
	if (name_idx < 0)
	{
		if (name[0] == '\0') return;
		m_entities[entity.index].name = m_names.size();
		EntityName& name_data = m_names.emplace();
		name_data.entity = entity;
		copyString(name_data.name, name);
	}
	else
	{
		copyString(m_names[name_idx].name, name);
	}
}


const char* Universe::getEntityName(EntityRef entity) const
{
	int name_idx = m_entities[entity.index].name;
	if (name_idx < 0) return "";
	return m_names[name_idx].name;
}


EntityPtr Universe::findByName(EntityPtr parent, const char* name)
{
	if (parent.isValid()) {
		int h_idx = m_entities[parent.index].hierarchy;
		if (h_idx < 0) return INVALID_ENTITY;

		EntityPtr e = m_hierarchy[h_idx].first_child;
		while (e.isValid()) {
			const EntityData& data = m_entities[e.index];
			int name_idx = data.name;
			if (name_idx >= 0) {
				if (equalStrings(m_names[name_idx].name, name)) return e;
			}
			e = m_hierarchy[data.hierarchy].next_sibling;
		}
	}
	else
	{
		for (int i = 0, c = m_names.size(); i < c; ++i) {
			if (equalStrings(m_names[i].name, name)) {
				const EntityData& data = m_entities[m_names[i].entity.index];
				if (data.hierarchy < 0) return m_names[i].entity;
				if (!m_hierarchy[data.hierarchy].parent.isValid()) return m_names[i].entity;
			}
		}
	}

	return INVALID_ENTITY;
}


void Universe::emplaceEntity(EntityRef entity)
{
	while (m_entities.size() <= entity.index)
	{
		EntityData& data = m_entities.emplace();
		Transform& tr = m_transforms.emplace();
		data.valid = false;
		data.prev = -1;
		data.name = -1;
		data.hierarchy = -1;
		data.next = m_first_free_slot;
		tr.scale = -1;
		if (m_first_free_slot >= 0)
		{
			m_entities[m_first_free_slot].prev = m_entities.size() - 1;
		}
		m_first_free_slot = m_entities.size() - 1;
	}
	if (m_first_free_slot == entity.index)
	{
		m_first_free_slot = m_entities[entity.index].next;
	}
	if (m_entities[entity.index].prev >= 0)
	{
		m_entities[m_entities[entity.index].prev].next = m_entities[entity.index].next;
	}
	if (m_entities[entity.index].next >= 0)
	{
		m_entities[m_entities[entity.index].next].prev= m_entities[entity.index].prev;
	}
	EntityData& data = m_entities[entity.index];
	Transform& tr = m_transforms[entity.index];
	tr.pos = DVec3(0, 0, 0);
	tr.rot.set(0, 0, 0, 1);
	tr.scale = 1;
	data.name = -1;
	data.hierarchy = -1;
	data.components = 0;
	data.valid = true;
	m_entity_created.invoke(entity);
}


EntityRef Universe::cloneEntity(EntityRef entity)
{
	Transform tr = getTransform(entity);
	EntityPtr parent = getParent(entity);
	EntityRef clone = createEntity(tr.pos, tr.rot);
	setScale(clone, tr.scale);
	setParent(parent, clone);

	OutputMemoryStream blob_out(m_allocator);
	blob_out.reserve(1024);
	for (ComponentUID cmp = getFirstComponent(entity); cmp.isValid(); cmp = getNextComponent(cmp))
	{
		blob_out.clear();
		struct : ISaveEntityGUIDMap
		{
			EntityGUID get(EntityPtr entity) override { return { (u64)entity.index }; }
		} save_map;
		TextSerializer serializer(blob_out, save_map);
		serializeComponent(serializer, cmp.type, entity);
		
		InputMemoryStream blob_in(blob_out);
		struct : ILoadEntityGUIDMap
		{
			EntityPtr get(EntityGUID guid) override { return { (int)guid.value }; }
		} load_map;
		TextDeserializer deserializer(blob_in, load_map);
		deserializeComponent(deserializer, clone, cmp.type, cmp.scene->getVersion());
	}
	return clone;
}


EntityRef Universe::createEntity(const DVec3& position, const Quat& rotation)
{
	EntityData* data;
	EntityRef entity;
	Transform* tr;
	if (m_first_free_slot >= 0)
	{
		data = &m_entities[m_first_free_slot];
		tr = &m_transforms[m_first_free_slot];
		entity.index = m_first_free_slot;
		if (data->next >= 0) m_entities[data->next].prev = -1;
		m_first_free_slot = data->next;
	}
	else
	{
		entity.index = m_entities.size();
		data = &m_entities.emplace();
		tr = &m_transforms.emplace();
	}
	tr->pos = position;
	tr->rot = rotation;
	tr->scale = 1;
	data->name = -1;
	data->hierarchy = -1;
	data->components = 0;
	data->valid = true;
	m_entity_created.invoke(entity);

	return entity;
}


void Universe::destroyEntity(EntityRef entity)
{
	EntityData& entity_data = m_entities[entity.index];
	ASSERT(entity_data.valid);
	for (EntityPtr first_child = getFirstChild(entity); first_child.isValid(); first_child = getFirstChild(entity))
	{
		setParent(INVALID_ENTITY, (EntityRef)first_child);
	}
	setParent(INVALID_ENTITY, entity);
	

	u64 mask = entity_data.components;
	for (int i = 0; i < ComponentType::MAX_TYPES_COUNT; ++i)
	{
		if ((mask & ((u64)1 << i)) != 0)
		{
			auto original_mask = mask;
			IScene* scene = m_component_type_map[i].scene;
			auto destroy_method = m_component_type_map[i].destroy;
			(scene->*destroy_method)(entity);
			mask = entity_data.components;
			ASSERT(original_mask != mask);
		}
	}

	entity_data.next = m_first_free_slot;
	entity_data.prev = -1;
	entity_data.hierarchy = -1;
	
	entity_data.valid = false;
	if (m_first_free_slot >= 0)
	{
		m_entities[m_first_free_slot].prev = entity.index;
	}

	if (entity_data.name >= 0)
	{
		m_entities[m_names.back().entity.index].name = entity_data.name;
		m_names.eraseFast(entity_data.name);
		entity_data.name = -1;
	}

	m_first_free_slot = entity.index;
	m_entity_destroyed.invoke(entity);
}


EntityPtr Universe::getFirstEntity() const
{
	for (int i = 0; i < m_entities.size(); ++i)
	{
		if (m_entities[i].valid) return {i};
	}
	return INVALID_ENTITY;
}


EntityPtr Universe::getNextEntity(EntityRef entity) const
{
	for (int i = entity.index + 1; i < m_entities.size(); ++i)
	{
		if (m_entities[i].valid) return {i};
	}
	return INVALID_ENTITY;
}


EntityPtr Universe::getParent(EntityRef entity) const
{
	int idx = m_entities[entity.index].hierarchy;
	if (idx < 0) return INVALID_ENTITY;
	return m_hierarchy[idx].parent;
}


EntityPtr Universe::getFirstChild(EntityRef entity) const
{
	int idx = m_entities[entity.index].hierarchy;
	if (idx < 0) return INVALID_ENTITY;
	return m_hierarchy[idx].first_child;
}


EntityPtr Universe::getNextSibling(EntityRef entity) const
{
	int idx = m_entities[entity.index].hierarchy;
	if (idx < 0) return INVALID_ENTITY;
	return m_hierarchy[idx].next_sibling;
}


bool Universe::isDescendant(EntityRef ancestor, EntityRef descendant) const
{
	for(EntityPtr e = getFirstChild(ancestor); e.isValid(); e = getNextSibling((EntityRef)e))
	{
		if (e == descendant) return true;
		if (isDescendant((EntityRef)e, descendant)) return true;
	}

	return false;
}


void Universe::setParent(EntityPtr new_parent, EntityRef child)
{
	bool would_create_cycle = new_parent.isValid() && isDescendant(child, (EntityRef)new_parent);
	if (would_create_cycle)
	{
		logError("Engine") << "Hierarchy can not contains a cycle.";
		return;
	}

	auto collectGarbage = [this](EntityRef entity) {
		Hierarchy& h = m_hierarchy[m_entities[entity.index].hierarchy];
		if (h.parent.isValid()) return;
		if (h.first_child.isValid()) return;

		const Hierarchy& last = m_hierarchy.back();
		m_entities[last.entity.index].hierarchy = m_entities[entity.index].hierarchy;
		m_entities[entity.index].hierarchy = -1;
		h = last;
		m_hierarchy.pop();
	};

	int child_idx = m_entities[child.index].hierarchy;
	
	if (child_idx >= 0)
	{
		EntityPtr old_parent = m_hierarchy[child_idx].parent;

		if (old_parent.isValid())
		{
			Hierarchy& old_parent_h = m_hierarchy[m_entities[old_parent.index].hierarchy];
			EntityPtr* x = &old_parent_h.first_child;
			while (x->isValid())
			{
				if (*x == child)
				{
					*x = getNextSibling(child);
					break;
				}
				x = &m_hierarchy[m_entities[x->index].hierarchy].next_sibling;
			}
			m_hierarchy[child_idx].parent = INVALID_ENTITY;
			m_hierarchy[child_idx].next_sibling = INVALID_ENTITY;
			collectGarbage((EntityRef)old_parent);
			child_idx = m_entities[child.index].hierarchy;
		}
	}
	else if(new_parent.isValid())
	{
		child_idx = m_hierarchy.size();
		m_entities[child.index].hierarchy = child_idx;
		Hierarchy& h = m_hierarchy.emplace();
		h.entity = child;
		h.parent = INVALID_ENTITY;
		h.first_child = INVALID_ENTITY;
		h.next_sibling = INVALID_ENTITY;
	}

	if (new_parent.isValid())
	{
		int new_parent_idx = m_entities[new_parent.index].hierarchy;
		if (new_parent_idx < 0)
		{
			new_parent_idx = m_hierarchy.size();
			m_entities[new_parent.index].hierarchy = new_parent_idx;
			Hierarchy& h = m_hierarchy.emplace();
			h.entity = (EntityRef)new_parent;
			h.parent = INVALID_ENTITY;
			h.first_child = INVALID_ENTITY;
			h.next_sibling = INVALID_ENTITY;
		}

		m_hierarchy[child_idx].parent = new_parent;
		Transform parent_tr = getTransform((EntityRef)new_parent);
		Transform child_tr = getTransform(child);
		m_hierarchy[child_idx].local_transform = parent_tr.inverted() * child_tr;
		m_hierarchy[child_idx].next_sibling = m_hierarchy[new_parent_idx].first_child;
		m_hierarchy[new_parent_idx].first_child = child;
	}
	else
	{
		if (child_idx >= 0) collectGarbage(child);
	}
}


void Universe::updateGlobalTransform(EntityRef entity)
{
	const Hierarchy& h = m_hierarchy[m_entities[entity.index].hierarchy];
	ASSERT(h.parent.isValid());
	Transform parent_tr = getTransform((EntityRef)h.parent);
	
	Transform new_tr = parent_tr * h.local_transform;
	setTransform(entity, new_tr);
}


void Universe::setLocalPosition(EntityRef entity, const DVec3& pos)
{
	int hierarchy_idx = m_entities[entity.index].hierarchy;
	if (hierarchy_idx < 0)
	{
		setPosition(entity, pos);
		return;
	}

	m_hierarchy[hierarchy_idx].local_transform.pos = pos;
	updateGlobalTransform(entity);
}


void Universe::setLocalRotation(EntityRef entity, const Quat& rot)
{
	int hierarchy_idx = m_entities[entity.index].hierarchy;
	if (hierarchy_idx < 0)
	{
		setRotation(entity, rot);
		return;
	}
	m_hierarchy[hierarchy_idx].local_transform.rot = rot;
	updateGlobalTransform(entity);
}


Transform Universe::computeLocalTransform(EntityRef parent, const Transform& global_transform) const
{
	Transform parent_tr = getTransform(parent);
	return parent_tr.inverted() * global_transform;
}


void Universe::setLocalTransform(EntityRef entity, const Transform& transform)
{
	int hierarchy_idx = m_entities[entity.index].hierarchy;
	if (hierarchy_idx < 0)
	{
		setTransform(entity, transform);
		return;
	}

	Hierarchy& h = m_hierarchy[hierarchy_idx];
	h.local_transform = transform;
	updateGlobalTransform(entity);
}


Transform Universe::getLocalTransform(EntityRef entity) const
{
	int hierarchy_idx = m_entities[entity.index].hierarchy;
	if (hierarchy_idx < 0)
	{
		return getTransform(entity);
	}

	return m_hierarchy[hierarchy_idx].local_transform;
}


float Universe::getLocalScale(EntityRef entity) const
{
	int hierarchy_idx = m_entities[entity.index].hierarchy;
	if (hierarchy_idx < 0)
	{
		return getScale(entity);
	}

	return m_hierarchy[hierarchy_idx].local_transform.scale;
}



void Universe::serializeComponent(ISerializer& serializer, ComponentType type, EntityRef entity)
{
	auto* scene = m_component_type_map[type.index].scene;
	auto& method = m_component_type_map[type.index].serialize;
	(scene->*method)(serializer, entity);
}


void Universe::deserializeComponent(IDeserializer& serializer, EntityRef entity, ComponentType type, int scene_version)
{
	auto* scene = m_component_type_map[type.index].scene;
	auto& method = m_component_type_map[type.index].deserialize;
	(scene->*method)(serializer, entity, scene_version);
}


void Universe::serialize(IOutputStream& serializer)
{
	serializer.write((i32)m_entities.size());
	if (!m_entities.empty()) {
		serializer.write(&m_entities[0], m_entities.byte_size());
		serializer.write(&m_transforms[0], m_transforms.byte_size());
	}
	serializer.write((i32)m_names.size());
	for (const EntityName& name : m_names) {
		serializer.write(name.entity);
		serializer.writeString(name.name);
	}
	serializer.write(m_first_free_slot);

	serializer.write(m_hierarchy.size());
	if(!m_hierarchy.empty()) serializer.write(&m_hierarchy[0], sizeof(m_hierarchy[0]) * m_hierarchy.size());
}


void Universe::deserialize(IInputStream& serializer)
{
	i32 count;
	serializer.read(count);
	m_entities.resize(count);
	m_transforms.resize(count);

	if (count > 0) {
		serializer.read(&m_entities[0], m_entities.byte_size());
		serializer.read(&m_transforms[0], m_transforms.byte_size());
	}

	serializer.read(count);
	for (int i = 0; i < count; ++i)
	{
		EntityName& name = m_names.emplace();
		serializer.read(name.entity);
		serializer.readString(name.name, lengthOf(name.name));
		m_entities[name.entity.index].name = m_names.size() - 1;
	}

	serializer.read(m_first_free_slot);

	serializer.read(count);
	m_hierarchy.resize(count);
	if (count > 0) serializer.read(&m_hierarchy[0], sizeof(m_hierarchy[0]) * m_hierarchy.size());
}


struct PrefabEntityGUIDMap : public ILoadEntityGUIDMap
{
	explicit PrefabEntityGUIDMap(const Array<EntityRef>& _entities)
		: entities(_entities)
	{
	}


	EntityPtr get(EntityGUID guid) override
	{
		if (guid.value >= entities.size()) return INVALID_ENTITY;
		return entities[(int)guid.value];
	}


	const Array<EntityRef>& entities;
};


EntityPtr Universe::instantiatePrefab(const PrefabResource& prefab,
	const DVec3& pos,
	const Quat& rot,
	float scale)
{
	InputMemoryStream blob(prefab.data.begin(), prefab.data.byte_size());
	Array<EntityRef> entities(m_allocator);
	PrefabEntityGUIDMap entity_map(entities);
	TextDeserializer deserializer(blob, entity_map);
	u32 version;
	deserializer.read(&version);
	if (version > (int)PrefabVersion::LAST)
	{
		logError("Engine") << "Prefab " << prefab.getPath() << " has unsupported version.";
		return INVALID_ENTITY;
	}
	int count;
	deserializer.read(&count);
	int entity_idx = 0;
	entities.reserve(count);
	for (int i = 0; i < count; ++i)
	{
		entities.push(createEntity({0, 0, 0}, {0, 0, 0, 1}));
	}
	while (blob.getPosition() < blob.size() && entity_idx < count)
	{
		u64 prefab;
		deserializer.read(&prefab);
		EntityRef entity = entities[entity_idx];
		setTransform(entity, {pos, rot, scale});
		if (version > (int)PrefabVersion::WITH_HIERARCHY)
		{
			EntityPtr parent;

			deserializer.read(&parent);
			if (parent.isValid())
			{
				RigidTransform local_tr;
				deserializer.read(&local_tr);
				float scale;
				deserializer.read(&scale);
				setParent(parent, entity);
				setLocalTransform(entity, {local_tr.pos, local_tr.rot, scale});
			}
		}
		u32 cmp_type_hash;
		deserializer.read(&cmp_type_hash);
		while (cmp_type_hash != 0)
		{
			ComponentType cmp_type = Reflection::getComponentTypeFromHash(cmp_type_hash);
			int scene_version;
			deserializer.read(&scene_version);
			deserializeComponent(deserializer, entity, cmp_type, scene_version);
			deserializer.read(&cmp_type_hash);
		}
		++entity_idx;
	}
	return entities[0];
}


void Universe::setScale(EntityRef entity, float scale)
{
	m_transforms[entity.index].scale = scale;
	transformEntity(entity, true);
}


float Universe::getScale(EntityRef entity) const
{
	return m_transforms[entity.index].scale;
}


ComponentUID Universe::getFirstComponent(EntityRef entity) const
{
	u64 mask = m_entities[entity.index].components;
	for (int i = 0; i < ComponentType::MAX_TYPES_COUNT; ++i)
	{
		if ((mask & (u64(1) << i)) != 0)
		{
			IScene* scene = m_component_type_map[i].scene;
			return ComponentUID(entity, {i}, scene);
		}
	}
	return ComponentUID::INVALID;
}


ComponentUID Universe::getNextComponent(const ComponentUID& cmp) const
{
	u64 mask = m_entities[cmp.entity.index].components;
	for (int i = cmp.type.index + 1; i < ComponentType::MAX_TYPES_COUNT; ++i)
	{
		if ((mask & (u64(1) << i)) != 0)
		{
			IScene* scene = m_component_type_map[i].scene;
			return ComponentUID(cmp.entity, {i}, scene);
		}
	}
	return ComponentUID::INVALID;
}


ComponentUID Universe::getComponent(EntityRef entity, ComponentType component_type) const
{
	u64 mask = m_entities[entity.index].components;
	if ((mask & (u64(1) << component_type.index)) == 0) return ComponentUID::INVALID;
	IScene* scene = m_component_type_map[component_type.index].scene;
	return ComponentUID(entity, component_type, scene);
}


u64 Universe::getComponentsMask(EntityRef entity) const
{
	return m_entities[entity.index].components;
}


bool Universe::hasComponent(EntityRef entity, ComponentType component_type) const
{
	u64 mask = m_entities[entity.index].components;
	return (mask & (u64(1) << component_type.index)) != 0;
}


void Universe::onComponentDestroyed(EntityRef entity, ComponentType component_type, IScene* scene)
{
	auto mask = m_entities[entity.index].components;
	auto old_mask = mask;
	mask &= ~((u64)1 << component_type.index);
	ASSERT(old_mask != mask);
	m_entities[entity.index].components = mask;
	m_component_destroyed.invoke(ComponentUID(entity, component_type, scene));
}


void Universe::createComponent(ComponentType type, EntityRef entity)
{
	IScene* scene = m_component_type_map[type.index].scene;
	auto& create_method = m_component_type_map[type.index].create;
	(scene->*create_method)(entity);
}


void Universe::destroyComponent(EntityRef entity, ComponentType type)
{
	IScene* scene = m_component_type_map[type.index].scene;
	auto& destroy_method = m_component_type_map[type.index].destroy;
	(scene->*destroy_method)(entity);
}


void Universe::onComponentCreated(EntityRef entity, ComponentType component_type, IScene* scene)
{
	ComponentUID cmp(entity, component_type, scene);
	m_entities[entity.index].components |= (u64)1 << component_type.index;
	m_component_added.invoke(cmp);
}


} // namespace Lumix
