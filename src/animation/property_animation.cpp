#include "animation/property_animation.h"
#include "engine/crc32.h"
#include "engine/allocator.h"
#include "engine/log.h"
#include "engine/reflection.h"
#include "engine/serializer.h"


namespace Lumix
{


const ResourceType PropertyAnimation::TYPE("property_animation");


PropertyAnimation::PropertyAnimation(const Path& path, ResourceManager& resource_manager, IAllocator& allocator)
	: Resource(path, resource_manager, allocator)
	, m_allocator(allocator)
	, fps(30)
	, curves(allocator)
{
}


PropertyAnimation::Curve& PropertyAnimation::addCurve()
{
	return curves.emplace(m_allocator);
}


bool PropertyAnimation::save(TextSerializer& serializer)
{
	if (!isReady()) return false;

	serializer.write("count", curves.size());
	for (Curve& curve : curves)
	{
		serializer.write("component", Reflection::getComponent(curve.cmp_type)->name);
		serializer.write("property", curve.property->name);
		serializer.write("keys_count", curve.frames.size());
		for (int i = 0; i < curve.frames.size(); ++i)
		{
			serializer.write("frame", curve.frames[i]);
			serializer.write("value", curve.values[i]);
		}
	}

	return true;
}


bool PropertyAnimation::load(u64 size, const u8* mem)
{
	InputMemoryStream file(mem, size);
	struct : ILoadEntityGUIDMap { 
		EntityPtr get(EntityGUID guid) override { ASSERT(false); return INVALID_ENTITY; }
	} dummy_map;
	TextDeserializer serializer(file, dummy_map);
	
	int count;
	serializer.read(&count);
	for (int i = 0; i < count; ++i) {
		Curve& curve = curves.emplace(m_allocator);
		char tmp[32];
		serializer.read(tmp, lengthOf(tmp));
		curve.cmp_type = Reflection::getComponentType(tmp);
		serializer.read(tmp, lengthOf(tmp));
		u32 prop_hash = crc32(tmp);
		
		int keys_count;
		serializer.read(&keys_count);
		curve.frames.resize(keys_count);
		curve.values.resize(keys_count);
		for (int j = 0; j < keys_count; ++j) {
			serializer.read(&curve.frames[i]);
			serializer.read(&curve.values[i]);
		}
	}
	return true;
}


void PropertyAnimation::unload()
{
	curves.clear();
}


} // namespace Lumix
