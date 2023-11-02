#include <cmath>
#include <dlfcn.h>
#include <unistd.h>
#include "machine.hpp"
#include "decoder_cache.hpp"
#include "instruction_list.hpp"
#include "rv32i_instr.hpp"
#include "tr_api.hpp"
#include "tr_types.hpp"
#include "util/crc32.hpp"

namespace riscv
{
	static constexpr bool VERBOSE_BLOCKS = false;
	static constexpr bool BINTR_TIMING = false;
	static constexpr bool SCAN_FOR_GP = true;

	inline timespec time_now();
	inline long nanodiff(timespec, timespec);
	#define TIME_POINT(x) \
		[[maybe_unused]] timespec x;  \
		if constexpr (BINTR_TIMING) { \
			asm("" : : : "memory");   \
			x = time_now();           \
			asm("" : : : "memory");   \
		}
	extern void  dylib_close(void* dylib);
	extern void* dylib_lookup(void* dylib, const char*);

template <int W>
inline uint32_t opcode(const TransInstr<W>& ti) {
	return rv32i_instruction{ti.instr}.opcode();
}

template <int W>
inline DecoderData<W>& decoder_entry_at(const DecodedExecuteSegment<W>& exec, address_type<W> addr) {
	return exec.decoder_cache()[addr / DecoderCache<W>::DIVISOR];
}

template <int W>
int CPU<W>::load_translation(const MachineOptions<W>& options,
	std::string* filename, DecodedExecuteSegment<W>& exec) const
{
	if constexpr (libtcc_enabled) {
		return 1;
	}

	// Disable translator with NO_TRANSLATE=1
	// or by setting max blocks to zero.
	if (0 == options.translate_blocks_max || getenv("NO_TRANSLATE")) {
		if (getenv("VERBOSE")) {
			printf("Binary translation disabled\n");
		}
		exec.set_binary_translated(nullptr);
		return -1;
	}
	if (exec.is_binary_translated()) {
		throw MachineException(ILLEGAL_OPERATION, "Execute segment already binary translated");
	}

	auto* exec_data = exec.exec_data(exec.exec_begin());

	// Checksum the execute segment + compiler flags
	TIME_POINT(t5);
	extern std::string compile_command(int arch, uint64_t arena_size, uint64_t arena_roend);
	const auto cc = compile_command(W, machine().memory.memory_arena_size(), machine().memory.initial_rodata_end());
	const uint32_t checksum =
		crc32c(exec_data, exec.exec_end() - exec.exec_begin())
		^ crc32c(cc.c_str(), cc.size());

	char filebuffer[256];
	int len = snprintf(filebuffer, sizeof(filebuffer),
		"/tmp/rvbintr-%08X", checksum);
	if (len <= 0)
		return -1;

	void* dylib = nullptr;
	if constexpr (BINTR_TIMING) {
		TIME_POINT(t6);
		printf(">> Execute segment hashing took %ld ns\n", nanodiff(t5, t6));
	}

	// Always check if there is an existing file
	if (access(filebuffer, R_OK) == 0) {
		TIME_POINT(t7);
		dylib = dlopen(filebuffer, RTLD_LAZY);
		if constexpr (BINTR_TIMING) {
			TIME_POINT(t8);
			printf(">> dlopen took %ld ns\n", nanodiff(t7, t8));
		}
	}

	// We must compile ourselves
	if (dylib == nullptr) {
		if (filename) *filename = std::string(filebuffer);
		return 1;
	}

	this->activate_dylib(exec, dylib);

	if constexpr (BINTR_TIMING) {
		TIME_POINT(t10);
		printf(">> Loading binary translation took %ld ns\n", nanodiff(t5, t10));
	}
	return 0;
}

template <int W>
void CPU<W>::try_translate(const MachineOptions<W>& options,
	const std::string& filename,
	DecodedExecuteSegment<W>& exec, address_t basepc, std::vector<TransInstr<W>> ipairs) const
{
	// Run with VERBOSE=1 to see command and output
	const bool verbose = (getenv("VERBOSE") != nullptr);

	address_t gp = 0;
	TIME_POINT(t0);
if constexpr (SCAN_FOR_GP) {
	// We assume that GP is initialized with AUIPC,
	// followed by OP_IMM (and maybe OP_IMM32)
	for (auto it = ipairs.begin(); it != ipairs.end(); ++it) {
		const rv32i_instruction instruction {it->instr};
		if (instruction.opcode() == RV32I_AUIPC) {
			const auto auipc = instruction;
			if (auipc.Utype.rd == 3) { // GP
				// calculate current PC for AUIPC
				const address_t pc = basepc + 4 * (it - ipairs.begin());
				// Malicious input
				if (UNLIKELY(it+1 == ipairs.end()))
					return;

				const auto addi = rv32i_instruction {(it+1)->instr};
				if (addi.opcode() == RV32I_OP_IMM && addi.Itype.funct3 == 0x0) {
					//printf("Found OP_IMM: ADDI  rd=%d, rs1=%d\n", addi.Itype.rd, addi.Itype.rs1);
					if (addi.Itype.rd == 3 && addi.Itype.rs1 == 3) { // GP
						gp = pc + auipc.Utype.upper_imm() + addi.Itype.signed_imm();
						break;
					}
				} else {
					gp = pc + auipc.Utype.upper_imm();
					break;
				}
			}
		} // opcode
	} // iterator
	if constexpr (BINTR_TIMING) {
		TIME_POINT(t1);
		printf(">> GP scan took %ld ns, GP=0x%lX\n", nanodiff(t0, t1), (long)gp);
	}
} // SCAN_FOR_GP

	// Code block and loop detection
	TIME_POINT(t2);
	size_t icounter = 0;
	auto it = ipairs.begin();
	std::vector<std::pair<decltype(it), address_t>> loops;
	struct CodeBlock {
		TransInstr<W>& instr;
		size_t      length;
		address_t   addr;
		bool        has_branch;
		std::set<address_t> jump_locations;
	};
	std::vector<CodeBlock> blocks;
	std::set<address_t> jump_locations;

	while (it != ipairs.end() && icounter < options.translate_instr_max)
	{
		const auto block = it;
		bool has_branch = false;
		// Measure length of instructions that belong
		// together sequentially (a code block).
		for (it = block; it != ipairs.end(); ++it) {
			const rv32i_instruction instruction{it->instr};
			const auto opcode = instruction.opcode();
			// JALR and STOP are show-stoppers / code-block enders
			if (opcode == RV32I_JALR || instruction.whole == 0x7ff00073) {
				++it; break;
			}
		} // find block

		const auto block_end = it;
		auto block_end_pc = basepc + (block_end - block) * 4;
		auto current_pc = basepc;
		it = block;

		// Find jump locations inside block
		for (; it != block_end; ++it) {
			const rv32i_instruction instruction{it->instr};
			const auto opcode = instruction.opcode();

			// detect far JAL, otherwise use as local jump
			if (opcode == RV32I_JAL) {
				const auto offset = instruction.Jtype.jump_offset();
				// Long jumps are considered returnable
				if (current_pc + offset < basepc || current_pc + offset >= block_end_pc) {
					block_end_pc = current_pc + 4;
					++it; break;
				}
				has_branch = true;
				jump_locations.insert(current_pc + offset);
			}
			// loop detection (negative branch offsets)
			if (opcode == RV32I_BRANCH) {
				has_branch = true;
				// detect jump location
				const auto offset = instruction.Btype.signed_imm();
				jump_locations.insert(current_pc + offset);
			}

			current_pc += 4;
		} // process block

		// Process block and add it for emission
		const size_t length = it - block;
		if (length >= options.block_size_treshold
			&& icounter + length < options.translate_instr_max)
		{
			if constexpr (VERBOSE_BLOCKS) {
				printf("Block found at %#lX. Length: %zu\n", (long) basepc, length);
			}
			blocks.push_back({
				*block, length, basepc, has_branch,
				std::move(jump_locations)
			});
			icounter += length;
			// we can't translate beyond this estimate, otherwise
			// the compiler will never finish code generation
			if (blocks.size() >= options.translate_blocks_max)
				break;
		}

		basepc = block_end_pc;
	}

	TIME_POINT(t3);
	if constexpr (BINTR_TIMING) {
		printf(">> Code block detection %ld ns\n", nanodiff(t2, t3));
	}

	// Code generation
	std::vector<TransMapping<W>> dlmappings;
	extern const std::string bintr_code;
	std::string code = bintr_code;

	for (const auto& block : blocks)
	{
		auto result = emit(code, &block.instr, {
			block.addr, gp, (int)block.length,
			block.has_branch,
			true, // forward jumps
			std::move(block.jump_locations)
		});
		for (auto& mapping : result) {
			dlmappings.push_back(std::move(mapping));
		}
	}
	// Append all instruction handler -> dl function mappings
	code += "const uint32_t no_mappings = "
		+ std::to_string(dlmappings.size()) + ";\n";
	code += R"V0G0N(
struct Mapping {
	addr_t addr;
	void (*handler)();
};
const struct Mapping mappings[] = {
)V0G0N";
	for (const auto& mapping : dlmappings)
	{
		char buffer[128];
		snprintf(buffer, sizeof(buffer), 
			"{0x%lX, %s},\n",
			(long)mapping.addr, mapping.symbol.c_str());
		code.append(buffer);
	}
	code += "};\n";

	if constexpr (BINTR_TIMING) {
		TIME_POINT(t4);
		printf(">> Code generation took %ld ns\n", nanodiff(t3, t4));
	}

	if (verbose) {
		printf("Emitted %zu accelerated instructions and %zu functions. GP=0x%lX\n",
			icounter, dlmappings.size(), (long) gp);
	}
	// nothing to compile without mappings
	if (dlmappings.empty()) {
		if (verbose) {
			printf("Binary translator has nothing to compile! No mappings.\n");
		}
		return;
	}

	void* dylib = nullptr;

	TIME_POINT(t9);
	if constexpr (libtcc_enabled) {
		extern void* libtcc_compile(const std::string& code, int arch, uint64_t arena_size, uint64_t arena_roend, const std::string&);
		dylib = libtcc_compile(code, W, machine().memory.memory_arena_size(), machine().memory.initial_rodata_end(), options.libtcc1_location);

	} else {
		extern void* compile(const std::string& code, int arch, uint64_t arena_size, uint64_t arena_roend, const char*);
		dylib = compile(code, W, machine().memory.memory_arena_size(), machine().memory.initial_rodata_end(), filename.c_str());
	}
	if constexpr (BINTR_TIMING) {
		TIME_POINT(t10);
		printf(">> Code compilation took %.2f ms\n", nanodiff(t9, t10) / 1e6);
	}

	// Check compilation result
	if (dylib == nullptr) {
		return;
	}

	this->activate_dylib(exec, dylib);

	if constexpr (!libtcc_enabled) {
		if (getenv("NO_TR_CACHE") != nullptr) {
			// Delete the program if the shared ELF is unwanted
			unlink(filename.c_str());
		}
	}
	if constexpr (BINTR_TIMING) {
		TIME_POINT(t12);
		printf(">> Binary translation totals %.2f ms\n", nanodiff(t0, t12) / 1e6);
	}
}

template <int W>
void CPU<W>::activate_dylib(DecodedExecuteSegment<W>& exec, void* dylib) const
{
	TIME_POINT(t11);
	// map the API callback table
	auto* ptr = dylib_lookup(dylib, "init");
	if (ptr == nullptr) {
		if constexpr (!libtcc_enabled) {
			// only warn when translation is not already disabled
			if (getenv("NO_TRANSLATE") == nullptr) {
				fprintf(stderr, "libriscv: Could not find dylib init function\n");
			}
		}
		dylib_close(dylib);
		exec.set_binary_translated(nullptr);
		return;
	}

	auto func = (void (*)(const CallbackTable<W>&, void*, uint64_t*, uint64_t*)) ptr;
	func(CallbackTable<W>{
		.mem_read = [] (CPU<W>& cpu, address_type<W> addr) -> const void* {
			return cpu.machine().memory.cached_readable_page(addr << 12, 1).buffer8.data();
		},
		.mem_write = [] (CPU<W>& cpu, address_type<W> addr) -> void* {
			return cpu.machine().memory.cached_writable_page(addr << 12).buffer8.data();
		},
		.vec_load = [] (CPU<W>& cpu, int vd, address_type<W> addr) {
#ifdef RISCV_EXT_VECTOR
			auto& rvv = cpu.registers().rvv();
			rvv.get(vd) = cpu.machine().memory.template read<VectorLane> (addr);
#else
			(void)cpu; (void)addr; (void)vd;
#endif
		},
		.vec_store = [] (CPU<W>& cpu, address_type<W> addr, int vd) {
#ifdef RISCV_EXT_VECTOR
			auto& rvv = cpu.registers().rvv();
			cpu.machine().memory.template write<VectorLane> (addr, rvv.get(vd));
#else
			(void)cpu; (void)addr; (void)vd;
#endif
		},
		.syscalls = m_machine.syscall_handlers.data(),
		.unknown_syscall = [] (CPU<W>& cpu, address_type<W> sysno) {
			cpu.machine().on_unhandled_syscall(cpu.machine(), sysno);
		},
		.ebreak = [] (CPU<W>& cpu) {
			cpu.machine().ebreak();
		},
		.system = [] (CPU<W>& cpu, uint32_t instr) {
			cpu.machine().system(rv32i_instruction{instr});
		},
		.execute = [] (CPU<W>& cpu, uint32_t instr) {
			const rv32i_instruction rvi{instr};
			cpu.decode(rvi).handler(cpu, rvi);
		},
		.trigger_exception = [] (CPU<W>& cpu, int e) {
			cpu.trigger_exception(e);
		},
		.sqrtf32 = [] (float f) -> float {
			return std::sqrt(f);
		},
		.sqrtf64 = [] (double d) -> double {
			return std::sqrt(d);
		},
	},
	m_machine.memory.memory_arena_ptr(),
	&m_machine.get_counters().first,
	&m_machine.get_counters().second);

	// Map all the functions to instruction handlers
	uint32_t* no_mappings = (uint32_t *)dylib_lookup(dylib, "no_mappings");
	struct Mapping {
		address_t addr;
		instruction_handler<W> handler;
	};
	Mapping* mappings = (Mapping *)dylib_lookup(dylib, "mappings");

	if (no_mappings == nullptr || mappings == nullptr) {
		dylib_close(dylib);
		exec.set_binary_translated(nullptr);
		throw MachineException(INVALID_PROGRAM, "Invalid mappings in binary translation program");
	}

	// After this, we should automatically close the dylib on destruction
	exec.set_binary_translated(dylib);

	// Apply mappings to decoder cache
	const auto nmappings = *no_mappings;
	exec.reserve_mappings(nmappings);
	for (size_t i = 0; i < nmappings; i++) {
		exec.add_mapping(mappings[i].handler);
		auto& entry = decoder_entry_at(exec, mappings[i].addr);
		if (mappings[i].handler != nullptr) {
			entry.instr = i;
			entry.set_bytecode(CPU<W>::computed_index_for(RV32_INSTR_BLOCK_END));
		} else {
			entry.set_bytecode(0x0); /* Invalid opcode */
		}
	}

	if constexpr (BINTR_TIMING) {
		TIME_POINT(t12);
		printf(">> Binary translation activation %ld ns\n", nanodiff(t11, t12));
	}
}

	template void CPU<4>::try_translate(const MachineOptions<4>&, const std::string&, DecodedExecuteSegment<4>&, address_t, std::vector<TransInstr<4>>) const;
	template void CPU<8>::try_translate(const MachineOptions<8>&, const std::string&, DecodedExecuteSegment<8>&, address_t, std::vector<TransInstr<8>>) const;
	template int CPU<4>::load_translation(const MachineOptions<4>&, std::string*, DecodedExecuteSegment<4>&) const;
	template int CPU<8>::load_translation(const MachineOptions<8>&, std::string*, DecodedExecuteSegment<8>&) const;
	template void CPU<4>::activate_dylib(DecodedExecuteSegment<4>&, void*) const;
	template void CPU<8>::activate_dylib(DecodedExecuteSegment<8>&, void*) const;

	static_assert(!compressed_enabled,
		"C-extension incompatible with binary translation");

	timespec time_now()
	{
		timespec t;
		clock_gettime(CLOCK_MONOTONIC, &t);
		return t;
	}
	long nanodiff(timespec start_time, timespec end_time)
	{
		return (end_time.tv_sec - start_time.tv_sec) * (long)1e9 + (end_time.tv_nsec - start_time.tv_nsec);
	}
} // riscv
