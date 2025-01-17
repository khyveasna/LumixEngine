#pragma once


#include "engine/allocator.h"
#include "engine/lumix.h"
#include "engine/metaprogramming.h"
#include "engine/math.h"
#include "engine/string.h"


namespace Lumix
{


template<class Key> 
struct HashFunc
{
	static u32 get(const Key& key);
};

// https://gist.github.com/badboy/6267743
template<>
struct HashFunc<u64>
{
	static u32 get(const u64& key)
	{
		u64 tmp = (~key) + (key << 18);
		tmp = tmp ^ (tmp >> 31);
		tmp = tmp * 21;
		tmp = tmp ^ (tmp >> 11);
		tmp = tmp + (tmp << 6);
		tmp = tmp ^ (tmp >> 22);
		return (u32)tmp;
	}
};


template<>
struct HashFunc<i32>
{
	static u32 get(const i32& key)
	{
		u32 x = ((key >> 16) ^ key) * 0x45d9f3b;
		x = ((x >> 16) ^ x) * 0x45d9f3b;
		x = ((x >> 16) ^ x);
		return x;
	}
};

template<>
struct HashFunc<ComponentType>
{
	static u32 get(const ComponentType& key)
	{
		static_assert(sizeof(i32) == sizeof(key.index), "Check this");
		return HashFunc<i32>::get(key.index);
	}
};

template<>
struct HashFunc<EntityRef>
{
	static u32 get(const EntityRef& key)
	{
		static_assert(sizeof(i32) == sizeof(key.index), "Check this");
		return HashFunc<i32>::get(key.index);
	}
};

template<>
struct HashFunc<u32>
{
	static u32 get(const u32& key)
	{
		u32 x = ((key >> 16) ^ key) * 0x45d9f3b;
		x = ((x >> 16) ^ x) * 0x45d9f3b;
		x = ((x >> 16) ^ x);
		return x;
	}
};

template<typename T>
struct HashFunc<T*>
{
	static u32 get(const void* key)
	{
		#ifdef PLATFORM64
			u64 tmp = (u64)key;
			tmp = (~tmp) + (tmp << 18);
			tmp = tmp ^ (tmp >> 31);
			tmp = tmp * 21;
			tmp = tmp ^ (tmp >> 11);
			tmp = tmp + (tmp << 6);
			tmp = tmp ^ (tmp >> 22);
			return (u32)tmp;
		#else
			size_t x = ((i32(key) >> 16) ^ i32(key)) * 0x45d9f3b;
			x = ((x >> 16) ^ x) * 0x45d9f3b;
			x = ((x >> 16) ^ x);
			return x;
		#endif
	}
};

template<>
struct HashFunc<char*>
{
	static u32 get(const char* key)
	{
		u32 result = 0x55555555;

		while (*key) 
		{ 
			result ^= *key++;
			result = ((result << 5) | (result >> 27));
		}

		return result;
	}
};

template<class Key, class Value, class Hasher = HashFunc<Key>>
class HashMap
{
private:
	struct Slot {
		u8 key_mem[sizeof(Key)];
		bool valid;
		//u8 padding[3];
	};

	template <typename HM, typename K, typename V>
	struct iterator_base {
		HM* hm;
		uint idx;

		template <typename HM, typename K, typename V>
		bool operator !=(const iterator_base<HM, K, V>& rhs) const {
			ASSERT(hm == rhs.hm);
			return idx != rhs.idx;
		}

		template <typename HM, typename K, typename V>
		bool operator ==(const iterator_base<HM, K, V>& rhs) const {
			ASSERT(hm == rhs.hm);
			return idx == rhs.idx;
		}

		void operator++() { 
			const Slot* keys = hm->m_keys;
			for(uint i = idx + 1, c = hm->m_capacity; i < c; ++i) {
				if(keys[i].valid) {
					idx = i;
					return;
				}
			}
			idx = hm->m_capacity;
		}

		K& key() {
			ASSERT(hm->m_keys[idx].valid);
			return *((Key*)hm->m_keys[idx].key_mem);
		}

		const V& value() const {
			ASSERT(hm->m_keys[idx].valid);
			return hm->m_values[idx];
		}

		V& value() {
			ASSERT(hm->m_keys[idx].valid);
			return hm->m_values[idx];
		}

		V& operator*() {
			ASSERT(hm->m_keys[idx].valid);
			return hm->m_values[idx];
		}

		bool isValid() const { return idx != hm->m_capacity; }
	};

public:
	using iterator = iterator_base<HashMap, Key, Value>;
	using const_iterator = iterator_base<const HashMap, const Key, const Value>;

	explicit HashMap(IAllocator& allocator) 
		: m_allocator(allocator) 
	{
		init(8, true); 
	}

	HashMap(uint size, IAllocator& allocator) 
		: m_allocator(allocator) 
	{
		init(size, true); 
	}

	~HashMap()
	{
		for(uint i = 0, c = m_capacity; i < c; ++i) {
			if (m_keys[i].valid) {
				((Key*)m_keys[i].key_mem)->~Key();
				m_values[i].~Value();
				m_keys[i].valid = false;
			}
		}
		m_allocator.deallocate(m_keys);
		m_allocator.deallocate(m_values);
	}

	iterator begin() {
		for (uint i = 0, c = m_capacity; i < c; ++i) {
			if (m_keys[i].valid) return { this, i };
		}
		return { this, m_capacity };
	}

	const_iterator begin() const {
		for (uint i = 0, c = m_capacity; i < c; ++i) {
			if (m_keys[i].valid) return { this, i };
		}
		return { this, m_capacity };
	}

	iterator end() { return iterator { this, m_capacity }; }
	const_iterator end() const { return const_iterator { this, m_capacity }; }

