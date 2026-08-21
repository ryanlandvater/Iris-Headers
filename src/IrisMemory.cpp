/**
 * @file IrisMemory.cpp
 * @author Ryan Landvater
 * @brief Virtual Memory Arena — the OS mapping calls.
 * @date 2026-08-12
 *
 * @copyright Copyright (c) 2026 Iris Digital Pathology (MIT)
 *
 * Provenance: extracted from FastFHIR's FF_Memory.cpp (MPL-2.0, copyright Ryan
 * Landvater), relicensed MIT by the author. The calls below are the same ones,
 * in the same order; what is absent is FastFHIR's stream layer — StreamHead,
 * claim_space, the FF_HEADER validation and FFHR magic, and the named SHM
 * segments — none of which Iris consumes.
 *
 * Everything platform-specific is confined to this file. priv/IrisMemory.hpp
 * declares the handles as `void*` and `int` precisely so that `windows.h`,
 * and the PLANES/TRANSPARENT/ERROR/IN/OUT macros it brings, never reach a
 * consumer's translation unit.
 */

#include "IrisMemory.hpp"

#include <cerrno>
#include <exception>
#include <string>
#include <system_error>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
// FSCTL_SET_SPARSE lives here, not in windows.h — FF_Memory.cpp includes it
// explicitly for exactly this reason.
#include <winioctl.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace Iris {

MemoryArenaCore::MemoryArenaCore(std::uint8_t* base, std::size_t capacity,
                                 std::string name, void* file_handle,
                                 void* map_handle, int fd,
                                 bool read_only) noexcept
    : m_name(std::move(name)), m_capacity(capacity), m_base(base),
      m_read_only(read_only)
#ifdef _WIN32
    , m_file_handle(file_handle), m_map_handle(map_handle)
#else
    , m_fd(fd)
#endif
{
    // The constructor signature is uniform so the header needs no platform
    // branch; the argument the platform does not use is discarded here.
#ifdef _WIN32
    (void)fd;
#else
    (void)file_handle;
    (void)map_handle;
#endif
}

void MemoryArenaCore::close() noexcept
{
    // Every field is nulled as it is released, which is what makes this
    // idempotent -- the destructor calls it, and a caller may have already.
#ifdef _WIN32
    if (m_base) {
        UnmapViewOfFile(m_base);
        m_base = nullptr;
    }
    if (m_map_handle) {
        CloseHandle(static_cast<HANDLE>(m_map_handle));
        m_map_handle = nullptr;
    }
    if (m_file_handle) {
        CloseHandle(static_cast<HANDLE>(m_file_handle));
        m_file_handle = nullptr;
    }
#else
    if (m_base) {
        munmap(m_base, m_capacity);
        m_base = nullptr;
    }
    if (m_fd != -1) {
        ::close(m_fd);
        m_fd = -1;
    }
#endif
}

MemoryArenaCore::~MemoryArenaCore() noexcept
{
    close();
}

void MemoryArenaCore::truncate_file(std::size_t size)
{
    // Anonymous arenas have no file, and a closed one has no handle: both are
    // a no-op rather than an error, so a caller need not track which it holds.
    // Read-only cores are the third case: the descriptor was opened without
    // write permission, so truncation cannot succeed and must not be tried.
    if (m_read_only) return;
#ifdef _WIN32
    if (!m_file_handle) return;
    HANDLE hFile = static_cast<HANDLE>(m_file_handle);
    LARGE_INTEGER li;
    li.QuadPart = static_cast<LONGLONG>(size);
    if (!SetFilePointerEx(hFile, li, NULL, FILE_BEGIN) || !SetEndOfFile(hFile))
        throw std::system_error(GetLastError(), std::system_category(),
                                "Iris::MemoryArena Win32 SetEndOfFile failed");
#else
    if (m_fd == -1) return;
    if (::ftruncate(m_fd, static_cast<off_t>(size)) == -1)
        throw std::system_error(errno, std::system_category(),
                                "Iris::MemoryArena POSIX ftruncate failed");
#endif
    // Deliberately louder than FF_Memory::truncate_file, which ignores the
    // result. A silent failure here yields a file whose length disagrees with
    // the size recorded inside it, and the consumer then sees a validation
    // error about the file size with nothing pointing back at the truncation.
}

