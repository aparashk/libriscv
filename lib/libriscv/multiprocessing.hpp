#pragma once
#include "util/threadpool.h"

namespace riscv {

template <int W>
struct Multiprocessing
{
#ifdef RISCV_MULTIPROCESS
	void async_work(std::function<void()> wrk);
	void wait();
	bool is_multiprocessing() const noexcept { return this->processing; }

	ThreadPool m_threadpool { multiprocessing_threads };
	std::mutex m_lock;
	bool processing = false;
	static constexpr bool shared_page_faults = false;
	static constexpr bool shared_read_faults = false;
	static constexpr size_t multiprocessing_threads = 4;
#else
	bool is_multiprocessing() const noexcept { return false; }
#endif
};

} // riscv
