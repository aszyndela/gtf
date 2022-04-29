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
#include "ScenarioSendReceive.h"
#include "ScenarioNameManagement.h"
#include "ImplGdbus.h"
#include "ImplDbus.h"
#include "../lib/testlib.h"
#include <chrono>
#include <iostream>
#include <sys/prctl.h>
#include <unordered_set>

using namespace testlib;

struct TestResult {
	enum {PASS, FAIL, TIMEOUT} result;
	std::chrono::nanoseconds duration;
};

typedef std::unordered_set<std::string> TestsToRun;

/********* A few helper functions *************/
// This causes children to die when parent dies.
NO_TEMPLATE void deathSig() { (void)!prctl(PR_SET_PDEATHSIG, SIGKILL); }

// A helper forking-function
template <class T, class ...Args> auto frk(T &&f, const Args &...args) {
	auto p = fork();
	if (p < 0)
		tfail("fork(", ERRNOMSG, ")");
	if (!p) {
		deathSig();
		std::forward<T>(f)(args...);
		exit(0);
	}
	return p;
}

template <typename Funcs, typename ...Args>
void gatherPids(std::vector<pid_t> &pids, const Funcs &test, const Args & ...spec)
{
	for (auto t: test)
		pids.push_back(frk(t, spec...));
}

NO_TEMPLATE wur TestResult waitForChildren(auto &timestart,
		auto &timepoint,
		const std::vector<pid_t> &pids,
		const std::vector<pid_t> &pids_to_kill)
{
	TestResult result{.result = TestResult::FAIL};

	std::promise<bool> promise;
	auto future = promise.get_future();

	unsigned components = pids.size();
	std::thread task_th([&components](decltype(promise) promise) {
				bool result = true;
				TIMES(components) {
					auto ret = wait();
					if (result && ret.failed()) {
						promise.set_value(false);
						result = false;
					}
				}
				if (result) promise.set_value(true);
			}, std::move(promise));

	if (future.wait_until(timepoint) != std::future_status::ready)
		result.result = TestResult::TIMEOUT;
	else if (future.get())
		result.result = TestResult::PASS;

	result.duration = std::chrono::system_clock::now() - timestart;

	FORZ(p, components)
		kill(pids[p], SIGKILL);	// FIXME - pids may be re-used already, do we kill an innocent process then?
	task_th.join();

	// at this point there are two possibilities:
	// 1. all was normal, and we end only with persistent processes to kill
	// 2. one of processes to kill ended prematurely with an error
	// so, we need to kill the rest and wait for them, it seems we don't even need to check for errors

	// kill the remaining processes, if there are any - those are services that do not end by themselves
	for (auto p : pids_to_kill) {
		kill(p, SIGKILL);
	}
	TIMES(pids_to_kill.size()) {
		auto ret = wait();
		if (ret.failed()) {
			tinfo("wait failed");
		}
	}

	return result;
}

// This function takes a single test as its parameter, and goes through
// components, running each component in separate process, passing all the arguments needed.
// After running components, it spawns a timer thread, which waits for all processes to finish.
template <typename Test, typename ...Args>
TestResult run(const Test &test, const Args & ...spec)
{
	// Compute the time point of timeout
	auto timestart = std::chrono::system_clock::now();
	auto timepoint = timestart + test.timeout;

	// Go!
	std::vector<pid_t> pids;
	pids.reserve(test.t.size());
	gatherPids(pids, test.t, spec...);

	std::vector<pid_t> pids_to_kill;
	pids_to_kill.reserve(test.t_to_kill.size());
	gatherPids(pids_to_kill, test.t_to_kill, spec...);

	// And now, wait for children
	return waitForChildren(timestart, timepoint, pids, pids_to_kill);
}

void operator<< (std::ostream &stream, TestResult r) {
	assert(r.result >= TestResult::PASS && r.result <= TestResult::TIMEOUT);
	const char *str[] = {"PASS;", "FAIL;", "FAIL;TIMEOUT;"};
	stream << str[r.result] << std::chrono::duration<double, std::milli>(r.duration).count();
}

// A bunch of administrative globals for selection (by user) and printing description
// of the tests.
unsigned cnt = 1;
unsigned cnt_fail = 0;
TestsToRun testsToRun;
bool getNameOnly = false;
const char *testPrefix = nullptr;
const char *testDesc = nullptr;
std::string testDetails;
std::string testImpl;
#define ANNOUNCE_TEST_DETAILS(msg) do { testDetails = std::string(" (") + msg + ")"; } while (0)
#define ANNOUNCE_IMPL(msg) do { testImpl = std::string(" [") + msg + "]"; } while (0)

template <typename Test, typename ...Args>
void runAndPrintSingleTest(const Test &test,
		const Args & ...spec)
{
	std::string name = std::string(testPrefix?testPrefix:"") + std::to_string(cnt);
	// check if we should touch this test
	if (testsToRun.size() == 0 || testsToRun.find(name) != testsToRun.end() ||
			(testPrefix && testsToRun.find(testPrefix) != testsToRun.end()))
	{
		// run or get the name?
		if (!getNameOnly) {
			tinfo("Running test ", cnt);
			auto ret = run(test, spec...);
			print_result(name, ";", ret);
			if (ret.result != TestResult::PASS) {
				print_fail(name, ";", ret);
				++cnt_fail;
			}
		} else {
			print_result(name, ";", testDesc?testDesc:"", testDetails, testImpl);
		}
	}
	++cnt;
}

#define DEF_TIMEOUT std::chrono::seconds(4)

// Components is a pair of two lists.
template <typename T>
using Components = std::pair<std::vector<std::pair<T, std::string>>, std::vector<std::pair<T, std::string>>>;

