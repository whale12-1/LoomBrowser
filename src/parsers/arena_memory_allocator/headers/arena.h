#pragma once
#include <vector>
#include <cstdint>
#include <cstddef>
#include <memory>
#include <utility>
#include <memory_resource>

class ArenaAllocator {
public:
	explicit ArenaAllocator(size_t Ichunk_size = 64 * 1024) : m_chunk_size(Ichunk_size) {
		AllocateNewChunk();
	}
	ArenaAllocator(const ArenaAllocator&) = delete;
	ArenaAllocator& operator=(const ArenaAllocator&) = delete;

	void* allocate(std::size_t size, std::size_t alignment) {
		void* ptr = m_current_chunk->data() + m_current_offset;
		std::size_t space = m_chunk_size - m_current_offset;
		if (!std::align(alignment, size, ptr, space)) {
			AllocateNewChunk();
			ptr = m_current_chunk->data() + m_current_offset;
			space = m_chunk_size - m_current_offset;
			std::align(alignment, size, ptr, space);
		}
		m_current_offset = m_chunk_size - space + size;
		return ptr;
	}

	template <typename T, typename ... Args>
	T* Alloc(Args&& ... args) {
		void* p = allocate(sizeof(T), alignof(T));
		return new(p) T(std::forward<Args>(args)...);
	}

	void reset() {
		m_chunks.clear();
		AllocateNewChunk();
	}

private:
	void AllocateNewChunk() {
		m_chunks.push_back(std::vector<uint8_t>(m_chunk_size));
		m_current_chunk = &m_chunks.back();
		m_current_offset = 0;

	}
	size_t m_chunk_size;
	std::vector<std::vector<uint8_t>> m_chunks;
	std::vector<uint8_t>* m_current_chunk = nullptr;
	size_t m_current_offset = 0;
};


class ArenaMemoryResource : public std::pmr::memory_resource {
	ArenaAllocator* arena_ = nullptr;
public:
	explicit ArenaMemoryResource(ArenaAllocator* a) : arena_(a) {}

	void set(ArenaAllocator* a) { arena_ = a; }

protected:
	void* do_allocate(std::size_t bytes, std::size_t align) override {
		return arena_ ? arena_->allocate(bytes, align) : ::operator new(bytes);
	}
	void do_deallocate(void*, std::size_t, std::size_t) override {}
	bool do_is_equal(const std::pmr::memory_resource& o) const noexcept override {
		return this == &o;
	}
};