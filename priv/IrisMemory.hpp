/**
 * @file IrisMemory.hpp
 * @author Ryan Landvater
 * @brief Virtual Memory Arena (VMA) — cross-platform anonymous and sparse
 *        file-backed memory mapping.
 * @version 0.1
 * @date 2026-08-12
 *
 * @copyright Copyright (c) 2026 Iris Digital Pathology (MIT)
 *
 * Provenance: extracted from FastFHIR's FF_Memory (src/FF_memory.cpp,
 * include/FF_Memory.hpp), MPL-2.0, copyright Ryan Landvater; relicensed MIT by
 * the author for inclusion here. The FastFHIR stream machinery (StreamHead,
 * FF_HEADER layout, SHM segments, the "FFHR" magic) is deliberately absent:
 * this is the arena core only — map, sparse-mark, truncate, unmap.
 *
 * \note **No OS headers here.** The implementation lives in src/IrisMemory.cpp
 * so that `windows.h` is not pulled into every translation unit that wants an
 * arena. That is not hygiene for its own sake: `<wingdi.h>` defines PLANES,
 * TRANSPARENT and ERROR as macros, `<winnt.h>` defines IN, OUT and DELETE, and
 * consumers of these headers declare fields by those names — Iris-File-Extension
 * keeps a lint table of the collisions and named LayerExtent::zPlanes around
 * one of them. The handles below are deliberately `void*` and `int` rather than
 * HANDLE and a descriptor type, which is what lets this header stay clean.
 */

#ifndef IrisMemory_hpp
#define IrisMemory_hpp

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace Iris {

/**
 * @brief Shared core of one OS memory mapping.
 *
 * Owned through `MemoryArena`'s shared_ptr: multiple handles, one mapping.
 * The mapping is demand-paged — an anonymous arena costs nothing until
 * touched, and a sparse file-backed arena costs only the pages actually
 * written. Unmaps and closes its handles when the last handle dies, or
 * earlier if a caller invokes `close()`.
 *
 * \warning 64-bit hosts only. The mapping size is a SIZE_T; a multi-GiB
 * arena cannot be addressed from a 32-bit process, and NTFS sparse files
 * require the FSCTL the implementation performs. Consumers needing the
 * portable windowed/remote path should not use this on wasm — see
 * IFE_Window's Emscripten branch for ranged fetch.
 */
class MemoryArenaCore {
public:
    /// Uniform across platforms; the unused handle is simply ignored. Called
    /// only by the factories on MemoryArena.
    MemoryArenaCore(std::uint8_t* base, std::size_t capacity, std::string name,
                    void* file_handle, void* map_handle, int fd) noexcept;

    ~MemoryArenaCore() noexcept;

    MemoryArenaCore(const MemoryArenaCore&)            = delete;
    MemoryArenaCore& operator=(const MemoryArenaCore&) = delete;

    /// Unmap and release the OS handles. Idempotent; the destructor calls it.
    void close() noexcept;

    /// Shrink (or grow) the backing file. No-op when anonymous or closed.
    void truncate_file(std::size_t size);

    [[nodiscard]] std::uint8_t*      base()     const noexcept { return m_base; }
    [[nodiscard]] std::size_t        capacity() const noexcept { return m_capacity; }
    [[nodiscard]] const std::string& name()     const noexcept { return m_name; }

private:
    std::string   m_name;
    std::size_t   m_capacity = 0;
    std::uint8_t* m_base     = nullptr;
#ifdef _WIN32
    void* m_file_handle = nullptr;   // CreateFileA handle (file-backed only)
    void* m_map_handle  = nullptr;   // CreateFileMapping handle
#else
    int   m_fd          = -1;        // open() descriptor (file-backed only)
#endif
};

/**
 * @brief Copyable handle over a shared virtual memory arena.
 *
 * Mirrors FastFHIR's FF_Memory handle/body split: copying a handle shares the
 * same underlying mapping rather than duplicating it.
 */
class MemoryArena {
public:
    MemoryArena() = default;
    explicit MemoryArena(std::shared_ptr<MemoryArenaCore> core) noexcept
        : m_core(std::move(core)) {}

    /**
     * @brief Anonymous read/write mapping (no backing file).
     * @param capacity Reserved address range; pages materialise on touch.
     * @throws std::system_error if the mapping cannot be created.
     */
    static MemoryArena create(std::size_t capacity = 4ull * 1024 * 1024 * 1024);

    /**
     * @brief File-backed read/write mapping, sparse on both platforms.
     *
     * POSIX: `open` + `ftruncate` (grow-only, preserves existing content) +
     * `mmap(MAP_SHARED)`. NTFS: `CreateFileA` + `FSCTL_SET_SPARSE` before
     * mapping, so a multi-GiB arena costs only the pages actually written
     * rather than zeros on disk.
     *
     * \note Existing content is preserved. A caller that needs a clean arena
     * must truncate or remove the file first.
     *
     * @param path Backing file; created if absent.
     * @param capacity Reserved address range; the file is extended to this
     *        size but only written pages occupy disk.
     * @throws std::system_error if the file cannot be opened, marked sparse,
     *         extended or mapped.
     */
    static MemoryArena create_from_file(
        const std::filesystem::path& path,
        std::size_t capacity = 4ull * 1024 * 1024 * 1024);

    /// True when this handle refers to a live mapping.
    [[nodiscard]] explicit operator bool() const noexcept {
        return m_core != nullptr;
    }

    /// The mapping's base pointer. Offsets into the arena are relative to it.
    [[nodiscard]] std::uint8_t* base() const noexcept {
        return m_core ? m_core->base() : nullptr;
    }

    /// Total reserved address range.
    [[nodiscard]] std::size_t capacity() const noexcept {
        return m_core ? m_core->capacity() : 0;
    }

    /// Backing path (file-backed) or empty (anonymous).
    [[nodiscard]] std::string name() const noexcept {
        return m_core ? m_core->name() : std::string();
    }

    /**
     * @brief Truncate the backing file to @p size bytes. No-op when anonymous.
     *
     * `create_from_file` extends the file to `capacity`, so a caller that maps
     * a large arena and writes less must call this or ship a file whose length
     * is the arena's rather than the payload's. That is not cosmetic for Iris:
     * an IFE slide records its own length in `FILE_HEADER.FILE_SIZE`, and
     * `validate_file_structure` compares it against the size the OS reports —
     * an untruncated arena-written slide fails its own validation.
     *
     * \warning Truncating below the mapped length leaves the mapping covering
     * bytes past end-of-file; touching those pages raises SIGBUS on POSIX.
     * Call it once writing is finished, with the payload size, and do not
     * write through `base()` afterwards.
     *
     * @throws std::system_error if the truncation fails.
     */
    void truncate_file(std::size_t size) const {
        if (m_core) m_core->truncate_file(size);
    }

    /**
     * @brief Release the mapping and its OS handles now, without waiting for
     *        the last handle copy to die. Idempotent.
     *
     * On Windows a file-backed mapping holds the backing file open for the
     * mapping's lifetime, so the file cannot be deleted or its directory
     * removed until every handle is gone. Dropping a `MemoryArena` only
     * unmaps when it happens to hold the final reference; this always does.
     *
     * `base()` returns nullptr afterwards. The handle still tests true — it
     * refers to a core, that core simply no longer owns a mapping.
     */
    void close() const noexcept {
        if (m_core) m_core->close();
    }

private:
    std::shared_ptr<MemoryArenaCore> m_core;
};

}  // namespace Iris

#endif  // IrisMemory_hpp