// These functions iterate over two lists from components
// and compose a test from each element from the first list paired with
// each element from the second list. Then it runs such a test with
// given specification and timeout.
template <typename T, class TestSpec>
void runCombined(const Components<T> &&components, const TestSpec &spec,
		std::chrono::seconds &&timeout = DEF_TIMEOUT)
{
	for (auto s : components.first) {
		for (auto r : components.second) {
			ATestT<T> test = {timeout, {s.first, r.first}};
			ANNOUNCE_IMPL(std::string("S:") + s.second + "+R:" + r.second);
			runAndPrintSingleTest(test, spec);
		}
	}
	ANNOUNCE_IMPL("");
}

template <typename T>
void runCombined(const Components<T> &&components,
		std::chrono::seconds &&timeout = DEF_TIMEOUT)
{
	for (auto s : components.first) {
		for (auto r : components.second) {
			ATestT<T> test = {timeout, {s, r}};
			ANNOUNCE_IMPL(std::string("S:") + s.second + "+R:" + r.second);
			runAndPrintSingleTest(test);
		}
	}
	ANNOUNCE_IMPL("");
}

std::string guessImplString(const std::string &str)
{
	const char *dbusStr = "ImplDbus";
	const char *gdbusStr = "ImplGdbus";
	std::map<std::string, size_t> counter;
	counter[dbusStr] = 0;
	counter[gdbusStr] = 0;
	for (auto &v: counter) {
		std::size_t found=0; // we can ignore first byte - Impls are further in the string
		do {
			found = str.find(v.first.c_str(), found+1);
			if (found != std::string::npos)
				v.second++;
		} while (found != std::string::npos);
	}
	return counter[dbusStr] > counter[gdbusStr] ? dbusStr : gdbusStr;
}

std::string getUnownedUniqueId()
{
	return getNameOnly ? "" : ImplDbus::getUnownedUniqueId();
}

/****** TEST DEFINITIONS *********/
// Each test consists of timeout, and some components, meant to run in separate processes
// Running is performed by executing operator() on a given object.
// For example, a single test might be:
// { 1, SomeClass(param1), SomeOtherClass(param2) }
// It means, there will be two processes, which will run as follows:
// 1. SomeClass::operator()
// 2. SomeOtherClass::operator()
// These processes must exit in 1 second to consider the test successful
//
// Another example: the test might be:
// { 1, [] { SomeClass(param1)(); }, [] { SomeOtherClass(param2)(); } }
// It means, there will be two processes, and each will run the above lambda functions,
// which in turn constructs objects and runs operator().
// The difference from the first example is that objects will be constructed in forked processes.
//
// Macros COMPONENT and COMPONENT_SPEC are provided for convenience - they wrap object definition into lambda functions.
// Additionally, they provide implementation part of test description.
// COMPONENT takes objects with parameterless constructor.
// COMPONENT_SPEC takes objects with constructor with single parameter of type testlib::TestSpec
#define COMPONENT(...) {[] { __VA_ARGS__(); }, guessImplString(#__VA_ARGS__)}
#define COMPONENT_NODESC(...) [] { __VA_ARGS__(); }
#define COMPONENT_SPEC(...) {[] (const testlib::TestSpec &spec) { ((__VA_ARGS__)(spec))(); }, guessImplString(#__VA_ARGS__)}
#define COMPONENT_SPEC_ADD_DESC(desc, ...) {[] (const testlib::TestSpec &spec) { ((__VA_ARGS__)(spec))(); }, guessImplString(#__VA_ARGS__) + " " + desc}
typedef std::pair<TestFunc, std::string> Component;
typedef std::pair<TestSpecFunc, std::string> ComponentSpec;

// CONCEPT TESTING
// A concept is like a very general scenario: there is a sender and a receiver, and they send themselves method calls.
// 		There are two actors: sender and receiver. If we have multiple implementations for each, then we may
//		run each sender paired with each receiver.

// CONCEPT1 : there is a sender and a receiver, and they send themselves messages.
//
// CONCEPT1 case1: a sender sends method calls (one method) to a receiver
// This concept is parameterized by member specification, number of messages, number of threads, and payload size
// This function generates components for the concept
auto senderReceiverMethodCallConcept()
{
	return Components<TestSpecFunc> {
		// senders
		{
			COMPONENT_SPEC(	ScenarioSendReceive::Sender<
								ImplGdbus::SetupWaitForName<>,
								ImplGdbus::SimpleProxySyncSendMethod<>,
								ImplGdbus::MessageVariantFeeder>),
			COMPONENT_SPEC( ScenarioSendReceive::Sender<
								ImplDbus::SetupWaitForName<>,
								ImplDbus::SimpleSendMethod<>,
								ImplDbus::MessageMethodCallFeeder>),
			COMPONENT_SPEC_ADD_DESC( "async",
								ScenarioSendReceive::Sender<
								ImplGdbus::SetupWaitForName<>,
								ImplGdbus::SimpleProxyAsyncSendMethod<>,
								ImplGdbus::MessageVariantFeeder>),
			COMPONENT_SPEC_ADD_DESC( "private bus",
								ScenarioSendReceive::Sender<
								ImplGdbus::SetupWaitForName<>,
								ImplGdbus::SimpleProxySyncSendMethod
									<ImplGdbus::CheckStringReplyHandlingMethod,
									// In this sender type we open a private bus in each thread.
									// With kdbus each private bus means a new pool.
									// When 100 pools of default 16MB are created, the system runs out of addressable memory,
									// so we decrease the pool size for this test to 1MB.
									ImplGdbus::PrivateBusTypeProxyCreator<
										ImplGdbus::SetPoolSize<1024*1024>>>,
								ImplGdbus::MessageVariantFeeder>),
		},
		// receivers
		{
			COMPONENT_SPEC( ScenarioSendReceive::Receiver<
								ImplGdbus::SingleMethodService<>,
								ImplGdbus::Dispatcher>),
			COMPONENT_SPEC( ScenarioSendReceive::Receiver<
								ImplDbus::SingleMethodService<>,
								ImplDbus::Dispatcher>)
		}
	};
}

