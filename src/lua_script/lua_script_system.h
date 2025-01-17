#pragma once


#include "engine/iplugin.h"
#include "engine/path.h"
#include "engine/resource.h"
#include "engine/string.h"


struct lua_State;


namespace Lumix
{


class LuaScript;


class LuaScriptScene : public IScene
{
public:
	struct Property
	{
		enum Type : int
		{
			BOOLEAN,
			FLOAT,
			INT,
			ENTITY,
			RESOURCE,
			STRING,
			ANY
		};

		explicit Property(IAllocator& allocator)
			: stored_value(allocator)
		{
		}

		u32 name_hash;
		Type type;
		ResourceType resource_type;
		String stored_value;
	};


	struct IFunctionCall
	{
		virtual ~IFunctionCall() {}
		virtual void add(int parameter) = 0;
		virtual void add(bool parameter) = 0;
		virtual void add(float parameter) = 0;
		virtual void add(void* parameter) = 0;
		virtual void addEnvironment(int env) = 0;
	};


	typedef int (*lua_CFunction) (lua_State *L);

public:
	virtual Path getScriptPath(EntityRef entity, int scr_index) = 0;	
	virtual void setScriptPath(EntityRef entity, int scr_index, const Path& path) = 0;
	virtual int getEnvironment(EntityRef entity, int scr_index) = 0;
	virtual IFunctionCall* beginFunctionCall(EntityRef entity, int scr_index, const char* function) = 0;
	virtual void endFunctionCall() = 0;
	virtual int getScriptCount(EntityRef entity) = 0;
	virtual lua_State* getState(EntityRef entity, int scr_index) = 0;
	virtual void insertScript(EntityRef entity, int idx) = 0;
	virtual int addScript(EntityRef entity) = 0;
	virtual void removeScript(EntityRef entity, int scr_index) = 0;
	virtual void enableScript(EntityRef entity, int scr_index, bool enable) = 0;
	virtual bool isScriptEnabled(EntityRef entity, int scr_index) const = 0;
	virtual void moveScript(EntityRef entity, int scr_index, bool up) = 0;
	virtual void serializeScript(EntityRef entity, int scr_index, OutputMemoryStream& blob) = 0;
	virtual void deserializeScript(EntityRef entity, int scr_index, InputMemoryStream& blob) = 0;
	virtual void setPropertyValue(EntityRef entity, int scr_index, const char* name, const char* value) = 0;
	virtual void getPropertyValue(EntityRef entity, int scr_index, const char* property_name, char* out, int max_size) = 0;
	virtual int getPropertyCount(EntityRef entity, int scr_index) = 0;
	virtual const char* getPropertyName(EntityRef entity, int scr_index, int prop_index) = 0;
	virtual Property::Type getPropertyType(EntityRef entity, int scr_index, int prop_index) = 0;
	virtual ResourceType getPropertyResourceType(EntityRef entity, int scr_index, int prop_index) = 0;
	virtual void getScriptData(EntityRef entity, OutputMemoryStream& blob) = 0;
	virtual void setScriptData(EntityRef entity, InputMemoryStream& blob) = 0;
};


} // namespace Lumix