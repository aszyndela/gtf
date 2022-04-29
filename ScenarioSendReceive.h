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
#ifndef TESTSENDRECEIVE_H
#define TESTSENDRECEIVE_H

#include <chrono>
#include <future>
#include <thread>
#include <algorithm>

#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>

#include <assert.h>

#include "../lib/testlib.h"

namespace ScenarioSendReceive {
/*

   Generic process:
   0. Setup - Receiver registers itself under well-known name, or registers filters;
   				Sender has to wait for setup to be finished
   1. Sender sends message(s) to well-known name
   2. Receiver receives message(s)
		   (opt) 3. Receiver checks validity of the message
   4. Receiver sends a reply, if needed
   5. Sender receives the reply, if needed
		   (opt) 6. Sender checks if the reply is correct, if needed
   */

/*
 * Potential parameters to the test:
 * - number of sending threads
 * - number of sent messages per thread
 * - size of sent payload in each message
 */

/*
 * Configuration of Sender:
 * Setup performs all needed actions before Sender is ready to start test. Methods:
 *		auto Setup::setup(Name name)
 *			creates connection, waits for name to appear on the bus
 *			returns setup object
 *		void tearDown()
 *			closes connections, performs clean up
 * SendMethod is responsible for sending messages. Methods:
 *		void prepareForSending(setup, testlib::TestSpec &spec)
 *			takes what it needs for further work from setup and spec
 *		void send(message)
 *			executes specified method call (sends message)
 * MessageFeeder is responsible for messages creation and disposal. Methods:
 *		auto next(const testlib::TestSpec &spec)
 *			creates a message which will be sent next
 *			returns the message
 *		void dispose(message)
 *			gets rid of no longer needed message
 */
template <typename Setup,
		 typename SendMethod,
		 typename MessageFeeder>
class Sender : private testlib::TestSpec, Setup, MessageFeeder
{
public:
	explicit Sender(const TestSpec &spec)
		: TestSpec(spec)
	{
		MessageFeeder::payload_size(spec.payload_size());
		tinfo("Sender()");
	}
	void operator()() {
		// Client side.
		// The test starts with creating necessary setup: connections, requesting names, etc.
		tinfo("INITIAL PHASE: Setup the test");
		auto setup = Setup::setup(name);
		std::vector<std::thread *> ths;
		ths.reserve(threads());
		tinfo("MAIN PHASE: Run");
		// launch all the needed threads
		TIMES(threads()) {
			ths.push_back(new std::thread([&] {
									SendMethod sendMethod;
						// in each thread send proper number of messages from each thread
									// first - prepare, e.g. create any additional needed objects
									sendMethod.prepareForSending(setup, *this);
									FORZ(m, msgs()) {
										// create each message
										auto msg = MessageFeeder::next(*this);
										// send the message
										sendMethod.send(msg);
										// free the message
										MessageFeeder::dispose(msg);
									}
									// finish - e.g. wait for any asynchronous operations
									// it should be done in sendMethod's destructor
								})
							);
		}
		// wait for all the threads
		std::for_each(ths.begin(), ths.end(), [] (std::thread *t) {
										t->join();
										delete t;
									});
		tinfo("FINAL PHASE: all threads finished, clean up");
		// clean up - disconnect, etc.
		Setup::tearDown();
	}
	~Sender() { tinfo("~Sender()"); }
};

/*
 * Configuration of Receiver:
 * Service is responsible for configuring bus name and handling incoming messages. Methods:
 * 		auto setup(testlib::TestSpec &spec, int served_messages = 1)
 *			configures bus name and object with spec data
 *			additionally sets expected number of messages to handle - for logging purposes
 *		void tearDown()
 *			releases bus name, unregisters objects, cleans up
 * Dispatcher is responsible for ensuring waiting for correct number of messages. Methods:
 *		unsigned dispatchOnce(setup)
 *			executes waiting for incoming messages, and handling them
 *			returns number of handled messages
 */
template <typename Service, typename Dispatcher>
class Receiver : private testlib::TestSpec, Service, Dispatcher
{
public:
	using TestSpec::TestSpec;
	explicit Receiver(const TestSpec &spec) : TestSpec(spec)
	{}
	void operator()() {
		// Server side
		// Total number of expected messages is number of sender threads multiplied
		// by number of messages per thread
		unsigned k = TestSpec::threads() * TestSpec::msgs();
		// prepare service
		auto &setup = Service::setup(*this, k);
		// receive in loop, until all are received
		while (k > 0) {
			// not all implementations may be able to dispatch exactly one message
			// thus - we need to account how many messages were already received
			k -= Dispatcher::dispatchOnce(setup);
		}
		// done - clean up
		Service::tearDown();
	}
	~Receiver() { tinfo("~Receiver()"); }
};

} // namespace ScenarioSendReceive

// This is temporary - defines a dummy class with given name and methods
// which accept everything, and just log that they were called
#define D_C(name, meth1, meth2) template <class ...Args> class name : public Args... \
{\
	public:\
		bool meth1() {\
			tinfo(#name"::"#meth1"()");\
			return true;\
		}\
		template <typename T> \
		bool meth1(T t, ...) {\
			tinfo(#name"::"#meth1"(T)");\
			return true;\
		}\
		bool meth2() {\
			tinfo(#name"::"#meth2"");\
			return false;\
		}\
		template <typename T> \
		bool meth2(T t, ...) {\
			tinfo(#name"::"#meth2"(T)");\
			return false;\
		}\
		~name() { tinfo("~"#name""); }\
};

D_C(DummySynchronizer, synchronizeService, synchronizeClient)
D_C(DummyMessageFeeder, dispose, next)
D_C(DummyService, setup, tearDown)
D_C(DummyDispatcher, dispatch, dummy)

#endif // TESTSENDRECEIVE_H
