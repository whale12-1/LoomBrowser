#pragma once
#include <vector>
#include <cstdint>
#include <cstddef>
#include <memory>
#include <utility>

class ArenaAllocator {
public:
	explicit ArenaAllocator(size_t Ichunk_size = 64 * 1024) : m_chunk_size(Ichunk_size) {
		AllocateNewChunk();
	}
	ArenaAllocator(const ArenaAllocator&) = delete;
	ArenaAllocator& operator=(const ArenaAllocator&) = delete;

	template <typename T, typename ... Args>
	T* Alloc(Args&& ... args) {
		size_t alignment = alignof(T);
		size_t size = sizeof(T);

		void* current_ptr = m_current_chunk->data() + m_current_offset;
		size_t space = m_chunk_size - m_current_offset;

		if (!std::align(alignment, size, current_ptr, space)) {
			AllocateNewChunk();
			current_ptr = m_current_chunk->data() + m_current_offset;
			space = m_chunk_size - m_current_offset;
			std::align(alignment, size, current_ptr, space);
		}

		m_current_offset = m_chunk_size - space + size;
		return new(current_ptr) T(std::forward<Args>(args)...);
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