// CONCEPT1 case1a: a sender sends method calls (one method) to a receiver, but sets unique id as a destination
// This concept is parameterized by member specification, number of messages, number of threads, and payload size
// This function generates components for the concept
auto senderReceiverMethodCallToUniqueIdConcept()
{
	return Components<TestSpecFunc> {
		// senders
		{
			COMPONENT_SPEC(	ScenarioSendReceive::Sender<
								ImplGdbus::SetupWaitForName<>,
								ImplGdbus::SimpleProxySyncUniqueIdSendMethod<>,
								ImplGdbus::MessageVariantFeeder>),
			COMPONENT_SPEC( ScenarioSendReceive::Sender<
								ImplDbus::SetupWaitForName<>,
								ImplDbus::SimpleSendMethod<>,
								ImplDbus::MessageMethodCallToUniqueIdFeeder>)
		},
		// receivers
		{
			COMPONENT_SPEC( ScenarioSendReceive::Receiver<
								ImplGdbus::SingleMethodService<>,
								ImplGdbus::Dispatcher>),
			COMPONENT_SPEC( ScenarioSendReceive::Receiver<
								ImplDbus::SingleMethodService<>,
								ImplDbus::Dispatcher>)
		}
	};
}

// CONCEPT1 case2: a sender sends a signal, a receiver receives it
// This concept is parameterized by member specification, number of messages, number of threads, and payload size
// This function generates components for the concept
auto senderReceiverSignalConcept()
{
	return Components<TestSpecFunc> {
		// senders
		{
			COMPONENT_SPEC( ScenarioSendReceive::Sender<
								ImplDbus::SetupWaitForName<>,
								ImplDbus::SimpleAsyncSendMethod,
								ImplDbus::MessageSignalFeeder>),
			COMPONENT_SPEC( ScenarioSendReceive::Sender<
								ImplGdbus::SetupWaitForName<>,
								ImplGdbus::SignalSendMethod,
								ImplGdbus::MessageVariantFeeder>)
		},
		// receivers
		{
			COMPONENT_SPEC( ScenarioSendReceive::Receiver<
								ImplDbus::ListenToSignalsService<>,
								ImplDbus::Dispatcher>),
			COMPONENT_SPEC( ScenarioSendReceive::Receiver<
								ImplGdbus::ListenToSignalService<>,
								ImplGdbus::Dispatcher>)
		}
	};
}

// CONCEPT1 case3: a sender sends method calls (one method) to a receiver
// This concept is parameterized by member specification, number of messages, number of threads, and payload size
// This function generates components for the concept
auto senderReceiverMethodCallPayloadConcept()
{
	return Components<TestSpecFunc> {
		// senders
		{
			COMPONENT_SPEC( ScenarioSendReceive::Sender<
								ImplGdbus::SetupWaitForName<>,
								ImplGdbus::SimpleProxySyncSendMethod<>,
								ImplGdbus::MessageVariantPayloadFeeder>),
			COMPONENT_SPEC( ScenarioSendReceive::Sender<
								ImplDbus::SetupWaitForName<>,
								ImplDbus::SimpleSendMethod<>,
								ImplDbus::MessageMethodCallPayloadFeeder>)
		},
		// receivers
		{
			COMPONENT_SPEC( ScenarioSendReceive::Receiver<
								ImplGdbus::SingleMethodService
									<ImplGdbus::GeneratedPayloadIfaceCreator,
									 ImplGdbus::ReactionAnd
										<ImplGdbus::CheckPayloadReaction,
										 ImplGdbus::RespondOnceReaction>>,
								ImplGdbus::Dispatcher>),
			COMPONENT_SPEC( ScenarioSendReceive::Receiver<
								ImplDbus::SingleMethodService
									<ImplDbus::ReactionAnd
										<ImplDbus::CheckPayloadReaction,
										 ImplDbus::RespondTimesSyncReaction>>,
								ImplDbus::Dispatcher>)
		}
	};
}

// CONCEPT1 case4: a sender sends a signal, a receiver receives it
// This concept is parameterized by member specification, number of messages, number of threads, and payload size
// This function generates components for the concept
auto senderReceiverSignalPayloadConcept()
{
	return Components<TestSpecFunc> {
		// senders
		{
			COMPONENT_SPEC( ScenarioSendReceive::Sender<
								ImplDbus::SetupWaitForName<>,
								ImplDbus::SimpleAsyncSendMethod,
								ImplDbus::MessageSignalPayloadFeeder>),
			COMPONENT_SPEC( ScenarioSendReceive::Sender<
								ImplGdbus::SetupWaitForName<>,
								ImplGdbus::SignalSendMethod,
								ImplGdbus::MessageVariantPayloadFeeder>)
		},
		// receivers
		{
			COMPONENT_SPEC( ScenarioSendReceive::Receiver<
								ImplDbus::ListenToSignalsService
									<ImplDbus::ReactionAnd
										<ImplDbus::CheckPayloadReaction,
										 ImplDbus::CountReaction>>,
								ImplDbus::Dispatcher>),
			COMPONENT_SPEC( ScenarioSendReceive::Receiver<
								ImplGdbus::ListenToSignalService
									<ImplGdbus::ReactionAnd
										<ImplGdbus::CheckPayloadReaction,
										 ImplGdbus::JustCountReaction>>,
								ImplGdbus::Dispatcher>)
		}
	};
}

// CONCEPT1 case5: a sender sends method calls (one method) to a receiver, messages include unix file descriptors
// This concept is parameterized by member specification, number of messages, number of threads, and number of file descriptors
// This function generates components for the concept
auto senderReceiverUnixFdMethodCallConcept()
{
	return Components<TestSpecFunc> {
		// senders
		{
			COMPONENT_SPEC( ScenarioSendReceive::Sender<
								ImplGdbus::SetupWaitForName<>,
								ImplGdbus::SimpleProxySyncSendMethod<>,
								ImplGdbus::MessageVariantUnixFdsFeeder>),
			COMPONENT_SPEC( ScenarioSendReceive::Sender<
								ImplDbus::SetupWaitForName<>,
								ImplDbus::SimpleSendMethod<>,
								ImplDbus::MessageMethodCallUnixFDFeeder>)
		},
		// receivers
		{
			COMPONENT_SPEC( ScenarioSendReceive::Receiver<
								ImplGdbus::SingleMethodService
									<ImplGdbus::StringUnixFdsMethodIfaceCreator,
									 ImplGdbus::ReactionAnd
										<ImplGdbus::CheckUnixFdsReaction,
										 ImplGdbus::RespondOnceReaction>>,
								ImplGdbus::Dispatcher>),
			COMPONENT_SPEC( ScenarioSendReceive::Receiver<
								ImplDbus::SingleMethodService
									<ImplDbus::ReactionAnd
										<ImplDbus::CheckUnixFDsReaction,
										 ImplDbus::RespondTimesSyncReaction>>,
								ImplDbus::Dispatcher>)
		}
	};
}