	void clear() {
		for(uint i = 0, c = m_capacity; i < c; ++i) {
			if (m_keys[i].valid) {
				((Key*)m_keys[i].key_mem)->~Key();
				m_values[i].~Value();
				m_keys[i].valid = false;
			}
		}
		m_allocator.deallocate(m_keys);
		m_allocator.deallocate(m_values);
		init(8, true);
	}

	const_iterator find(const Key& key) const {
		return { this, findPos(key) };
	}

	iterator find(const Key& key) {
		return { this, findPos(key) };
	}
	
	Value& operator[](const Key& key) {
		const uint pos = findPos(key);
		ASSERT(pos < m_capacity);
		return m_values[pos];
	}
	
	const Value& operator[](const Key& key) const {
		const uint pos = findPos(key);
		ASSERT(pos < m_capacity);
		return m_values[pos];
	}

	Value& insert(const Key& key, Value&& value) {
		uint pos = Hasher::get(key) & m_mask;
		while (m_keys[pos].valid) ++pos;
		if(pos == m_capacity) {
			pos = 0;
			while (m_keys[pos].valid) ++pos;
		}

		new (NewPlaceholder(), m_keys[pos].key_mem) Key(key);
		new (NewPlaceholder(), &m_values[pos]) Value(value);
		++m_size;
		m_keys[pos].valid = true;

		if (m_size > m_capacity * 3 / 4) {
			grow(m_capacity << 1);
		}

		return m_values[pos];
	}

	iterator insert(const Key& key, const Value& value) {
		uint pos = Hasher::get(key) & m_mask;
		while (m_keys[pos].valid) ++pos;
		if(pos == m_capacity) {
			pos = 0;
			while (m_keys[pos].valid) ++pos;
		}

		new (NewPlaceholder(), m_keys[pos].key_mem) Key(key);
		new (NewPlaceholder(), &m_values[pos]) Value(value);
		++m_size;
		m_keys[pos].valid = true;

		if (m_size > m_capacity * 3 / 4) {
			grow(m_capacity << 1);
		}

		return { this, pos };
	}

	void erase(const iterator& key) {
		ASSERT(key.isValid());

		Slot* keys = m_keys;
		uint pos = key.idx;
		((Key*)keys[pos].key_mem)->~Key();
		m_values[pos].~Value();
		keys[pos].valid = false;
		--m_size;

		const uint mask = m_mask;
		while (keys[pos + 1].valid) {
			rehash(pos + 1);
			++pos;
		}
	}

	void erase(const Key& key) {
		const uint pos = findPos(key);
		if (m_keys[pos].valid) erase({this, pos});
	}

	bool empty() const { return m_size == 0; }
	uint size() const { return m_size; }

	void reserve(uint new_capacity) {
		if (new_capacity > m_capacity) grow(new_capacity);
	}

private:
	void grow(uint new_capacity) {
		HashMap<Key, Value, Hasher> tmp(new_capacity, m_allocator);
		if (m_size > 0) {
			for(auto iter = begin(); iter.isValid(); ++iter) {
				tmp.insert(iter.key(), static_cast<Value&&>(iter.value()));
			}
		}

		swap(m_capacity, tmp.m_capacity);
		swap(m_size, tmp.m_size);
		swap(m_mask, tmp.m_mask);
		swap(m_keys, tmp.m_keys);
		swap(m_values, tmp.m_values);
	}

	uint findEmptySlot(const Key& key, uint end_pos) const {
		const uint mask = m_mask;
		uint pos = Hasher::get(key) & mask;
		while (m_keys[pos].valid && pos != end_pos) ++pos;
		if (pos == m_capacity) {
			pos = 0;
			while (m_keys[pos].valid && pos != end_pos) ++pos;
		}
		return pos;
	}

	void rehash(uint pos) {
		Key& key = *((Key*)m_keys[pos].key_mem);
		const uint rehashed_pos = findEmptySlot(key, pos);
		if (rehashed_pos != pos) {
			new (NewPlaceholder(), m_keys[rehashed_pos].key_mem) Key(static_cast<Key&&>(key));
			new (NewPlaceholder(), &m_values[rehashed_pos]) Value(static_cast<Value&&>(m_values[pos]));
			
			((Key*)m_keys[pos].key_mem)->~Key();
			m_values[pos].~Value();
			m_keys[pos].valid = false;
			m_keys[rehashed_pos].valid = true;
		}
	}

	uint findPos(const Key& key) const {
		uint pos = Hasher::get(key) & m_mask;
		const Slot* LUMIX_RESTRICT keys = m_keys;
		while (keys[pos].valid) {
			if (*((Key*)keys[pos].key_mem) == key) return pos;
			++pos;
		}
		pos = 0;
		while (keys[pos].valid) {
			if (*((Key*)keys[pos].key_mem) == key) return pos;
			++pos;
		}
		return m_capacity;
	}

	void init(uint capacity, bool all_invalid) {
		ASSERT(isPowOfTwo(capacity));
		m_size = 0;
		m_mask = capacity - 1;
		m_keys = (Slot*)m_allocator.allocate(sizeof(Slot) * (capacity + 1));
		m_values = (Value*)m_allocator.allocate(sizeof(Value) * capacity);
		m_capacity = capacity;
		if (all_invalid) {
			for(uint i = 0; i < capacity; ++i) {
				m_keys[i].valid = false;
			}
		}
		m_keys[capacity].valid = false;
	}

	IAllocator& m_allocator;
	Slot* m_keys;
	Value* m_values;
	uint m_capacity;
	uint m_size;
	uint m_mask;
};


} // namespace Lumix
