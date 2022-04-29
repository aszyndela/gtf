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
#ifndef SCENARIONAMEMANAGEMENT_H
#define SCENARIONAMEMANAGEMENT_H

#include "../lib/testlib.h"

namespace ScenarioNameManagement {

/*
 * Test scenario specifications:
 * - bus well-known name for testing;
 * - number of iterations of executing operations;
 * - number of iterations of waiting for signals.
 * Those numbers may differ, because operations may be composite operations yielding
 * more than one signal per operation set.
 */
struct TestSpec
{
	testlib::Name name;

	explicit TestSpec(testlib::Name name_)
		: name(name_), iterations_ops_(0), iterations_signals_(0), separate_(false)
	{}
	unsigned iterations_ops() { return iterations_ops_; }
	auto &iterations_ops(unsigned v)
	{
		iterations_ops_ = v;
		if (!separate_)
			iterations_signals_ = iterations_ops_ * 2;
		return *this;
	}
	unsigned iterations_signals() { return iterations_signals_; }
	auto &iterations_signals(unsigned v)
	{
		iterations_signals_ = v;
		separate_ = true;
		return *this;
	}
private:
	unsigned iterations_ops_;
	unsigned iterations_signals_;
	bool separate_;
};

/*
 *	Generic process:
 *	0. Receiver setups itself for listening to name events
 *	1. NameOwner and Receiver synchronize to both know that they are ready
 *	2. NameOwner starts operations with well-known name used for tests
 *	3. Receiver receives name signals
 *	4. After specified sequence NameOwner and Receiver terminate
 *
 *	Parameters:
 *	- well-known name used for testing
 *	- number of iterations of name operations.
 */

/* Configuration of NameOwner:
 *	Synchronizer is responsible for waiting for Receiver
 *		void synchronizeService(testlib::Name name)
 *			finishes when Receiver is ready
 *	NameOperation executes operation on a name - may be composite
 *		Setup prepare(testlib::Name name)
 *			prepares for executing operation
 *		void execute(Setup setup)
 *			executes operation
 *		void finish()
 *			cleans up after operations
 */
template <typename Synchronizer, typename NameOperation>
class NameOwner : private TestSpec, Synchronizer, NameOperation
{
public:
	explicit NameOwner(const TestSpec &spec)
		: TestSpec(spec)
	{
		tinfo("NameOwner()");
	}
	void operator()()
	{
		// prepare connections, filters, etc.
		NameOperation::prepare(name);
		// wait for client ro be ready
		Synchronizer::synchronizeService(name);
		// do something for desired number of times
		TIMES(iterations_ops()) {
			NameOperation::execute();
		}
		// done
		NameOperation::finish();
	}
	~NameOwner() { tinfo("~NameOwner()"); }
};

/* Configuration of Receiver:
 *	Synchronizer is responsible for waiting for NameOwner and signaling its readiness to
 *	the NameOwner's Synchronizer
 *		void synchronizeClient(testlib::Name name)
 *			finishes when NameOwner is ready
 *	Waiter is responsible for receiving name signals
 *		void setup(testlib::Name name)
 *			prepares Receiver for receiving desired signals - it opens connections,
 *						 registers filters, etc.
 *		void expectSignals()
 *			waits for desired sequence of name signals, e.g. two NameOwnerChanged
 *		void tearDown()
 *			closes connections, performs cleanup
 */
template <typename Synchronizer, typename Waiter>
class Receiver : private TestSpec, Synchronizer, Waiter
{
public:
	explicit Receiver(const TestSpec &spec)
		: TestSpec(spec)
	{
		tinfo("Receiver()");
	}
	void operator()()
	{
		// prepares connections, filters, etc.
		Waiter::setup(name);
		// wait for name owner to be ready, and let him know that we're ready
		Synchronizer::synchronizeClient(name);
		// wait for desired number of name signals
		TIMES(iterations_signals()) {
			Waiter::expectSignals();
		}
		// done
		Waiter::tearDown();
	}
	~Receiver() { tinfo("~Receiver()"); }
};

}

#endif // SCENARIONAMEMANAGEMENT_H
