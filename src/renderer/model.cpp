#include "engine/lumix.h"
#include "renderer/model.h"

#include "engine/array.h"
#include "engine/crc32.h"
#include "engine/file_system.h"
#include "engine/log.h"
#include "engine/lua_wrapper.h"
#include "engine/path_utils.h"
#include "engine/profiler.h"
#include "engine/resource_manager.h"
#include "engine/stream.h"
#include "engine/math.h"
#include "renderer/material.h"
#include "renderer/pose.h"
#include "renderer/renderer.h"

#include <cfloat>
#include <cmath>


namespace Lumix
{


u32 Mesh::s_last_sort_key = 0;


static LocalRigidTransform invert(const LocalRigidTransform& tr)
{
	LocalRigidTransform result;
	result.rot = tr.rot.conjugated();
	result.pos = result.rot.rotate(-tr.pos);
	return result;
}


Mesh::Mesh(Material* mat,
	const ffr::VertexDecl& vertex_decl,
	const char* name,
	const AttributeSemantic* semantics,
	Renderer& renderer,
	IAllocator& allocator)
	: name(name, allocator)
	, material(mat)
	, indices(allocator)
	, vertices(allocator)
	, uvs(allocator)
	, skin(allocator)
{
	render_data = LUMIX_NEW(renderer.getAllocator(), RenderData);
	render_data->vertex_decl = vertex_decl;
	render_data->vertex_buffer_handle = ffr::INVALID_BUFFER;
	render_data->vao = ffr::INVALID_VAO;
	render_data->index_buffer_handle = ffr::INVALID_BUFFER;
	render_data->index_type = ffr::DataType::U32;
	for(AttributeSemantic& attr : render_data->attributes_semantic) {
		attr = AttributeSemantic::NONE;
	}
	if(semantics) {
		for(uint i = 0; i < vertex_decl.attributes_count; ++i) {
			render_data->attributes_semantic[i] = semantics[i];
		}
	}

	sort_key = s_last_sort_key;
	++s_last_sort_key;
}

// TODO remove?
/*
void Mesh::set(const Mesh& rhs)
{
	type = rhs.type;
	indices = rhs.indices;
	vertices = rhs.vertices;
	uvs = rhs.uvs;
	skin = rhs.skin;
	flags = rhs.flags;
	layer_mask = rhs.layer_mask;
	indices_count = rhs.indices_count;
	vertex_decl = rhs.vertex_decl;
	vertex_buffer_handle = rhs.vertex_buffer_handle;
	index_buffer_handle = rhs.index_buffer_handle;
	name = rhs.name;
	copyMemory(attributes_semantic, rhs.attributes_semantic, sizeof(attributes_semantic));
	// all except material
}*/


static bool hasAttribute(Mesh& mesh, Mesh::AttributeSemantic attribute)
{
	for(const Mesh::AttributeSemantic& attr : mesh.render_data->attributes_semantic) {
		if(attr == attribute) return true;
	}
	return false;
}


void Mesh::setMaterial(Material* new_material, Model& model, Renderer& renderer)
{
	if (material) material->getResourceManager().unload(*material);
	material = new_material;
	type = model.getBoneCount() == 0 || skin.empty() ? Mesh::RIGID_INSTANCED : Mesh::SKINNED;
}


const ResourceType Model::TYPE("model");


Model::Model(const Path& path, ResourceManager& resource_manager, Renderer& renderer, IAllocator& allocator)
	: Resource(path, resource_manager, allocator)
	, m_bounding_radius()
	, m_allocator(allocator)
	, m_bone_map(m_allocator)
	, m_meshes(m_allocator)
	, m_bones(m_allocator)
	, m_first_nonroot_bone_index(0)
	, m_renderer(renderer)
{
	m_lods[0] = { 0, -1, FLT_MAX };
	m_lods[1] = { 0, -1, FLT_MAX };
	m_lods[2] = { 0, -1, FLT_MAX };
	m_lods[3] = { 0, -1, FLT_MAX };
}


Model::~Model()
{
	ASSERT(isEmpty());
}


static Vec3 evaluateSkin(Vec3& p, Mesh::Skin s, const Matrix* matrices)
{
	Matrix m = matrices[s.indices[0]] * s.weights.x + matrices[s.indices[1]] * s.weights.y +
			   matrices[s.indices[2]] * s.weights.z + matrices[s.indices[3]] * s.weights.w;

	return m.transformPoint(p);
}


static void computeSkinMatrices(const Pose& pose, const Model& model, Matrix* matrices)
{
	for (int i = 0; i < pose.count; ++i)
	{
		auto& bone = model.getBone(i);
		LocalRigidTransform tmp = { pose.positions[i], pose.rotations[i] };
		matrices[i] = (tmp * bone.inv_bind_transform).toMatrix();
	}
}


bool Model::isSkinned() const
{
	ASSERT(isReady());
	for (const Mesh& m : m_meshes) {
		if(m.type == Mesh::SKINNED) return true;
	}
	return false;
}


RayCastModelHit Model::castRay(const Vec3& origin, const Vec3& dir, const Pose* pose)
{
	RayCastModelHit hit;
	hit.is_hit = false;
	if (!isReady()) return hit;

	Matrix matrices[256];
	ASSERT(!pose || pose->count <= lengthOf(matrices));
	bool is_skinned = false;
	for (int mesh_index = m_lods[0].from_mesh; mesh_index <= m_lods[0].to_mesh; ++mesh_index)
	{
		Mesh& mesh = m_meshes[mesh_index];
		is_skinned = pose && !mesh.skin.empty() && pose->count <= lengthOf(matrices);
	}
	if (is_skinned)
	{
		computeSkinMatrices(*pose, *this, matrices);
	}

	for (int mesh_index = m_lods[0].from_mesh; mesh_index <= m_lods[0].to_mesh; ++mesh_index)
	{
		Mesh& mesh = m_meshes[mesh_index];
		bool is_mesh_skinned = !mesh.skin.empty();
		u16* indices16 = (u16*)&mesh.indices[0];
		u32* indices32 = (u32*)&mesh.indices[0];
		bool is16 = mesh.flags.isSet(Mesh::Flags::INDICES_16_BIT);
		int index_size = is16 ? 2 : 4;
		for(int i = 0, c = mesh.indices.size() / index_size; i < c; i += 3)
		{
			Vec3 p0, p1, p2;
			if (is16)
			{
				p0 = mesh.vertices[indices16[i]];
				p1 = mesh.vertices[indices16[i + 1]];
				p2 = mesh.vertices[indices16[i + 2]];
				if (is_mesh_skinned)
				{
					p0 = evaluateSkin(p0, mesh.skin[indices16[i]], matrices);
					p1 = evaluateSkin(p1, mesh.skin[indices16[i + 1]], matrices);
					p2 = evaluateSkin(p2, mesh.skin[indices16[i + 2]], matrices);
				}
			}
			else
			{
				p0 = mesh.vertices[indices32[i]];
				p1 = mesh.vertices[indices32[i + 1]];
				p2 = mesh.vertices[indices32[i + 2]];
				if (is_mesh_skinned)
				{
					p0 = evaluateSkin(p0, mesh.skin[indices32[i]], matrices);
					p1 = evaluateSkin(p1, mesh.skin[indices32[i + 1]], matrices);
					p2 = evaluateSkin(p2, mesh.skin[indices32[i + 2]], matrices);
				}
			}


			Vec3 normal = crossProduct(p1 - p0, p2 - p0);
			float q = dotProduct(normal, dir);
			if (q == 0)	continue;

			float d = -dotProduct(normal, p0);
			float t = -(dotProduct(normal, origin) + d) / q;
			if (t < 0) continue;

			Vec3 hit_point = origin + dir * t;

			Vec3 edge0 = p1 - p0;
			Vec3 VP0 = hit_point - p0;
			if (dotProduct(normal, crossProduct(edge0, VP0)) < 0) continue;

			Vec3 edge1 = p2 - p1;
			Vec3 VP1 = hit_point - p1;
			if (dotProduct(normal, crossProduct(edge1, VP1)) < 0) continue;

			Vec3 edge2 = p0 - p2;
			Vec3 VP2 = hit_point - p2;
			if (dotProduct(normal, crossProduct(edge2, VP2)) < 0) continue;

			if (!hit.is_hit || hit.t > t)
			{
				hit.is_hit = true;
				hit.t = t;
				hit.mesh = &m_meshes[mesh_index];
			}
		}
	}
	hit.origin = DVec3(origin.x, origin.y, origin.z);
	hit.dir = dir;
	return hit;
}


void Model::getRelativePose(Pose& pose)
{
	ASSERT(pose.count == getBoneCount());
	Vec3* pos = pose.positions;
	Quat* rot = pose.rotations;
	for (int i = 0, c = getBoneCount(); i < c; ++i)
	{
		pos[i] = m_bones[i].relative_transform.pos;
		rot[i] = m_bones[i].relative_transform.rot;
	}
	pose.is_absolute = false;
}


void Model::getPose(Pose& pose)
{
	ASSERT(pose.count == getBoneCount());
	Vec3* pos = pose.positions;
	Quat* rot = pose.rotations;
	for (int i = 0, c = getBoneCount(); i < c; ++i)
	{
		pos[i] = m_bones[i].transform.pos;
		rot[i] = m_bones[i].transform.rot;
	}
	pose.is_absolute = true;
}


static bool parseVertexDecl(IInputStream& file, ffr::VertexDecl* vertex_decl, Mesh::AttributeSemantic* semantics)
{
	u32 attribute_count;
	file.read(&attribute_count, sizeof(attribute_count));
	vertex_decl->attributes_count = 0;

	for (u32 i = 0; i < attribute_count; ++i)
	{
		i32 attr;
		file.read(&attr, sizeof(attr));
		semantics[i] = (Mesh::AttributeSemantic)attr;

		switch(semantics[i]) {
			case Mesh::AttributeSemantic::POSITION:
				vertex_decl->addAttribute(3, ffr::AttributeType::FLOAT, false, false);
				break;
			case Mesh::AttributeSemantic::COLOR0:
				vertex_decl->addAttribute(4, ffr::AttributeType::U8, true, false);
				break;
			case Mesh::AttributeSemantic::TEXCOORD0:
				vertex_decl->addAttribute(2, ffr::AttributeType::FLOAT, false, false);
				break;
			case Mesh::AttributeSemantic::NORMAL:
			case Mesh::AttributeSemantic::TANGENT:
				vertex_decl->addAttribute(4, ffr::AttributeType::U8, true, true);
				break;
			case Mesh::AttributeSemantic::WEIGHTS:
				vertex_decl->addAttribute(4, ffr::AttributeType::FLOAT, false, false);
				break;
			case Mesh::AttributeSemantic::INDICES:
				vertex_decl->addAttribute(4, ffr::AttributeType::I16, false, true);
				break;
			default: ASSERT(false); break;
		}
	}

	return true;
}


void Model::onBeforeReady()
{
	for (Mesh& mesh : m_meshes) {
		mesh.type = getBoneCount() == 0 || mesh.skin.empty() ? Mesh::RIGID_INSTANCED : Mesh::SKINNED;
		mesh.layer = mesh.material->getLayer();
	}
}


bool Model::parseBones(InputMemoryStream& file)
{
	int bone_count;
	file.read(&bone_count, sizeof(bone_count));
	if (bone_count < 0) return false;
	if (bone_count > Bone::MAX_COUNT)
	{
		logWarning("Renderer") << "Model " << getPath().c_str() << " has too many bones.";
		return false;
	}

	m_bones.reserve(bone_count);
	for (int i = 0; i < bone_count; ++i)
	{
		Model::Bone& b = m_bones.emplace(m_allocator);
		int len;
		file.read(&len, sizeof(len));
		char tmp[MAX_PATH_LENGTH];
		if (len >= MAX_PATH_LENGTH)
		{
			return false;
		}
		file.read(tmp, len);
		tmp[len] = 0;
		b.name = tmp;
		m_bone_map.insert(crc32(b.name.c_str()), m_bones.size() - 1);
		file.read(&len, sizeof(len));
		if (len >= MAX_PATH_LENGTH)
		{
			return false;
		}
		if(len > 0)
		{
			file.read(tmp, len);
			tmp[len] = 0;
			b.parent = tmp;
		}
		else
		{
			b.parent = "";
		}
		file.read(&b.transform.pos.x, sizeof(float) * 3);
		file.read(&b.transform.rot.x, sizeof(float) * 4);
	}
	m_first_nonroot_bone_index = -1;
	for (int i = 0; i < bone_count; ++i)
	{
		Model::Bone& b = m_bones[i];
		if (b.parent.length() == 0)
		{
			if (m_first_nonroot_bone_index != -1)
			{
				logError("Renderer") << "Invalid skeleton in " << getPath().c_str();
				return false;
			}
			b.parent_idx = -1;
		}
		else
		{
			b.parent_idx = getBoneIdx(b.parent.c_str());
			if (b.parent_idx > i || b.parent_idx < 0)
			{
				logError("Renderer") << "Invalid skeleton in " << getPath().c_str();
				return false;
			}
			if (m_first_nonroot_bone_index == -1)
			{
				m_first_nonroot_bone_index = i;
			}
		}
	}

	for (int i = 0; i < m_bones.size(); ++i)
	{
			m_bones[i].inv_bind_transform = invert(m_bones[i].transform);
	}
	
	for (int i = 0; i < m_bones.size(); ++i)
	{
		int p = m_bones[i].parent_idx;
		if (p >= 0)
		{
			m_bones[i].relative_transform = m_bones[p].inv_bind_transform * m_bones[i].transform;
		}
		else
		{
			m_bones[i].relative_transform = m_bones[i].transform;
		}
	}
	return true;
}

int Model::getBoneIdx(const char* name)
{
	for (int i = 0, c = m_bones.size(); i < c; ++i)
	{
		if (m_bones[i].name == name)
		{
			return i;
		}
	}
	return -1;
}


static int getAttributeOffset(Mesh& mesh, Mesh::AttributeSemantic attr)
{
	for (int i = 0; i < lengthOf(mesh.render_data->attributes_semantic); ++i) {
		if(mesh.render_data->attributes_semantic[i] == attr) {
			return mesh.render_data->vertex_decl.attributes[i].offset;
		}
	}
	return -1;
}


bool Model::parseMeshes(InputMemoryStream& file, FileVersion version)
{
	int object_count = 0;
	file.read(&object_count, sizeof(object_count));
	if (object_count <= 0) return false;

	char model_dir[MAX_PATH_LENGTH];
	PathUtils::getDir(model_dir, MAX_PATH_LENGTH, getPath().c_str());

	m_meshes.reserve(object_count);
	for (int i = 0; i < object_count; ++i)
	{
		ffr::VertexDecl vertex_decl;
		Mesh::AttributeSemantic semantics[ffr::VertexDecl::MAX_ATTRIBUTES];
		for(auto& i : semantics) i = Mesh::AttributeSemantic::NONE;
		if (!parseVertexDecl(file, &vertex_decl, semantics)) return false;

		i32 mat_path_length;
		char mat_path[MAX_PATH_LENGTH + 128];
		file.read(&mat_path_length, sizeof(mat_path_length));
		if (mat_path_length + 1 > lengthOf(mat_path)) return false;
		file.read(mat_path, mat_path_length);
		mat_path[mat_path_length] = '\0';
		
		Material* material = m_resource_manager.getOwner().load<Material>(Path(mat_path));
	
		i32 str_size;
		file.read(&str_size, sizeof(str_size));
		char mesh_name[MAX_PATH_LENGTH];
		mesh_name[str_size] = 0;
		file.read(mesh_name, str_size);

		m_meshes.emplace(material, vertex_decl, mesh_name, semantics, m_renderer, m_allocator);
		addDependency(*material);
	}

	for (int i = 0; i < object_count; ++i)
	{
		Mesh& mesh = m_meshes[i];
		int index_size;
		int indices_count;
		file.read(&index_size, sizeof(index_size));
		if (index_size != 2 && index_size != 4) return false;
		file.read(&indices_count, sizeof(indices_count));
		if (indices_count <= 0) return false;
		mesh.indices.resize(index_size * indices_count);
		mesh.render_data->indices_count = indices_count;
		file.read(&mesh.indices[0], mesh.indices.size());

		if (index_size == 2) mesh.flags.set(Mesh::Flags::INDICES_16_BIT);
		// TODO do not copy, allocate in advance
		const Renderer::MemRef mem = m_renderer.copy(&mesh.indices[0], mesh.indices.size());
		mesh.render_data->index_buffer_handle = m_renderer.createBuffer(mem);
		mesh.render_data->index_type = index_size == 2 ? ffr::DataType::U16 : ffr::DataType::U32;
	}

	for (int i = 0; i < object_count; ++i)
	{
		Mesh& mesh = m_meshes[i];
		int data_size;
		file.read(&data_size, sizeof(data_size));
		Array<u8> vertices_mem(m_allocator);
		vertices_mem.resize(data_size);
		file.read(&vertices_mem[0], data_size);

		int position_attribute_offset = getAttributeOffset(mesh, Mesh::AttributeSemantic::POSITION);
		int uv_attribute_offset = getAttributeOffset(mesh, Mesh::AttributeSemantic::TEXCOORD0);
		int weights_attribute_offset = getAttributeOffset(mesh, Mesh::AttributeSemantic::WEIGHTS);
		int bone_indices_attribute_offset = getAttributeOffset(mesh, Mesh::AttributeSemantic::INDICES);
		bool keep_skin = hasAttribute(mesh, Mesh::AttributeSemantic::WEIGHTS) && hasAttribute(mesh, Mesh::AttributeSemantic::INDICES);

		int vertex_size = mesh.render_data->vertex_decl.size;
		int mesh_vertex_count = vertices_mem.size() / mesh.render_data->vertex_decl.size;
		mesh.vertices.resize(mesh_vertex_count);
		mesh.uvs.resize(mesh_vertex_count);
		if (keep_skin) mesh.skin.resize(mesh_vertex_count);
		const u8* vertices = &vertices_mem[0];
		for (int j = 0; j < mesh_vertex_count; ++j)
		{
			int offset = j * vertex_size;
			if (keep_skin)
			{
				mesh.skin[j].weights = *(const Vec4*)&vertices[offset + weights_attribute_offset];
				copyMemory(mesh.skin[j].indices,
					&vertices[offset + bone_indices_attribute_offset],
					sizeof(mesh.skin[j].indices));
			}
			mesh.vertices[j] = *(const Vec3*)&vertices[offset + position_attribute_offset];
			mesh.uvs[j] = *(const Vec2*)&vertices[offset + uv_attribute_offset];
		}
		// TODO do not copy, allocate in advance
		const Renderer::MemRef mem = m_renderer.copy(&vertices_mem[0], vertices_mem.size());
		mesh.render_data->vertex_buffer_handle = m_renderer.createBuffer(mem);
		ffr::VertexAttrib attribs[16] = {};
		ffr::VertexAttrib* attr = attribs;
		if (position_attribute_offset >= 0)	{
			*attr = { 0, 3, ffr::AttributeType::FLOAT, (u32)position_attribute_offset, false, false, false };
			++attr;
		}
		if (uv_attribute_offset >= 0)	{
			*attr = { 1, 2, ffr::AttributeType::FLOAT, (u32)uv_attribute_offset, false, false, false };
			++attr;
		}
		const int normal_attribute_offset = getAttributeOffset(mesh, Mesh::AttributeSemantic::NORMAL);
		if (normal_attribute_offset >= 0)	{
			*attr = { 2, 4, ffr::AttributeType::U8, (u32)normal_attribute_offset, true, true, false };
			++attr;
		}
		const int tangent_attribute_offset = getAttributeOffset(mesh, Mesh::AttributeSemantic::TANGENT);
		if (tangent_attribute_offset >= 0)	{
			*attr = { 3, 4, ffr::AttributeType::U8, (u32)tangent_attribute_offset, true, true, false };
			++attr;
		}

		if(weights_attribute_offset < 0) {
			*attr = { 4, 4, ffr::AttributeType::FLOAT, 0, false, false, true };
			++attr;
			*attr = { 5, 4, ffr::AttributeType::FLOAT, 16, false, false, true };
			++attr;
			// TODO this is here because of grass, find a better solution
			*attr = { 6, 4, ffr::AttributeType::FLOAT, 32, false, false, true };
			++attr;
		}
		else {
			*attr = { 4, 4, ffr::AttributeType::I16, (u32)bone_indices_attribute_offset, false, true, false };
			++attr;
			*attr = { 5, 4, ffr::AttributeType::FLOAT, (u32)weights_attribute_offset, false, false, false };
			++attr;
		}
		mesh.render_data->vao = m_renderer.createVAO(attribs, u32(attr - attribs));
	}
	file.read(&m_bounding_radius, sizeof(m_bounding_radius));
	file.read(&m_aabb, sizeof(m_aabb));

	return true;
}


bool Model::parseLODs(InputMemoryStream& file)
{
	i32 lod_count;
	file.read(&lod_count, sizeof(lod_count));
	if (lod_count <= 0 || lod_count > lengthOf(m_lods))
	{
		return false;
	}
	for (int i = 0; i < lod_count; ++i)
	{
		file.read(&m_lods[i].to_mesh, sizeof(m_lods[i].to_mesh));
		file.read(&m_lods[i].distance, sizeof(m_lods[i].distance));
		m_lods[i].from_mesh = i > 0 ? m_lods[i - 1].to_mesh + 1 : 0;
	}
	return true;
}


bool Model::load(u64 size, const u8* mem)
{
	PROFILE_FUNCTION();
	FileHeader header;
	InputMemoryStream file(mem, size);
	file.read(&header, sizeof(header));

	if (header.magic != FILE_MAGIC)
	{
		logWarning("Renderer") << "Corrupted model " << getPath().c_str();
		return false;
	}

	if(header.version > (u32)FileVersion::LATEST)
	{
		logWarning("Renderer") << "Unsupported version of model " << getPath().c_str();
		return false;
	}

	if (parseMeshes(file, (FileVersion)header.version)
		&& parseBones(file)
		&& parseLODs(file))
	{
		m_size = file.size();
		return true;
	}

	logError("Renderer") << "Error loading model " << getPath().c_str();
	return false;
}


static Vec3 getBonePosition(Model* model, int bone_index)
{
	return model->getBone(bone_index).transform.pos;
}


static int getBoneParent(Model* model, int bone_index)
{
	return model->getBone(bone_index).parent_idx;
}


void Model::registerLuaAPI(lua_State* L)
{
	#define REGISTER_FUNCTION(F)\
		do { \
			auto f = &LuaWrapper::wrapMethod<Model, decltype(&Model::F), &Model::F>; \
			LuaWrapper::createSystemFunction(L, "Model", #F, f); \
		} while(false) \

	REGISTER_FUNCTION(getBoneCount);

	#undef REGISTER_FUNCTION
	

	#define REGISTER_FUNCTION(F)\
		do { \
			auto f = &LuaWrapper::wrap<decltype(&F), &F>; \
			LuaWrapper::createSystemFunction(L, "Model", #F, f); \
		} while(false) \

	REGISTER_FUNCTION(getBonePosition);
	REGISTER_FUNCTION(getBoneParent);

	#undef REGISTER_FUNCTION
}


void Model::unload()
{
	auto* material_manager = m_resource_manager.getOwner().get(Material::TYPE);
	for (int i = 0; i < m_meshes.size(); ++i) {
		removeDependency(*m_meshes[i].material);
		material_manager->unload(*m_meshes[i].material);
	}

	for (Mesh& mesh : m_meshes) {
		m_renderer.destroy(mesh.render_data->vao);
		m_renderer.runInRenderThread(mesh.render_data, [](Renderer& renderer, void* ptr){
			Mesh::RenderData* rd = (Mesh::RenderData*)ptr;
			if (rd->index_buffer_handle.isValid()) ffr::destroy(rd->index_buffer_handle);
			if (rd->vertex_buffer_handle.isValid()) ffr::destroy(rd->vertex_buffer_handle);
			LUMIX_DELETE(renderer.getAllocator(), rd); 
		});
	}
	m_meshes.clear();
	m_bones.clear();
}


} // namespace Lumix