#define STANDARD_NAME(...) Name("com.samsung.TestService" __VA_ARGS__)
#define STANDARD_PATH(...) Path("/com/samsung/Test/MainService" __VA_ARGS__)
#define STANDARD_IFACE(...) Iface("com.samsung.TestService.interface" __VA_ARGS__)
#define STANDARD_MEMBER(...) Member("TestMember" __VA_ARGS__)

#define STANDARD_SIG_SENDER_IFACE(...) Iface("com.samsung.TestService.SignalSender.interface")
#define STANDARD_SIG_RECEIVER_PATH(...) Path("/com/samsung/Test/SignalReceiver" __VA_ARGS__)
#define STANDARD_SIGNAL(...) Member("TestSignal" __VA_ARGS__)

// CONCEPT1 case6: a sender sends messages (method calls, signals) to a receiver,
//					using long names as path, destination, interface, etc.
// This is self-contained test
void checkLongNames()
{
	std::string longname;
	longname.reserve(2048); 	// maximum for bus names, interfaces, members - 255 characters
							// object path has no limits
							// signature is 255 characters long

	auto fill = [] (std::string &s, const char *seed, unsigned targetsize) {
		s = seed;
		for (auto k = s.length(); k < targetsize; ++k) {
			s += 'a' + (k%26);
		}
		return s.c_str();
	};
	// this will make nice test description
	auto make_desc = [] (const char *msg, auto len)
		{ return std::string(msg) + ": " + std::to_string(len); };

	for (auto len : {64, 127, 255}) {
		// check long bus names
		ANNOUNCE_TEST_DETAILS(make_desc("bus name", len));
		runCombined(senderReceiverMethodCallConcept(),
				TestSpec(Name(fill(longname, STANDARD_NAME(".")(), len)),
						 STANDARD_PATH(),
						 STANDARD_IFACE(),
						 STANDARD_MEMBER()));

		// check long interface names
		ANNOUNCE_TEST_DETAILS(make_desc("interface in methods", len));
		runCombined(senderReceiverMethodCallConcept(),
				TestSpec(STANDARD_NAME(),
						 STANDARD_PATH(),
						 Iface(fill(longname, STANDARD_IFACE(".")(), len)),
						 STANDARD_MEMBER()));

		// check long interface names for signals
		ANNOUNCE_TEST_DETAILS(make_desc("interface in signals", len));
		runCombined(senderReceiverSignalConcept(),
				TestSpec(STANDARD_NAME(),
						 STANDARD_SIG_RECEIVER_PATH(),
						 Iface(fill(longname, STANDARD_SIG_SENDER_IFACE(".")(), len)),
						 STANDARD_SIGNAL()));

		// check long member names
		ANNOUNCE_TEST_DETAILS(make_desc("members: methods", len));
		runCombined(senderReceiverMethodCallConcept(),
				TestSpec(STANDARD_NAME(),
						 STANDARD_PATH(),
						 STANDARD_IFACE(),
						 Member(fill(longname, STANDARD_MEMBER()(), len))));

		// check long member names for signals
		ANNOUNCE_TEST_DETAILS(make_desc("members: signals", len));
		runCombined(senderReceiverSignalConcept(),
				TestSpec(STANDARD_NAME(),
						 STANDARD_SIG_RECEIVER_PATH(),
						 STANDARD_SIG_SENDER_IFACE(),
						 Member(fill(longname, STANDARD_SIGNAL()(), len))));
	}

	// check long paths
	for (auto len : {255, 1024, 2047} ) {
		ANNOUNCE_TEST_DETAILS(make_desc("paths in methods", len));
		runCombined(senderReceiverMethodCallConcept(),
				TestSpec(STANDARD_NAME(),
						 Path(fill(longname, STANDARD_PATH("/")(), len)),
						 STANDARD_IFACE(),
						 STANDARD_MEMBER()));
		// check long paths for signals
		ANNOUNCE_TEST_DETAILS(make_desc("paths in signals", len));
		runCombined(senderReceiverSignalConcept(),
				TestSpec(STANDARD_NAME(),
						 Path(fill(longname, STANDARD_SIG_RECEIVER_PATH("/")(), len)),
						 STANDARD_SIG_SENDER_IFACE(),
						 STANDARD_SIGNAL()));
	}
}

// These definitions create convenience names for various sender flavors.
template <GDBusError err>
using GdbusBusErrorChecker = ScenarioSendReceive::Sender<
								ImplGdbus::GetBusTypeSetup<>,
								ImplGdbus::SimpleProxySyncSendMethod
									<ImplGdbus::ExpectDBusErrorReplyHandlingMethod<err>>,
								ImplGdbus::MessageVariantFeeder>;

template <GDBusError err>
using GdbusServiceErrorChecker = ScenarioSendReceive::Sender<
									ImplGdbus::SetupWaitForName<>,
									ImplGdbus::SimpleProxySyncSendMethod
										<ImplGdbus::ExpectDBusErrorReplyHandlingMethod<err>>,
									ImplGdbus::MessageVariantFeeder>;

template <ImplDbus::Errors err>
using DbusBusErrorChecker = ScenarioSendReceive::Sender<
									ImplDbus::GetConnectionSetup<>,
									ImplDbus::SimpleSendMethod
										<ImplDbus::ExpectDBusErrorReplyHandlingMethod<err>>,
									ImplDbus::MessageMethodCallFeeder>;