// The three mapping recipes below stay separate functions rather than one
// branchy body: they are three different OS call sequences, not three
// argument sets, and each still throws so the errno / GetLastError value
// survives to the boundary in create_memory_arena. They are internal —
// create_memory_arena is the only way in from outside this file.
namespace {

MemoryArena create_anonymous(std::size_t capacity)
{
#ifdef _WIN32
    const std::uint64_t total = static_cast<std::uint64_t>(capacity);
    HANDLE hMap = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
                                     static_cast<DWORD>(total >> 32),
                                     static_cast<DWORD>(total & 0xFFFFFFFFu),
                                     NULL);
    if (!hMap)
        throw std::system_error(GetLastError(), std::system_category(),
                                "Iris::MemoryArena Win32 CreateFileMappingA failed");
    std::uint8_t* base = static_cast<std::uint8_t*>(
        MapViewOfFile(hMap, FILE_MAP_ALL_ACCESS, 0, 0, total));
    if (!base) {
        CloseHandle(hMap);
        throw std::system_error(GetLastError(), std::system_category(),
                                "Iris::MemoryArena Win32 MapViewOfFile failed");
    }
    return MemoryArena(std::make_shared<MemoryArenaCore>(
        base, capacity, "", nullptr, static_cast<void*>(hMap), -1));
#else
    std::uint8_t* base = static_cast<std::uint8_t*>(
        mmap(nullptr, capacity, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    if (base == MAP_FAILED)
        throw std::system_error(errno, std::system_category(),
                                "Iris::MemoryArena POSIX anonymous mmap failed");
    return MemoryArena(std::make_shared<MemoryArenaCore>(
        base, capacity, "", nullptr, nullptr, -1));
#endif
}

MemoryArena create_file_backed(const std::filesystem::path& path,
                               std::size_t capacity)
{
    const std::string path_str = path.string();

#ifdef _WIN32
    const std::uint64_t total = static_cast<std::uint64_t>(capacity);
    HANDLE hFile = CreateFileA(path_str.c_str(), GENERIC_READ | GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                               OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE)
        throw std::system_error(GetLastError(), std::system_category(),
                                "Iris::MemoryArena Win32 CreateFileA failed");

    // Sparse mark FIRST: without FSCTL_SET_SPARSE, writing the file to its
    // mapped length materialises zeros — a 4 GiB arena becomes a 4 GiB file.
    DWORD bytes_returned = 0;
    if (!DeviceIoControl(hFile, FSCTL_SET_SPARSE, NULL, 0, NULL, 0,
                         &bytes_returned, NULL)) {
        CloseHandle(hFile);
        throw std::system_error(GetLastError(), std::system_category(),
                                "Iris::MemoryArena Win32 FSCTL_SET_SPARSE failed");
    }

    HANDLE hMap = CreateFileMappingA(hFile, NULL, PAGE_READWRITE,
                                     static_cast<DWORD>(total >> 32),
                                     static_cast<DWORD>(total & 0xFFFFFFFFu),
                                     NULL);
    if (!hMap) {
        CloseHandle(hFile);
        throw std::system_error(GetLastError(), std::system_category(),
                                "Iris::MemoryArena Win32 CreateFileMappingA failed");
    }
    std::uint8_t* base = static_cast<std::uint8_t*>(
        MapViewOfFile(hMap, FILE_MAP_ALL_ACCESS, 0, 0, total));
    if (!base) {
        CloseHandle(hMap);
        CloseHandle(hFile);
        throw std::system_error(GetLastError(), std::system_category(),
                                "Iris::MemoryArena Win32 MapViewOfFile failed");
    }
    return MemoryArena(std::make_shared<MemoryArenaCore>(
        base, capacity, path_str, static_cast<void*>(hFile),
        static_cast<void*>(hMap), -1));
#else
    const int fd = ::open(path_str.c_str(), O_CREAT | O_RDWR, 0666);
    if (fd == -1)
        throw std::system_error(errno, std::system_category(),
                                "Iris::MemoryArena POSIX open failed");

    // Grow-only: never shrink a pre-existing backing file.
    struct stat st {};
    if (::fstat(fd, &st) == -1) {
        ::close(fd);
        throw std::system_error(errno, std::system_category(),
                                "Iris::MemoryArena POSIX fstat failed");
    }
    if (static_cast<std::size_t>(st.st_size) < capacity) {
        if (::ftruncate(fd, static_cast<off_t>(capacity)) == -1) {
            ::close(fd);
            throw std::system_error(errno, std::system_category(),
                                    "Iris::MemoryArena POSIX ftruncate failed");
        }
    }

    std::uint8_t* base = static_cast<std::uint8_t*>(
        mmap(nullptr, capacity, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
    if (base == MAP_FAILED) {
        ::close(fd);
        throw std::system_error(errno, std::system_category(),
                                "Iris::MemoryArena POSIX file mmap failed");
    }
    return MemoryArena(std::make_shared<MemoryArenaCore>(
        base, capacity, path_str, nullptr, nullptr, fd));
#endif
}

MemoryArena create_file_read_only(const std::filesystem::path& path)
{
    const std::string path_str = path.string();

#ifdef _WIN32
    HANDLE hFile = CreateFileA(path_str.c_str(), GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE)
        throw std::system_error(GetLastError(), std::system_category(),
                                "Iris::MemoryArena Win32 CreateFileA (read-only) failed");

    LARGE_INTEGER file_size;
    if (!GetFileSizeEx(hFile, &file_size)) {
        CloseHandle(hFile);
        throw std::system_error(GetLastError(), std::system_category(),
                                "Iris::MemoryArena Win32 GetFileSizeEx failed");
    }
    const std::uint64_t total = static_cast<std::uint64_t>(file_size.QuadPart);

    // No FSCTL_SET_SPARSE here: this mapping never writes, so there is
    // nothing to keep sparse.
    HANDLE hMap = CreateFileMappingA(hFile, NULL, PAGE_READONLY,
                                     static_cast<DWORD>(total >> 32),
                                     static_cast<DWORD>(total & 0xFFFFFFFFu),
                                     NULL);
    if (!hMap) {
        CloseHandle(hFile);
        throw std::system_error(GetLastError(), std::system_category(),
                                "Iris::MemoryArena Win32 CreateFileMappingA (read-only) failed");
    }
    std::uint8_t* base = static_cast<std::uint8_t*>(
        MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, total));
    if (!base) {
        CloseHandle(hMap);
        CloseHandle(hFile);
        throw std::system_error(GetLastError(), std::system_category(),
                                "Iris::MemoryArena Win32 MapViewOfFile (read-only) failed");
    }
    return MemoryArena(std::make_shared<MemoryArenaCore>(
        base, static_cast<std::size_t>(total), path_str,
        static_cast<void*>(hFile), static_cast<void*>(hMap), -1, true));
#else
    int fd = ::open(path_str.c_str(), O_RDONLY);
    if (fd < 0)
        throw std::system_error(errno, std::system_category(),
                                "Iris::MemoryArena POSIX open (read-only) failed");

    struct stat st {};
    if (::fstat(fd, &st) != 0) {
        ::close(fd);
        throw std::system_error(errno, std::system_category(),
                                "Iris::MemoryArena POSIX fstat failed");
    }
    // No ftruncate: the mapped length is the file's own size, and the
    // descriptor was opened read-only.
    std::uint8_t* base = static_cast<std::uint8_t*>(
        mmap(nullptr, static_cast<std::size_t>(st.st_size), PROT_READ,
             MAP_SHARED, fd, 0));
    if (base == MAP_FAILED) {
        ::close(fd);
        throw std::system_error(errno, std::system_category(),
                                "Iris::MemoryArena POSIX mmap (read-only) failed");
    }
    return MemoryArena(std::make_shared<MemoryArenaCore>(
        base, static_cast<std::size_t>(st.st_size), path_str,
        nullptr, nullptr, fd, true));
#endif
}

}  // namespace

Result create_memory_arena(const MemoryArenaCreateInfo& info,
                           MemoryArena& out_arena) noexcept
{
    // Emptied before anything can fail, including the argument checks below:
    // "empty on failure" has to hold on *every* return, or a caller reusing a
    // handle keeps the mapping it thinks was just replaced. (FF_CreateMemory
    // clears after its argument checks and so does not hold this for them.)
    out_arena = MemoryArena{};

    // Contradictions are rejected before any OS call, so the caller gets the
    // mistake rather than whatever errno the kernel produces from it.
    if (info.read_only && info.filepath.empty())
        return {IRIS_FAILURE,
                "create_memory_arena: read_only needs a filepath; there is no "
                "read-only form of an anonymous arena"};
    if (!info.read_only && info.capacity == 0)
        return {IRIS_FAILURE,
                "create_memory_arena: capacity is zero; both platforms reject a "
                "zero-length mapping"};

    // The catch-all is the point of the boundary -- "nothing throws past
    // create_memory_arena" is the contract this header now publishes, and a
    // mapping failure that escaped it would cross into C consumers and the
    // Python and WASM bindings, which cannot catch anything.
    try {
        if (info.filepath.empty())
            out_arena = create_anonymous(info.capacity);
        else if (info.read_only)
            out_arena = create_file_read_only(info.filepath);
        else
            out_arena = create_file_backed(info.filepath, info.capacity);
        return IRIS_SUCCESS;
    } catch (const std::system_error& e) {
        // Carries the OS code as well as the text; losing it would make a
        // permission error and a missing directory read identically.
        return {IRIS_FAILURE, std::string("create_memory_arena: ") + e.what() +
                              " (code " + std::to_string(e.code().value()) + ")"};
    } catch (const std::exception& e) {
        return {IRIS_FAILURE, std::string("create_memory_arena: ") + e.what()};
    } catch (...) {
        return {IRIS_FAILURE, "create_memory_arena: unknown non-std exception"};
    }
}

}  // namespace Iris
