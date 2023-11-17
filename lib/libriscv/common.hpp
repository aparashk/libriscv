#pragma once
#include "libriscv_settings.h" // Build system generated

#include <type_traits>
#include <string>
#include <string_view>
#include "util/function.hpp"
#include "types.hpp"

#ifndef RISCV_SYSCALLS_MAX
#define RISCV_SYSCALLS_MAX   512
#endif

#ifndef RISCV_SYSCALL_EBREAK_NR
#define RISCV_SYSCALL_EBREAK_NR    (RISCV_SYSCALLS_MAX-1)
#endif

#ifndef RISCV_PAGE_SIZE
#define RISCV_PAGE_SIZE  4096UL
#endif

#ifndef RISCV_FORCE_ALIGN_MEMORY
#define RISCV_FORCE_ALIGN_MEMORY 1
#endif

namespace riscv
{
	static constexpr int SYSCALL_EBREAK = RISCV_SYSCALL_EBREAK_NR;

	static constexpr size_t PageSize = RISCV_PAGE_SIZE;
	static constexpr size_t PageMask = RISCV_PAGE_SIZE-1;

#ifdef RISCV_MEMORY_TRAPS
	static constexpr bool memory_traps_enabled = true;
#else
	static constexpr bool memory_traps_enabled = false;
#endif

#if RISCV_FORCE_ALIGN_MEMORY
	static constexpr bool force_align_memory = true;
#else
	static constexpr bool force_align_memory = false;
#endif

#ifdef RISCV_DEBUG
	static constexpr bool memory_alignment_check = true;
	static constexpr bool verbose_branches_enabled = false;
	static constexpr bool unaligned_memory_slowpaths = true;
#else
	static constexpr bool memory_alignment_check = false;
	static constexpr bool verbose_branches_enabled = false;
	static constexpr bool unaligned_memory_slowpaths = false;
#endif

#ifdef RISCV_EXT_A
#define RISCV_EXT_ATOMICS
	static constexpr bool atomics_enabled = true;
#else
	static constexpr bool atomics_enabled = false;
#endif
#ifdef RISCV_EXT_C
#define RISCV_EXT_COMPRESSED
	static constexpr bool compressed_enabled = true;
#else
	static constexpr bool compressed_enabled = false;
#endif
#ifdef RISCV_EXT_V
#define RISCV_EXT_VECTOR 32
	static constexpr unsigned vector_extension = RISCV_EXT_VECTOR;
#else
	static constexpr unsigned vector_extension = 0;
#endif
#ifdef RISCV_128I
#define RISCV_128BIT_ISA
	static constexpr bool rv128i_enabled = true;
#else
	static constexpr bool rv128i_enabled = false;
#endif
#ifdef RISCV_BINARY_TRANSLATION
	static constexpr bool binary_translation_enabled = true;
#else
	static constexpr bool binary_translation_enabled = false;
#endif
#ifdef __linux__
	static constexpr bool memory_arena_is_default = true;
#else
	static constexpr bool memory_arena_is_default = false;
#endif
#ifdef RISCV_FLAT_RW_ARENA
	static constexpr bool flat_readwrite_arena = true;
#else
	static constexpr bool flat_readwrite_arena = false;
#endif
#ifdef RISCV_LIBTCC
	static constexpr bool libtcc_enabled = true;
#else
	static constexpr bool libtcc_enabled = false;
#endif
}

namespace riscv
{
	template <int W> struct Memory;

	template <int W>
	struct MachineOptions
	{
		uint64_t memory_max = 64ull << 20; // 64MB
		uint64_t stack_size = 1ul << 20; // 1MB default stack
		unsigned cpu_id = 0;
		bool load_program = true;
		bool protect_segments = true;
		bool allow_write_exec_segment = false;
		bool enforce_exec_only = false;
		bool verbose_loader = false;
		// Perform experimental minimal dynamic linking (-fPIC)
		bool dynamic_linking = false;
		// Minimal fork does not loan any pages from the source Machine
		bool minimal_fork = false;
		// Allow the use of a linear arena to increase memory locality somewhat
		bool use_memory_arena = memory_arena_is_default;
		// Override exit function with a program-provided function
		std::string_view default_exit_function {};

		riscv::Function<struct Page&(Memory<W>&, address_type<W>, bool)> page_fault_handler = nullptr;

#ifdef RISCV_BINARY_TRANSLATION
		unsigned block_size_treshold = 5;
		unsigned translate_blocks_max = 16'000;
		unsigned translate_instr_max = 150'000;
#ifdef RISCV_LIBTCC
		std::string libtcc1_location;
#endif
#endif
	};

	template <int W> struct MultiThreading;
	template <int W> struct Multiprocessing;
	template <int W> struct SerializedMachine;
	struct Arena;

	template <typename T>
	using remove_cvref = std::remove_cv_t<std::remove_reference_t<T>>;

	template <class...> constexpr std::false_type always_false {};

	template<typename T>
	struct is_string
		: public std::disjunction<
			std::is_same<char *, typename std::decay<T>::type>,
			std::is_same<const char *, typename std::decay<T>::type>
	> {};

	template<class T>
	struct is_stdstring : public std::is_same<T, std::basic_string<char>> {};
} // riscv

#ifdef __GNUG__

#ifndef LIKELY
#define LIKELY(x) __builtin_expect((x), 1)
#endif
#ifndef UNLIKELY
#define UNLIKELY(x) __builtin_expect((x), 0)
#endif
#ifndef RISCV_COLD_PATH
#define RISCV_COLD_PATH() __attribute__((cold))
#endif
#ifndef RISCV_HOT_PATH
#define RISCV_HOT_PATH() __attribute__((hot))
#endif
#define RISCV_NOINLINE __attribute__((noinline))
#define RISCV_ALWAYS_INLINE __attribute__((always_inline))

#else
#define LIKELY(x)   (x)
#define UNLIKELY(x) (x)
#define RISCV_COLD_PATH() /* */
#define RISCV_HOT_PATH()  /* */
#endif

#ifdef _MSC_VER
#define RISCV_ALWAYS_INLINE __forceinline
#define RISCV_NOINLINE      __declspec(noinline)
#endif

#ifdef __HAVE_BUILTIN_SPECULATION_SAFE_VALUE
#define RISCV_SPECSAFE(x) __builtin_speculation_safe_value(x)
#else
#define RISCV_SPECSAFE(x) (x)
#endif

#ifndef RISCV_INTERNAL
#if defined(__GNUG__) && !defined(_WIN32)
#define RISCV_INTERNAL __attribute__((visibility("internal")))
#else
#define RISCV_INTERNAL /* */
#endif
#endif

#ifdef RISCV_128BIT_ISA
#define INSTANTIATE_128_IF_ENABLED(x) template struct x<16>
#else
#define INSTANTIATE_128_IF_ENABLED(x) /* */
#endif
