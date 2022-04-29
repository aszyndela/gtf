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
#ifndef IMPL_COMMON_H
#define IMPL_COMMON_H

namespace ImplCommon {

/*
 * Counter is a helper class for all classes that want to count something.
 */
class Counter
{
public:
	Counter() : _counter(0) {}
	void counter(unsigned c) { _counter = c; }
	auto counter() { return _counter; }
	bool finish() { return counter() == 0; }
protected:
	bool updateCounter() { if (_counter > 0) --_counter; return _counter == 0; }
private:
	unsigned _counter;
};

/*
 * **** NameOperation interface for Impls. ****
 * **** For use directly by test scenarios ****
 * NameOperation is responsible for performing an operation on a given bus well-known name. Methods:
 *		Setup prepare(testlib::Name name)
 *			prepares for executing operation
 *		void execute()
 *			executes operation
 *		void finish()
 *			cleans up after operations
 */
/*
 * NameOperations is an implementation of NameOperation interface (see ImplDbus.h and ImplGdbus.h).
 * It is a composite name operation that makes multiple name operations appear as one.
 * It is parameterized by Bus, Setup and Args.
 * Bus must be an implementation of Bus interface.
 * Setup must be a class suitable for passing to Bus.
 * Args must be implementations of NameOperation interface.
 */
template <typename Bus, typename Setup, class ...Args>
class NameOperations : private Args...
{
	Bus bus;
public:
	void prepare(const testlib::Name name, Setup &setup)
	{
		_name = name;
		bus.prepare(setup);

		std::initializer_list<int>{((void)(Args::prepare(name, bus.bus())), 0)...};
	}
	void prepare(const testlib::Name name)
	{
		prepare(name, bus.bus());
	}
	void execute()
	{
		std::initializer_list<int>{((void)(Args::execute()), 0)...};
	}
	void finish()
	{
		std::initializer_list<int>{((void)(Args::finish()), 0)...};
		bus.done();
	}
private:
	testlib::Name _name;
};

/*
 * **** NameOperationResult interface for Impls. ****
 * **** For use by NameOperations ****
 * NameOperationResult is responsible for providing correct reactions on name events.
 * Methods:
 * 		void nameAcquired(name)
 *			react to acquiring name
 *		void nameLost(name)
 *			react to losing name
 */
/*
 * ExpectedSuccessNameOperationResult is an implementation of NameOperationResult interface.
 * It passes if a name is acquired and fails if a name is lost.
 */
class ExpectedSuccessNameOperationResult
{
public:
	void nameAcquired(const char *name) { tinfo("got name ", name); }
	void nameLost(const char *name) { tfail("unexpected name loss:", name); }
};

/*
 * ExpectedFailureNameOperationResult is an implementation of NameOperationResult interface.
 * It passes if a name is lost and fails if a name is acquired.
 */
class ExpectedFailureNameOperationResult
{
public:
	void nameAcquired(const char *name) { tfail("unexpected name acquire:", name); }
	void nameLost(const char *name) { tinfo("lost name ", name); }
};

/*
 * ExpectedFailureThenSuccessNameOperationResult is an implementation of NameOperationResult interface.
 * It passes if a name is first lost and then acquired and fails if a name is first acquired
 * or lost second time.
 */
class ExpectedFailureThenSuccessNameOperationResult
{
	bool gotLost;
public:
	ExpectedFailureThenSuccessNameOperationResult() : gotLost(false) {}
	void nameAcquired(const char *name) { tinfo("name acquire after queuing:", name); tassert(gotLost); }
	void nameLost(const char *name) { tinfo("lost name ", name); tassert(!gotLost); gotLost = true; }
};

/*
 * **** Signal interface for Impls. ****
 * **** For use by Waiters ****
 * Signal is responsible for providing interface and member for a signal.
 * Methods:
 *		Iface iface
 *			returns interface name for signal
 *		Name name
 *			returns member name for signal
 */
/*
 * NameOwnerChangedSignal is an implementation of Signal interface.
 * It serves as a source of interface and member name for NameOwnerChangedSignal.
 */
class NameOwnerChangedSignal
{
public:
	testlib::Iface iface() { return testlib::Iface("org.freedesktop.DBus"); }
	testlib::Member name() { return testlib::Member("NameOwnerChanged"); }
};

/*
 * ARealBus is a base class for all the implementations of Bus interface.
 * It serves as a container for specialized Setup.
 * It is parameterized by its template argument Setup, which is specific for underlying implementation.
 */
template <class Setup>
class ARealBus
{
protected:
	Setup _setup;
public:
	void prepare()
	{
		prepare(_setup);
	}
	auto &bus()
	{
		return _setup;
	}
	virtual void prepare(Setup &setup) = 0;
	virtual void done() = 0;
	virtual ~ARealBus() {}
};

/*
 * ExternalBus is an implementation of Bus interface (see ImplDbus.h and ImplGdbus.h).
 *
 * ExternalBus does not create its own connection. It takes connection from the outside
 * and provide it to user.
 * With ExternalBus several NameOperations may work on same physical connection:
 * the main NameOperation may create connection with SharedBus or PrivateBus
 * and pass it to ExternalBuses of other NameOperations.
 */
template <class Setup>
class ExternalBus
{
public:
	ExternalBus() : _setup(nullptr) {}
	void prepare(Setup &setup)
	{
		tassert(&setup != _setup);
		_setup = &setup;
	}
	auto &bus()
	{
		return *_setup;
	}
	void done()
	{
	}
private:
	Setup *_setup;
};

}

#endif // IMPL_COMMON_H