template <ImplDbus::Errors err>
using DbusServiceErrorChecker = ScenarioSendReceive::Sender<
									ImplDbus::SetupWaitForName<>,
									ImplDbus::SimpleSendMethod
										<ImplDbus::ExpectDBusErrorReplyHandlingMethod<err>>,
									ImplDbus::MessageMethodCallFeeder>;

// CONCEPT1 case7: a sender sends messages (method calls, signals) using non-existent destinations,
//					interfaces, paths, etc. In all cases where receivers appear, they should not
//					receive any message.
// This is self-contained test
void checkErrors()
{
	std::string unownedUniqueId = getUnownedUniqueId();
	// tests for checking that correct error is returned when calling nonexisting service
	// with auto-start
	// NOTE: no service side for this test
	for (auto c: std::vector<ComponentSpec>
				 {
				 	// gdbus
				 	COMPONENT_SPEC(GdbusBusErrorChecker<G_DBUS_ERROR_SERVICE_UNKNOWN>),
					// libdbus
					COMPONENT_SPEC(DbusBusErrorChecker<ImplDbus::Errors::SERVICE_UNKNOWN>),
				 }) {

		ANNOUNCE_IMPL(c.second);
		// take next component for testing, and run the test
		ANNOUNCE_TEST_DETAILS("no destination with auto-start");
		runAndPrintSingleTest(
				ATestS	{DEF_TIMEOUT, {c.first}},
				TestSpec(STANDARD_NAME(".nonexistent"),
					STANDARD_PATH(),
					STANDARD_IFACE(),
					STANDARD_MEMBER())
			);
		ANNOUNCE_TEST_DETAILS("no unique-id destination with auto-start");
		runAndPrintSingleTest(
				ATestS	{DEF_TIMEOUT, {c.first}},
				TestSpec(testlib::Name(unownedUniqueId),
					STANDARD_PATH(),
					STANDARD_IFACE(),
					STANDARD_MEMBER())
			);
	}

	// tests for checking that correct error is returned when calling nonexisting service
	// without auto-start
	// NOTE: no service side for this test
	for (auto c: std::vector<ComponentSpec>
				 {
				 	// gdbus
				 	COMPONENT_SPEC(GdbusBusErrorChecker<G_DBUS_ERROR_NAME_HAS_NO_OWNER>),
					// libdbus
					COMPONENT_SPEC(DbusBusErrorChecker<ImplDbus::Errors::NAME_HAS_NO_OWNER>),
				 }) {

		ANNOUNCE_IMPL(c.second);
		ANNOUNCE_TEST_DETAILS("no destination without auto-start");
		runAndPrintSingleTest(
				ATestS	{DEF_TIMEOUT, {c.first}},
				TestSpec(STANDARD_NAME(".nonexistent"),
					STANDARD_PATH(),
					STANDARD_IFACE(),
					STANDARD_MEMBER()).no_auto_start(true)
			);
		ANNOUNCE_TEST_DETAILS("no unique-id destination with auto-start");
		runAndPrintSingleTest(
				ATestS	{DEF_TIMEOUT, {c.first}},
				TestSpec(testlib::Name(unownedUniqueId),
					STANDARD_PATH(),
					STANDARD_IFACE(),
					STANDARD_MEMBER()).no_auto_start(true)
			);
	}

	// check what happens when calling various non-existent entities
	Component sendersNoIface[] = {
							// checking non-existing interface
							COMPONENT( GdbusServiceErrorChecker<G_DBUS_ERROR_UNKNOWN_METHOD>(
											TestSpec(STANDARD_NAME(),
													 STANDARD_PATH(),
													 STANDARD_IFACE(".nonexistent"),
													 STANDARD_MEMBER()))),
							COMPONENT( DbusServiceErrorChecker<ImplDbus::Errors::UNKNOWN_METHOD>(
											TestSpec(STANDARD_NAME(),
													 STANDARD_PATH(),
													 STANDARD_IFACE(".nonexistent"),
													 STANDARD_MEMBER())))
						};

	Component sendersNoPath[] = {
							// checking non-existing path
							COMPONENT( GdbusServiceErrorChecker<G_DBUS_ERROR_UNKNOWN_METHOD>(
											TestSpec(STANDARD_NAME(),
													 STANDARD_PATH("/Non/Existent"),
													 STANDARD_IFACE(),
													 STANDARD_MEMBER()))),
							COMPONENT( DbusServiceErrorChecker<ImplDbus::Errors::UNKNOWN_METHOD>(
											TestSpec(STANDARD_NAME(),
													 STANDARD_PATH("/Non/Existent"),
													 STANDARD_IFACE(),
													 STANDARD_MEMBER())))
						};

	Component sendersNoMember[] = {
							// checking non-existing member
							COMPONENT( GdbusServiceErrorChecker<G_DBUS_ERROR_UNKNOWN_METHOD>(
											TestSpec(STANDARD_NAME(),
													 STANDARD_PATH(),
													 STANDARD_IFACE(),
													 STANDARD_MEMBER("NonExistent")))),
							COMPONENT( DbusServiceErrorChecker<ImplDbus::Errors::UNKNOWN_METHOD>(
											TestSpec(STANDARD_NAME(),
													 STANDARD_PATH(),
													 STANDARD_IFACE(),
													 STANDARD_MEMBER("NonExistent"))))
						 };
	// These are receivers that register bus names
	// and fail if a method is called.
	// They are meant to be killed. They do not terminate by themselves.
	Component receivers[] = {
							COMPONENT( ScenarioSendReceive::Receiver<
											ImplGdbus::SingleMethodService
												<ImplGdbus::SingleStringMethodIfaceCreator,
												 ImplGdbus::NoMessageAllowedReaction>,
											ImplGdbus::Dispatcher>
											(TestSpec(STANDARD_NAME(),
													  STANDARD_PATH(),
													  STANDARD_IFACE(),
													  STANDARD_MEMBER()))),
							COMPONENT( ScenarioSendReceive::Receiver<
											ImplDbus::SingleMethodService
												<ImplDbus::NoMessageAllowedReaction>,
											ImplDbus::Dispatcher>
											(TestSpec(STANDARD_NAME(),
													  STANDARD_PATH(),
													  STANDARD_IFACE(),
													  STANDARD_MEMBER())))
						 };

	// execute every combination
	auto run = [] (auto &senders, auto &receivers) {
			FORZ(k, TABSIZE(senders)) {
				FORZ(j, TABSIZE(receivers)) {
					// NOTE: receivers may stay alive after each test - thus they go to second list (t_to_kill)
					ATest test = {DEF_TIMEOUT, .t = {senders[k].first}, .t_to_kill = {receivers[j].first}};
					ANNOUNCE_IMPL(std::string("S:") + senders[k].second + "+R:" + receivers[j].second);
					runAndPrintSingleTest(test);
				}
			}
	};

	ANNOUNCE_TEST_DETAILS("no interface");
	run(sendersNoIface, receivers);
	ANNOUNCE_TEST_DETAILS("no path");
	run(sendersNoPath, receivers);
	ANNOUNCE_TEST_DETAILS("no member");
	run(sendersNoMember, receivers);
}

