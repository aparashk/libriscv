
template <int W>
template <typename... Args> constexpr
inline void Machine<W>::setup_call(Args&&... args)
{
	cpu.reg(REG_RA) = memory.exit_address();
	[[maybe_unused]] int iarg = REG_ARG0;
	[[maybe_unused]] int farg = REG_FA0;
	([&] {
		if constexpr (std::is_integral_v<remove_cvref<Args>>) {
			cpu.reg(iarg++) = args;
			if constexpr (sizeof(Args) > W) // upper 32-bits for 64-bit integers
				cpu.reg(iarg++) = args >> 32;
		}
		else if constexpr (is_stdstring<remove_cvref<Args>>::value)
			cpu.reg(iarg++) = stack_push(args.data(), args.size()+1);
		else if constexpr (is_string<Args>::value)
			cpu.reg(iarg++) = stack_push(args, strlen(args)+1);
		else if constexpr (std::is_floating_point_v<remove_cvref<Args>>)
			cpu.registers().getfl(farg++).set_float(args);
		else if constexpr (std::is_standard_layout_v<remove_cvref<Args>>)
			cpu.reg(iarg++) = stack_push(&args, sizeof(args));
		else
			static_assert(always_false<decltype(args)>, "Unknown type");
	}(), ...);
	cpu.reg(REG_SP) &= ~address_t(0xF);
}

template <int W>
template <uint64_t MAXI, bool Throw, typename... Args> constexpr
inline address_type<W> Machine<W>::vmcall(address_t pc, Args&&... args)
{
	// reset the stack pointer to an initial location (deliberately)
	this->cpu.reset_stack_pointer();
	// setup calling convention
	this->setup_call(std::forward<Args>(args)...);
	// execute guest function
	this->simulate_with<Throw>(MAXI, 0u, pc);

	// address-sized integer return value
	return cpu.reg(REG_ARG0);
}

template <int W>
template <uint64_t MAXI, bool Throw, typename... Args> constexpr
inline address_type<W> Machine<W>::vmcall(const char* funcname, Args&&... args)
{
	address_t call_addr = memory.resolve_address(funcname);
	return vmcall<MAXI, Throw>(call_addr, std::forward<Args>(args)...);
}

template <int W>
template <bool Throw, bool StoreRegs, typename... Args> inline
address_type<W> Machine<W>::preempt(uint64_t max_instr, address_t call_addr, Args&&... args)
{
	Registers<W> regs;
	if constexpr (StoreRegs) {
		regs = cpu.registers();
	}
	const auto prev_max = this->max_instructions();
	// we need to make some stack room
	this->cpu.reg(REG_SP) -= 16u;
	// setup calling convention
	this->setup_call(std::forward<Args>(args)...);
	try {
		// execute by extending the max instruction counter (resuming)
		this->simulate_with(
			this->instruction_counter() + max_instr, this->instruction_counter(), call_addr);
	} catch (...) {
		this->set_max_instructions(prev_max);
		if constexpr (StoreRegs) {
			cpu.registers() = regs;
		}
		throw;
	}
	// restore registers and return value
	this->set_max_instructions(prev_max);
	const auto retval = cpu.reg(REG_ARG0);
	if constexpr (StoreRegs) {
		cpu.registers() = regs;
	}
	return retval;
}

template <int W>
template <bool Throw, bool StoreRegs, typename... Args> inline
address_type<W> Machine<W>::preempt(uint64_t max_instr, const char* funcname, Args&&... args)
{
	address_t call_addr = memory.resolve_address(funcname);
	return preempt<Throw, StoreRegs>(max_instr, call_addr, std::forward<Args>(args)...);
}
