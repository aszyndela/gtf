/*
 * Copyright (c) 2017 - 2022 Samsung Electronics Co., Ltd. All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#ifndef TESTLIB_H
#define TESTLIB_H

#include "sugar.h"
#include "pos_ind.h"

#include <cstdlib>
#include <iostream>
#include <sys/wait.h>
#include <unistd.h>
#include <chrono>
#include <vector>
#include <sstream>

namespace testlib {

template <typename ...Args>
void log_to_stream(std::ostream &stream, const Args &...args) {
	std::initializer_list<int>{((void)(stream << args), 0)...};
}

template <typename ...Args>
void tlog(const Args &...args) { log_to_stream(std::cerr, args...); }

template <typename ...Args>
void print_result(const Args &...args) { log_to_stream(std::cout, args..., "\n"); }

std::ostringstream os;
template <typename ...Args>
void print_fail(const Args &...args) { log_to_stream(os, args..., "\n"); }

enum class CERRNOMSG {ERRNOMSG};
#define ERRNOMSG testlib::CERRNOMSG::ERRNOMSG

NO_TEMPLATE void operator<< (std::ostream &stream, const CERRNOMSG)
{
	char buf[100];
	std::cout << strerror_r(errno, buf, sizeof(buf));
}

#define logheader(...) testlib::tlog("[",std::chrono::system_clock::now().time_since_epoch().count()%1000000000,",",getpid(),",",__FILE__, ":", __LINE__,",",__FUNCTION__,"()", "" __VA_ARGS__, "] ")

#define logl(...) testlib::tlog(__VA_ARGS__, "\n")
#define tinfo(...) do { logheader(""); logl(__VA_ARGS__); } while(0)
#define tfail(...) do { logheader("ERROR: "); logl(__VA_ARGS__); assert(0); } while(0)
#define tassert(COND) do { if (!(COND)) tfail("assertion failed (", #COND, ")"); } while (0)

struct WaitStatus {
	constexpr WaitStatus(int d_) : d(d_) {}
	constexpr bool ok() const { return !failed(); }
	constexpr bool failed() const { return !WIFEXITED(d) || WEXITSTATUS(d); }
	constexpr bool exited() const { return WIFEXITED(d); }
private:
	int d;
};

NO_TEMPLATE wur auto wait(pid_t &p) {
	int status;
	if (0 > (p = ::wait(&status)))
		tfail("wait(", ERRNOMSG, ")");
	return WaitStatus(status);
}
NO_TEMPLATE wur auto wait() { pid_t dummy; return wait(dummy); }

D_STRINGTYPE(Name)
D_STRINGTYPE(Path)
D_STRINGTYPE(Iface)
D_STRINGTYPE(Member)
D_STRINGTYPE(Arg0)

struct TestSpec {
	TestSpec(const Name &name_,
			const Path &path_,
			const Iface &iface_,
			const Member &member_,
			unsigned threads = 1,
			unsigned msgs = 1,
			uint64_t payload_size = 256)
	: name(name_), path(path_), iface(iface_), member(member_),
		_threads(threads), _msgs(msgs), _payload_size(payload_size), _no_auto_start(false)
	{}
#define D_GS(m) TestSpec &m(auto val) { _##m = val; return *this; } \
				auto m() const { return _##m; }
	D_GS(threads);
	D_GS(msgs);
	D_GS(payload_size);
	D_GS(no_auto_start);
	D_GS(arg0);
#undef D_GS

	const Name &name;
	const Path &path;
	const Iface &iface;
	const Member &member;
private:
	unsigned _threads;
	unsigned _msgs;
	uint64_t _payload_size;
	bool _no_auto_start;
	Arg0 _arg0;
};

typedef void (*TestFunc)();
typedef void (*TestSpecFunc)(const TestSpec &spec);
template <typename Func>
struct ATestT {
	std::chrono::seconds timeout;
	std::vector<Func> t;
	std::vector<Func> t_to_kill;
};
typedef ATestT<TestFunc> ATest;
typedef ATestT<TestSpecFunc> ATestS;

} // namespace testlib

#endif // TESTLIB_H