// CONCEPT2 : a client waits for signal NameOwnerChanged sent for a specific name; name owner takes and releases the name.
// This concept is parameterized by well-known name and number of iterations
// This function generates components for the concept
//
// Components in this concept are function objects that take ScenarioNameManagement::TestSpec as their only argument.
typedef void (*NameManagementTestSpecFunc)(const ScenarioNameManagement::TestSpec &spec);
#define COMPONENT_NMSPEC(...) {[] (const ScenarioNameManagement::TestSpec &spec) { ((__VA_ARGS__)(spec))(); }, guessImplString(#__VA_ARGS__)}
auto nameOwnerChangedConcept()
{
	return Components<NameManagementTestSpecFunc> {
		// name owners
		{
				COMPONENT_NMSPEC( ScenarioNameManagement::NameOwner<
									ImplGdbus::SynchronizerConst<>,
									ImplGdbus::NameOperations<
									// operations:
										ImplGdbus::PrivateBus<>,
										// 1. own name
										ImplGdbus::RequestNameOperation<ImplGdbus::ExternalBus>,
										// 2. unown name
										ImplGdbus::ReleaseNameOperation<>>>),
				COMPONENT_NMSPEC( ScenarioNameManagement::NameOwner<
									ImplGdbus::SynchronizerConst<>,
									ImplDbus::NameOperations<
									// operations:
										ImplDbus::PrivateBus<>,
										// 1. own name
										ImplDbus::RequestNameOperation<ImplDbus::ExternalBus>,
										// 2. unown name
										ImplDbus::ReleaseNameOperation<ImplDbus::ExternalBus>>>),
				COMPONENT_NMSPEC( ScenarioNameManagement::NameOwner<
									ImplGdbus::SynchronizerConst<>,
									ImplGdbus::NameOperations<
									//operations:
										ImplGdbus::PrivateBus<>,
										// 1. own name
										ImplGdbus::RequestNameOperation<ImplGdbus::ExternalBus>,
										// 2. second operation is composite operation
										ImplGdbus::NameOperations<
											ImplGdbus::SharedBus<>,
											// 2.1 try to own name, but expect to fail
											ImplGdbus::RequestNameOperation<
												ImplGdbus::ExternalBus,
												ImplCommon::ExpectedFailureNameOperationResult>,
											// 2.2 unown to avoid getting name
											ImplGdbus::ReleaseNameOperation<1>>,
										// 3. unown name
										ImplGdbus::ReleaseNameOperation<0>>>)
		},
		// receivers
		{
				COMPONENT_NMSPEC (ScenarioNameManagement::Receiver<
									ImplGdbus::SynchronizerConst<>,
									ImplGdbus::NameSignalWaiter<
										ImplGdbus::SharedBus<>,
										ImplCommon::NameOwnerChangedSignal>>),
				COMPONENT_NMSPEC (ScenarioNameManagement::Receiver<
									ImplGdbus::SynchronizerConst<>, // we can use gdbus synchronizer in dbus implementation
																	// because it just works and dbus impl would not test anything more
																	// than already tested things
									ImplDbus::NameSignalWaiter<
										ImplDbus::SharedBus<>,
										ImplCommon::NameOwnerChangedSignal>>)
		}
	};
}

int main(int argc, char **argv)
{
	// check params
	for (int i=1; i<argc; ++i) {
		const std::string s = argv[i];
		if (s == "-r") continue;
		if (s == "-l" || s == "--list") {
			getNameOnly = true;
		} else {
			// gather tests
			testsToRun.insert(s);
		}
	}

#define ANNOUNCE(prefix, msg) do { \
									if (!getNameOnly) tinfo("Running ",msg); \
									testPrefix = prefix; \
									testDesc = msg; \
									testDetails.clear(); \
									testImpl.clear(); \
								} while (0)
#define METHOD_CALL_ECHO_SPEC TestSpec(Name("com.samsung.TestService"),\
			Path("/Test1/MainService"),\
			Iface("com.samsung.TestService.interface"),\
			Member("Echo"))
#define SIGNAL_SPEC TestSpec(Name("com.samsung.TestService"),\
			Path("/Test/SignalReceiver"),\
			Iface("com.samsung.TestService.SignalSender.interface"),\
			Member("TestSignal"))

	// go with the tests
	ANNOUNCE("method", "single message tests");
	runCombined(senderReceiverMethodCallConcept(), METHOD_CALL_ECHO_SPEC);

	ANNOUNCE("methoduid", "single message tests directed to unique ids");
	runCombined(senderReceiverMethodCallToUniqueIdConcept(), METHOD_CALL_ECHO_SPEC);

	ANNOUNCE("signal", "single signal tests");
	runCombined(senderReceiverSignalConcept(), SIGNAL_SPEC);

	ANNOUNCE("longnames", "single message tests with long names");
	checkLongNames();

	ANNOUNCE("errors", "single message tests with expected errors");
	checkErrors();

	auto nums = [] (size_t num, const char *msg) {
		return std::to_string(num) + " " + msg + (num != 1 ? "s" : "");
	};

	ANNOUNCE("threads", "multiple threads tests");
	unsigned threads_num[] = {1, 5, 10, 50, 100};
	FORZ(k, TABSIZE(threads_num)) {
		ANNOUNCE_TEST_DETAILS(std::string("method call, ") + nums(threads_num[k], "thread"));
		runCombined(senderReceiverMethodCallConcept(), METHOD_CALL_ECHO_SPEC.threads(threads_num[k]));
		ANNOUNCE_TEST_DETAILS(std::string("signal, ") + nums(threads_num[k], "thread"));
		runCombined(senderReceiverSignalConcept(), SIGNAL_SPEC.threads(threads_num[k]));
	}

	ANNOUNCE("messages", "multiple messages tests");
	unsigned msgs_num[] = {1, 5, 10, 100};
	FORZ(k, TABSIZE(msgs_num)) {
		ANNOUNCE_TEST_DETAILS(std::string("method call, ") + nums(msgs_num[k], "msg"));
		runCombined(senderReceiverMethodCallConcept(), METHOD_CALL_ECHO_SPEC.msgs(msgs_num[k]));
		ANNOUNCE_TEST_DETAILS(std::string("signal, ") + nums(msgs_num[k], "msg"));
		runCombined(senderReceiverSignalConcept(), SIGNAL_SPEC.msgs(msgs_num[k]));
	}

	// special tests: lots of method calls
	unsigned msgs_big_num[] = {1000, 5000, 10000};
	FORZ(k, TABSIZE(msgs_big_num)) {
		ANNOUNCE_TEST_DETAILS(std::string("lots of method calls, ") + nums(msgs_big_num[k], "msg"));
		runCombined(senderReceiverMethodCallConcept(),
				METHOD_CALL_ECHO_SPEC.msgs(msgs_big_num[k]),
				std::chrono::seconds(std::max(4U, msgs_big_num[k] / 250))); // 10000 messages => 40 seconds
	}

	ANNOUNCE("combined", "combined multiple messages/threads tests");
	for (auto threads : threads_num) {
		for (auto msgs : msgs_num) {
			ANNOUNCE_TEST_DETAILS(std::string("method call, ") + nums(msgs, "msg") + ", " + nums(threads, "thread"));
			runCombined(senderReceiverMethodCallConcept(),
					METHOD_CALL_ECHO_SPEC.threads(threads).msgs(msgs),
					std::chrono::seconds(std::max(4U, (threads*msgs) / 400))); // 10000 messages => 25 seconds
		}
		for (auto msgs : msgs_big_num) {
			ANNOUNCE_TEST_DETAILS(std::string("method call, ") + nums(msgs, "msg") + ", " + nums(threads, "thread"));
			runCombined(senderReceiverMethodCallConcept(),
					METHOD_CALL_ECHO_SPEC.threads(threads).msgs(msgs),
					std::chrono::seconds(std::max(4U, (threads*msgs) / 400)));
		}
	}

	ANNOUNCE("combinedSignals", "combined multiple messages/threads tests with signals");
	// signals may be dropped by kdbus if there are too many of them - so, we use less resources...
	// just skip 100 from threads and nums
	FORZ(k, TABSIZE(threads_num)-1) {
		FORZ(j, TABSIZE(msgs_num)-1) {
			ANNOUNCE_TEST_DETAILS(std::string("signal, ") + nums(msgs_num[j], "msg")
					+ ", " + nums(threads_num[k], "thread"));
			runCombined(senderReceiverSignalConcept(),
					SIGNAL_SPEC.threads(threads_num[k]).msgs(msgs_num[j]));
		}
	}

	ANNOUNCE("unixfd", "tests for sending UNIX file descriptors");
	// there is maximum number of inflight fds in a target queue per user
	// KDBUS_CONN_MAX_FDS_PER_USE=16
	// Thus, if we test with multiple threads we must take special care
	for (auto k : {0, 1, 10, 16, 16}) {
		ANNOUNCE_TEST_DETAILS(nums(k, "file descriptor"));
		runCombined(senderReceiverUnixFdMethodCallConcept(),
				METHOD_CALL_ECHO_SPEC.payload_size(k));
		// with multiple messages
		ANNOUNCE_TEST_DETAILS(nums(k, "file descriptor") + ", 100 messages");
		runCombined(senderReceiverUnixFdMethodCallConcept(),
				METHOD_CALL_ECHO_SPEC.payload_size(k).msgs(100));
	}
	ANNOUNCE("unixfdth", "tests for sending UNIX file descriptors from multiple threads");
	// with multiple threads - a few tests with caring to keep inflight fds as little below 16 as we can
	for (unsigned k = 1; k < 15; ++k) {
		ANNOUNCE_TEST_DETAILS(nums(k, "file descriptor") + ", " + nums(16/k, "thread"));
		runCombined(senderReceiverUnixFdMethodCallConcept(),
				METHOD_CALL_ECHO_SPEC.payload_size(k).threads(16/k));
	}

	/*
	 * Dbus-daemon usually limits the size of the messages sent over the system bus to 32MB. (Kdbus does not)
	 * If we set payload to 32MB (0x2000000), then after adding the header it goes over 32MB.
	 * Thus, we try to estimate header size with format dbus1, to make the message size as close to 32MB as possible.
	 * The header should look like this:
	 * [................] 16 standard bytes
	 * [8 1 'g' 0 2 "ay" 0] 8 bytes - signature
	 * [1 1 'o' 0 0x00000012 "/Test1/MainService" 0 + 8-padding] 32 bytes - path
	 * [3 1 's' 0 0x00000004 "Echo" 0 + 8-padding] 16 bytes - member
	 * [2 1 's' 0 0x00000021 "com.samsung.TestService.interface" 0 + 8-padding] 48 bytes - interface
	 * [7 1 's' 0 0x000000?? "sender" 0 + 8-padding] sender - unknown size
	 * [6 1 's' 0 0x000000?? "destination" 0 + 8-padding] destination - unknown size
	 * That gives 120 bytes plus some space for destination, sender and their padding.
	 * Destination can be "com.samsung.TestService" or unique id of the destination. We assume
	 * that the unique id string will be shorter than "com.samsung.TestService".
	 * So, the maximum size would be 8 bytes + 24 bytes of string (no padding needed).
	 * We add the same for the sender.
	 */
	const auto ROUGH_ESTIMATION_OF_SIMPLE_HEADER_SIZE = 8 + 32 + 16 + 48 + 2*(8 + 24);

	ANNOUNCE("payload", "payload sizes tests");
	/* Large payloads need special care with signals. We may send payloads up to 32MB,
	   but only receivers that allocate large enough pool may receive it. With the default pool
	   a receiver may receive signal with max little below ~2.7M payload */
	for (auto payload_size: {
			// some basic payload sizes first
			0, 0x1000, 0x10000, 0x80000, 0x100000, 0x200000,
			// poke around 512K boundary where memfd is being used
			0x80000-1, 0x80000-2, 0x80000-3, 0x80000-4,
			0x80000+1, 0x80000+2, 0x80000+3, 0x80000+4,
			// poke around 2M boundary - it is maximum vector size.
			0x200000-0x10, 0x200000-0xF, 0x200000-0xE, 0x200000-0xD,
			0x200000-0xC,  0x200000-0xB, 0x200000-0xA, 0x200000-0x9,
			0x200000-0x8,  0x200000-0x7, 0x200000-0x6, 0x200000-0x5,
			0x200000-0x4,  0x200000-0x3, 0x200000-0x2, 0x200000-0x1})
	{
		ANNOUNCE_TEST_DETAILS(std::string("method call, ") + nums(payload_size, "byte"));
		runCombined(senderReceiverMethodCallPayloadConcept(),
				METHOD_CALL_ECHO_SPEC.payload_size(payload_size));
		ANNOUNCE_TEST_DETAILS(std::string("signal, ") + nums(payload_size, "byte"));
		runCombined(senderReceiverSignalPayloadConcept(), SIGNAL_SPEC.payload_size(payload_size),
				std::chrono::seconds(25));
	}
	/* Test payloads larger than 2M with method calls only.
	 * Payloads larger than 2M are packed into multiple vectors.
	 * Glib handles char arrays spanning over multiple vectors very inefficiently.
	 */
	for (auto payload_size: {
			0x2B0000,  0x2C0000,  0x300000,  0x400000,  0x800000,
			0xC00000,  0x1000000, 0x1400000, 0x1800000, 0x1c00000,
			// poke around 2M boundary - it is maximum vector size.
			0x200000+0x10, 0x200000+0xF, 0x200000+0xE, 0x200000+0xD,
			0x200000+0xC,  0x200000+0xB, 0x200000+0xA, 0x200000+0x9,
			0x200000+0x8,  0x200000+0x7, 0x200000+0x6, 0x200000+0x5,
			0x200000+0x4,  0x200000+0x3, 0x200000+0x2, 0x200000+0x1,
			// a few more larger payloads that do not fit into a single vector
			0x240000,      0x280000,
			0x290000, 0x2a0000, 0x2a8000,
			// fiddling with larger payloads of non-aligned sizes
			0x280000+0x1,  0x280000+0x2, 0x280000+0x3, 0x280000+0x4,
			// system bus usually has limit 32MB per message, and we need to account the header
			0x2000000 - ROUGH_ESTIMATION_OF_SIMPLE_HEADER_SIZE})
	{
		ANNOUNCE_TEST_DETAILS(std::string("method call, ") + nums(payload_size, "byte"));
		runCombined(senderReceiverMethodCallPayloadConcept(),
				METHOD_CALL_ECHO_SPEC.payload_size(payload_size),
				std::chrono::seconds(std::max(4, payload_size / 1000000))); // 30s for 32MB of payload
	}

	/* TODO: add tests with increased receiver pool and large signal payloads. */

	ANNOUNCE("namemgmt", "tests for checking name management and its signals");
	runCombined(nameOwnerChangedConcept(), ScenarioNameManagement::TestSpec(STANDARD_NAME()).iterations_ops(3));

	ANNOUNCE("custom", "custom tests");
// specification of test with two threads and two messages
#define TEST1_SPEC_M TestSpec(Name("com.samsung.TestService"), Path("/Test1/MainService"), Iface("com.samsung.TestService.interface"), Member("Echo")).threads(2).msgs(2)

	// an array of custom tests - a good place to add a test that does not follow
	// any of the concept tests
	std::pair<ATest, std::string> test[] = {
		{{
			DEF_TIMEOUT,
			{
				COMPONENT_NODESC( ScenarioSendReceive::Sender<
									ImplDbus::SetupWaitForName<>,
									ImplDbus::SimpleSendMethod<>,
									ImplDbus::MessageMethodCallFeeder>(TEST1_SPEC_M) ),
				COMPONENT_NODESC( ScenarioSendReceive::Receiver<
									ImplGdbus::SingleMethodService<>,
									ImplGdbus::Dispatcher>(TEST1_SPEC_M) )
			}
		}, "custom test"}
	};

	FORZ(k, TABSIZE(test)) {
		ANNOUNCE_TEST_DETAILS(test[k].second);
		runAndPrintSingleTest(test[k].first);
	}

	// print result
	std::cout << std::endl;
	std::cout << "Total : " << cnt << std::endl;
	std::cout << "Pass  : " << cnt - cnt_fail << std::endl;
	std::cout << "Fail  : " << cnt_fail << std::endl;
	if (cnt_fail) {
		std::cout << "Fail List" << std::endl;
		std::cout << os.str();
	}

	return 0;
}
