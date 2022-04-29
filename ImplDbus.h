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
#ifndef TESTDBUS_H
#define TESTDBUS_H

#include <cstring>
#include <dbus/dbus.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include "../lib/testlib.h"
#include "ImplCommon.h"

#define failIfErr(ERR,STR) do { if (dbus_error_is_set(&(ERR))) tfail((STR), " [", (ERR).message, "]"); } while (0)

namespace ImplDbus {

NO_TEMPLATE void unref(DBusMessage *m) { dbus_message_unref(m); }
NO_TEMPLATE void unref(DBusConnection *c) { dbus_connection_unref(c); }

/*
 * **** BusGetter interface for ImplDbus. ****
 * **** For use with Setups (client-side) and Services (server-side) ****
 * BusGetter is responsible for creating bus connections. Required methods are:
 * 		Bus bus()
 *			creates and returns shared bus connection
 *		Bus privBus()
 *			creates and returns private bus connection
 *		done(Bus bus)
 *			unrefs/closes bus connection
 */

/*
 * BusGetterFromEnv is an implementation of BusGetter interface.
 * It takes data needed to open the connection from environment variables.
 */
class BusGetterFromEnv
{
public:
	static void done(DBusConnection *bus) { unref(bus); }
	static wur DBusConnection *bus() { return getbus_(dbus_bus_get, getBusType); }
	static wur DBusConnection *privBus() { return getbus_(dbus_bus_get_private, getBusType); }
private:
	static wur DBusConnection *getbus_(DBusConnection *(*func)(DBusBusType,DBusError*),
			DBusBusType (*getBusType)() = getBusType) {
		DBusError dberr;
		dbus_error_init(&dberr);
		auto conn = func(getBusType(), &dberr);
		dbus_connection_set_exit_on_disconnect(conn, FALSE);
		failIfErr(dberr, "Getting bus failed");
		assert(conn);
		return conn;
	}
	static DBusBusType getBusType() {
		if (getenv("DBUS_STARTER_BUS_TYPE"))
			return DBUS_BUS_STARTER;

		char *busType = getenv("DBUS_BUS_TYPE");
		if (busType != nullptr) {
			if (strcmp(busType, "1") == 0)
				return DBUS_BUS_SYSTEM;
			else if (strcmp(busType, "0") == 0)
				return DBUS_BUS_SESSION;
		}

		busType = getenv("DBUS_SESSION_BUS_ADDRESS");
		if (busType != nullptr)
			return DBUS_BUS_SESSION;
		return DBUS_BUS_SYSTEM;
	}
};

/*
 * ConnectionSetupData is a wrapper for data needed to keep
 * by Buses and NameOperations (see below).
 */
struct ConnectionSetupData
{
	DBusConnection *conn;
};

/*
 * **** Bus interface for ImplDbus. ****
 * **** For use by NameOperations and Waiters ****
 * **** Additionally it is used in MethodCallForUniqueIdCreator for creating short-lived local bus.
 * Bus is responsible for controlling timelife of bus connection. Methods:
 *		void prepare(setup)
 *			creates connection
 *		void done()
 *			closes connection
 */

/*
 * ABus is a base class for all the Dbus implementations of Bus interface.
 * It is parameterized by BusTypeGetter, which selects type of bus and provides bus address.
 */
template <typename BusGetter = BusGetterFromEnv>
class ABus : public ImplCommon::ARealBus<ConnectionSetupData>
{
protected:
	BusGetter busGetter;
	using ARealBus<ConnectionSetupData>::prepare;
	template <class F>
	void prepare(ConnectionSetupData &setup, F fun)
	{
		_setup.conn = fun();
		tassert(_setup.conn);
	}
};

/*
 * SharedBus is a class that controls timelife of shared bus connection.
 * Shared bus connection is a connection that may be used by other users of libdbus.
 */
template <typename BusGetter = BusGetterFromEnv>
class SharedBus : public ABus<BusGetter>
{
public:
	using ABus<BusGetter>::prepare;
	void prepare(ConnectionSetupData &setup)
	{
		prepare(setup, &BusGetter::bus);
	}
	void done()
	{
		this->busGetter.done(this->_setup.conn);
	}
};

/*
 * PrivateBus is a class that controls timelife of private bus connection.
 * Private bus connection is a connection that no one else can use.
 */
template <typename BusGetter = BusGetterFromEnv>
class PrivateBus : public ABus<BusGetter>
{
public:
	using ABus<BusGetter>::prepare;
	void prepare(ConnectionSetupData &setup)
	{
		prepare(setup, &BusGetter::privBus);
	}
	void done()
	{
		dbus_connection_close(this->_setup.conn);
		this->busGetter.done(this->_setup.conn);
	}
};

/*
 * ExternalBus does not create its own connection. It takes connection from the outside
 * and provide it to user.
 * With ExternalBus several NameOperations may work on same physical connection:
 * the main NameOperation may create connection with SharedBus or PrivateBus
 * and pass it to ExternalBuses of other NameOperations.
 */
typedef ImplCommon::ExternalBus<ConnectionSetupData> ExternalBus;

/******************************** CLIENT-SIDE ***************************/
/*
 * ClientSetupData is a wrapper for data needed to pass between Setup and SendMethod interfaces (see below).
 */
struct ClientSetupData
{
	DBusConnection *connection;
};

/*
 * **** Setup interface for ImplDbus. ****
 * **** For direct use by test scenarios ****
 * Setup manages lifetime of bus connection, and may perform additional actions
 * needed before using SendMethod, e.g. waiting for a specific bus name (see below).
 * Required methods are:
 *		auto Setup::setup(Name name)
 *			prepares bus connection for SendMethod
 *			returns ClientSetupData
 *		void tearDown()
 *			closes connections, performs clean up
 */

/*
 * GetConnectionSetup is an implementation of Setup interface. Its single purpose
 * is the management of life time of bus connection.
 */
template <typename BusGetter = BusGetterFromEnv>
class GetConnectionSetup
{
	BusGetter busGetter;
public:
	auto setup(const testlib::Name &name)
	{
		_setup.connection = busGetter.bus();
		return _setup;
	}
	void tearDown()
	{
		busGetter.done(_setup.connection);
	}
private:
	ClientSetupData _setup;
};

/*
 * SetupWaitForName is an implementation of Setup interface. It extends GetConnectionSetup
 * with waiting for a given bus name on the bus.
 */
template <typename BusGetter = BusGetterFromEnv>
class SetupWaitForName : private GetConnectionSetup<BusGetter>
{
public:
	auto setup(const testlib::Name &name)
	{
		auto s = GetConnectionSetup<BusGetter>::setup(name);

		DBusError dberr;
		dbus_error_init(&dberr);

		std::string matchRule{"type='signal',"
		                      "sender='org.freedesktop.DBus',"
		                      "interface='org.freedesktop.DBus',"
		                      "member='NameOwnerChanged',"
		                      "arg0='"};
		matchRule += name();
		matchRule += "'";

		dbus_bus_add_match(s.connection, matchRule.c_str(), &dberr);
		failIfErr(dberr, "match management (add) while waiting for names");
		tinfo("adding match for NameOwnerChanged");

		while (!nameHasOwner(name, s.connection))
			dbus_connection_read_write_dispatch(s.connection, -1);

		dbus_bus_remove_match(s.connection, matchRule.c_str(), &dberr);
		failIfErr(dberr, "match management (remove) while waiting for names");
		tinfo("removing match for NameOwnerChanged");

		return s;
	}
	using GetConnectionSetup<BusGetter>::tearDown;
private:
	NO_TEMPLATE wur dbus_bool_t nameHasOwner(testlib::Name name, DBusConnection *c)
	{
		DBusError dberr;
		dbus_bool_t res;
		dbus_error_init(&dberr);
		res = dbus_bus_name_has_owner(c, name(), &dberr);
		failIfErr(dberr, "name has owner");
		tinfo("nameHasOwner(", name(), ")=", unsigned(res));
		return res;
	}
};

/*
 * **** MessageCreator interface for ImplDbus. ****
 * **** For use by MessageFeeders ****
 * MessageCreator is responsible for creating a message. Required methods are:
 *		Message create()
 *			creates and returns a single message
 */
/*
 * MethodCallCreator is an implementation of MessageCreator interface. It creates
 * a single method call.
 */
class MethodCallCreator
{
public:
	auto create(const testlib::TestSpec &spec) const
	{
		auto msg = dbus_message_new_method_call(spec.name(), spec.path(), spec.iface(),
				spec.member());
		tassert(msg);
		dbus_message_set_auto_start(msg, !spec.no_auto_start());
		return msg;
	}
};

/*
 * MethodCallForUniqueIdCreator is an implementation of MessageCreator interface. It creates
 * a single method call with unique id of the owner of the specified name.
 */
template <class Bus = PrivateBus<>>
class MethodCallForUniqueIdCreator
{
	Bus bus;
public:
	auto create(const testlib::TestSpec &spec)
	{
		// get name owner
		bus.prepare();
		auto ownermsg = dbus_message_new_method_call(
				"org.freedesktop.DBus",
				"/org/freedesktop/DBus",
				"org.freedesktop.DBus",
				"GetNameOwner");
		tassert(ownermsg);
		const char *name = spec.name();
		tassert(dbus_message_append_args(ownermsg, DBUS_TYPE_STRING, &name, DBUS_TYPE_INVALID));
		DBusError error;
		dbus_error_init(&error);
		auto ownerreply = dbus_connection_send_with_reply_and_block(bus.bus().conn,
					ownermsg, -1, &error);
		tassert(ownerreply);
		failIfErr(error, "GetNameOwner");
		unref(ownermsg);

		const char *struid;
		tassert(dbus_message_get_args(ownerreply, &error, DBUS_TYPE_STRING, &struid, DBUS_TYPE_INVALID));

		bus.done();

		auto msg = dbus_message_new_method_call(struid, spec.path(), spec.iface(),
				spec.member());
		tassert(msg);
		unref(ownerreply);
		dbus_message_set_auto_start(msg, !spec.no_auto_start());
		return msg;
	}
};

/*
 * MethodCallCreator is an implementation of MessageCreator interface. It creates
 * a single signal.
 */
class SignalCreator
{
public:
	auto create(const testlib::TestSpec &spec) const
	{
		auto msg = dbus_message_new_signal(spec.path(), spec.iface(), spec.member());
		tassert(msg);
		return msg;
	}
};

/*
 * **** PayloadGenerator interface for ImplDbus. ****
 * **** For use by MessageFeeders ****
 * PayloadGenerator is responsible for providing payload for a message.
 *		Message addPayload(Message message, size)
 *			creates payload and adds it to the message
 *			returns the same message
 */

/*
 * ConstPayloadGenerator is an implementation of PayloadGenerator interface.
 * It appends hard-coded string as the element of the message.
 */
class ConstPayloadGenerator
{
public:
	auto addPayload(DBusMessage *msg, dbus_uint64_t size) const
	{
		const char *s = "This is THE message";
		tassert(dbus_message_append_args(msg, DBUS_TYPE_STRING, &s, DBUS_TYPE_INVALID));
		return msg;
	}
};

/*
 * PayloadSizeGenerator is an implementation of PayloadGenerator interface.
 * It creates an array of bytes and appends it as the element of the message.
 */
class PayloadSizeGenerator
{
public:
	auto addPayload(DBusMessage *msg, dbus_uint64_t size) const
	{
		if (size > 0) {
			auto array = new unsigned char[size];
			FORLLZ(k, size) array[k] = k % 10;
			tassert(dbus_message_append_args(msg, DBUS_TYPE_ARRAY, DBUS_TYPE_BYTE,
					&array, size, DBUS_TYPE_INVALID));
			delete [] array;
		}
		return msg;
	}
};

/*
 * UnixFDGenerator is an implementation of PayloadGenerator interface.
 * It appends hard-coded string as the element of the message, and additionally creates and adds
 * a specified number of file descriptors to the message.
 */
class UnixFDGenerator
{
public:
	auto addPayload(DBusMessage *msg, dbus_uint64_t size) const
	{
		DBusMessageIter it;
		dbus_message_iter_init_append(msg, &it);

		const char *s = "This is THE message with UNIX fds";
		tassert(dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &s));
		DBusMessageIter it_arr;

		tassert(dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, DBUS_TYPE_UNIX_FD_AS_STRING, &it_arr));

		TIMESLLZ(size) {
			int fd = open("/dev/zero", O_RDONLY|O_CLOEXEC);
			tassert(fd != -1);
			tassert(dbus_message_iter_append_basic(&it_arr, DBUS_TYPE_UNIX_FD, &fd));
			close(fd);
		}

		tassert(dbus_message_iter_close_container(&it, &it_arr));
		return msg;
	}
};

/*
 * **** MessageFeeder interface for ImplDbus. ****
 * **** For direct use by test scenarios ****
 * MessageFeeder is responsible for messages creation and disposal. Required methods:
 *		Message next()
 *			creates a message
 *			returns the created message
 *		void dispose(Message message)
 *			gets rid of the message
 */

/*
 * MessageFeeder is an implementation of MessageFeeder interface. It is parameterized
 * by its template arguments NewMessageCreator and PayloadGenerator.
 * NewMessageCreator must be an implementation of MessageCreator interface.
 * PayloadGenerator must be an implementation of PayloadGenerator interface.
 */
template <class NewMessageCreator, class PayloadGenerator>
class MessageFeeder
{
	NewMessageCreator newMessageCreator;
	PayloadGenerator payloadGenerator;
public:
	// auto next(const testlib::TestSpec &spec) - to be implemented in subclasses
	void dispose(DBusMessage *msg) const {
		unref(msg);
	}
	void payload_size(dbus_uint64_t size) { _payload_size = size; }
	auto next(const testlib::TestSpec &spec)
	{
		return payloadGenerator.addPayload(newMessageCreator.create(spec), _payload_size);
	}
private:
	dbus_uint64_t _payload_size;
};

/*
 * These are convenient names for specializations of MessageFeeder.
 */
typedef MessageFeeder
			<MethodCallCreator,
			ConstPayloadGenerator> MessageMethodCallFeeder;
typedef MessageFeeder
			<MethodCallForUniqueIdCreator<>,
			ConstPayloadGenerator> MessageMethodCallToUniqueIdFeeder;
typedef MessageFeeder
			<SignalCreator,
			ConstPayloadGenerator> MessageSignalFeeder;
typedef MessageFeeder
			<MethodCallCreator,
			PayloadSizeGenerator> MessageMethodCallPayloadFeeder;
typedef MessageFeeder
			<SignalCreator,
			PayloadSizeGenerator> MessageSignalPayloadFeeder;
typedef MessageFeeder
			<MethodCallCreator,
			UnixFDGenerator> MessageMethodCallUnixFDFeeder;

/*
 * **** ReplyHandlingMethod interface for ImplDbus. ****
 * **** For use by SendMethods ****
 * ReplyHandlingMethod is responsible for actions that should be made upon receiving
 * some reply for a method call. RequiredMethods:
 * 		handleReply(Message message, Message reply, Error error)
 *			considers sent message, reply and error received after sending a method call
 *			and performs checks or other actions
 */

/*
 * IsEqualReplyHandlingMethod is an implementation of ReplyHandlingMethod interface.
 * It checks if a response's data is equal to message's data. It may be useful
 * while calling echo-like service.
 */
class IsEqualReplyHandlingMethod
{
public:
	void handleReply(DBusMessage *msg, DBusMessage *reply, DBusError &dberrsend)
	{
		failIfErr(dberrsend, "get reply");
		tassert(reply);

// This won't work because it takes into account header and body together:
//		char *msg_data, *reply_data;
//		int msg_len, reply_len;
//		tassert(dbus_message_marshal(msg, &msg_data, &msg_len));
//		tassert(dbus_message_marshal(reply, &reply_data, &reply_len));
//		tassert(msg_len == reply_len);		
//		tassert(0 == memcmp(msg_data, reply_data, msg_len));
//		free(msg_data);
//		free(reply_data);
// FIXME: implement iterating over messages contents.
	}
};

/*
 * IsStringReplyHandlingMethod is an implementation of ReplyHandlingMethod interface.
 * It checks if a response contains single argument of string type.
 */
class IsStringReplyHandlingMethod
{
public:
	void handleReply(DBusMessage *msg, DBusMessage *reply, DBusError &dberrsend)
	{
		failIfErr(dberrsend, "get reply");
		tassert(reply);

		DBusError dberr;
		char *str;
		dbus_error_init(&dberr);
		tassert(dbus_message_get_type(reply) == DBUS_MESSAGE_TYPE_METHOD_RETURN);
		tassert(dbus_message_get_args(reply, &dberr, DBUS_TYPE_STRING, &str, DBUS_TYPE_INVALID));
		failIfErr(dberr, "method return invalid");
	}
};

/*
 * Errors in libdbus are strings. We cannot put strings into types, so we must enumerate
 * the handled errors.
 */
enum class Errors {
	SERVICE_UNKNOWN,
	NAME_HAS_NO_OWNER,
	UNKNOWN_METHOD
};

/*
 * ExpectDBusErrorReplyHandlingMethod is an implementation of ReplyHandlingMethod interface.
 * It checks if the method call failed with a specific error. It is parameterized by error.
 */
template <Errors err>
class ExpectDBusErrorReplyHandlingMethod
{
public:
	void handleReply(DBusMessage *msg, DBusMessage *reply, DBusError &error)
	{
		tassert(0 && "This error is not handled");
	}
};

/*
 * This is a macro for generating specializations of ExpectDBusErrorReplyHandlingMethod.
 */
#define IMPL_DBUS_HANDLE_ERROR(e) \
	template <> \
	class ExpectDBusErrorReplyHandlingMethod<Errors::e> \
	{ \
	public: \
		void handleReply(DBusMessage *msg, DBusMessage *reply, DBusError &error) \
		{ \
			tassert(dbus_error_has_name(&error, DBUS_ERROR_##e)); \
			tassert(!reply); \
		} \
	};

/*
 * Specializations of ExpectDBusErrorReplyHandlingMethod.
 */
IMPL_DBUS_HANDLE_ERROR(SERVICE_UNKNOWN);
IMPL_DBUS_HANDLE_ERROR(UNKNOWN_METHOD);
IMPL_DBUS_HANDLE_ERROR(NAME_HAS_NO_OWNER);

/*
 * **** SendMethod interface for ImplDbus. ****
 * **** For use directly by test scenarios ****
 * SendMethod is responsible for sending messages. Methods:
 *		void prepareForSending(setup)
 *			takes what it needs for further work
 *		void send(Message message)
 *			executes specified method call (sends message)
 */

/*
 * SendMethod is a base class with common operations for implementations of SendMethod interface.
 */
class SendMethod
{
public:
	void prepareForSending(const ClientSetupData &setup, const testlib::TestSpec &)
	{
		conn = setup.connection;
	}
protected:
	DBusConnection *conn;
};

/*
 * SimpleSendMethod is an implementation of SendMethod interface.
 * It sends passed messages synchronously and handles replies.
 * It is parameterized by its template argument ReplyHandlingMethod.
 * ReplyHandlingMethod must be an implementation of ReplyHandlingMethod interface.
 */
template <typename ReplyHandlingMethod = IsStringReplyHandlingMethod>
class SimpleSendMethod : public SendMethod
{
	ReplyHandlingMethod replyHandlingMethod;
public:
	void send(DBusMessage *msg)
	{
		DBusError dberr;
		dbus_error_init(&dberr);
		auto r = dbus_connection_send_with_reply_and_block(conn, msg, -1, &dberr);

		replyHandlingMethod.handleReply(msg, r, dberr);
	}
};

/*
 * SimpleAsyncSendMethod is an implementation of SendMethod interface.
 * It sends passed messages asynchronously (that is, it does not care for replies).
 */
class SimpleAsyncSendMethod : public SendMethod
{
public:
	void send(DBusMessage *msg)
	{
		if (!dbus_connection_send(conn, msg, NULL))
			tfail("Failed to send a message asynchronously");
		dbus_connection_flush(conn);
	}
};

/******************************** SERVER-SIDE ***************************/
/*
 * ServiceSetupData is a wrapper for data needed to pass between Service and Dispatcher
 * interfaces (see below).
 */
struct ServiceSetupData
{
	void init(DBusConnection *c) { conn = c; finish = false; }
	DBusConnection *conn;
	bool finish;
	bool processed;
};

/*
 * **** Reaction interface for ImplDbus. ****
 * **** For use by Services ****
 * Reaction is responsible for reacting on incoming messages. Methods:
 *		void react(Setup setup, Message message)
 *			acts appropriately for incoming message
 */
/*
 * RespondTimesSyncReaction is an implementation of Reaction interface.
 * It creates a constant hard-coded string reply and sends it back.
 * Additionally it counts incoming messages and sets 'finish' flag when
 * the number hits given value.
 */
class RespondTimesSyncReaction : public ImplCommon::Counter
{
public:
	void react(ServiceSetupData &setup, DBusMessage *msg, testlib::TestSpec *spec)
	{
		auto reply = dbus_message_new_method_return(msg);
		const char *s = "This is THE reply";
		tassert(dbus_message_append_args(reply, DBUS_TYPE_STRING, &s, DBUS_TYPE_INVALID));
		tassert(dbus_connection_send(setup.conn, reply, NULL));
		dbus_connection_flush(setup.conn);
		unref(reply);
		if (updateCounter())
			setup.finish = true;
	}
};

/*
 * CountReaction is an implementation of Reaction interface.
 * It counts incoming messages and sets 'finish' flag when the number hits
 * given value.
 */
class CountReaction : public ImplCommon::Counter
{
public:
	void react(ServiceSetupData &setup, DBusMessage *msg, testlib::TestSpec *spec)
	{
		if (updateCounter())
			setup.finish = true;
	}
};

/*
 * CheckUnixFDsReaction is an implementation of Reaction interface.
 * It gets unix file descriptors from the incoming message and checks
 * if they are valid.
 */
class CheckUnixFDsReaction
{
public:
	void react(ServiceSetupData &setup, DBusMessage *msg, testlib::TestSpec *spec)
	{
		DBusMessageIter it;
		tassert(dbus_message_iter_init(msg, &it));

		tassert(dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_STRING);
		dbus_message_iter_next(&it);

		tassert(dbus_message_iter_get_element_type(&it) == DBUS_TYPE_UNIX_FD);
		unsigned size = dbus_message_iter_get_element_count(&it);
		tassert(size == spec->payload_size());

		DBusMessageIter it_arr;
		dbus_message_iter_recurse(&it, &it_arr);

		FORZ(k, size) {
			int fd;
			tassert(dbus_message_iter_get_arg_type(&it_arr) == DBUS_TYPE_UNIX_FD);
			dbus_message_iter_get_basic(&it_arr, &fd);
			tassert(fd != -1);
			tassert(dbus_message_iter_next(&it_arr) || k == size-1);
			char c;
			tassert(read(fd, &c, 1) == 1);		// read one byte from /dev/zero
			tassert(c == 0);					// check zero
			tassert(close(fd) != -1);
		}
	}
};

/*
 * CheckPayloadReaction is an implementation of Reaction interface.
 * It checks if the payload in incoming message is valid
 * as generated by PayloadSizeGenerator.
 */
class CheckPayloadReaction
{
public:
	void react(ServiceSetupData &setup, DBusMessage *msg, testlib::TestSpec *spec)
	{
		if (spec->payload_size() > 0) {
			unsigned char *array;
			int elems;
			DBusError error;

			dbus_error_init(&error);

			tassert(dbus_message_get_args(msg, &error,
					DBUS_TYPE_ARRAY, DBUS_TYPE_BYTE, &array, &elems, DBUS_TYPE_INVALID));
			failIfErr(error, "Payload checking failed");
			tassert(static_cast<size_t>(elems) == spec->payload_size());
			FORZ(k, static_cast<size_t>(elems)) {
				tassert(array[k] == k % 10);
			}
		} else {
			DBusMessageIter iter;
			dbus_message_iter_init(msg, &iter);
			tassert(dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_INVALID);
		}
	}
};

/*
 * ReactionAnd is an implementation of Reaction interface.
 * It is template class which makes composite reaction from two reactions.
 * It is parameterized by two reactions T1 and T2 (implementations of Reaction interface).
 * Note: it may be extended to accept any number of reactions, if needed.
 */
template <typename T1, typename T2>
class ReactionAnd : public T1, public T2
{
public:
	void react(ServiceSetupData &setup, DBusMessage *msg, testlib::TestSpec *spec)
	{
		T1::react(setup, msg, spec);
		T2::react(setup, msg, spec);
	}
};

/*
 * NoMessageAllowedReaction is an implementation of Reaction interface.
 * A test is considered as failed if a message is received and passed to
 * this reaction.
 */
class NoMessageAllowedReaction : public ImplCommon::Counter
{
public:
	void react(ServiceSetupData &setup, DBusMessage *msg, testlib::TestSpec *spec)
	{
		tassert(0 && "Unexpected message received");
	}
};

/*
 * MatchString is a helper class used to manage filter strings.
 * It composes a single valid filter string out of valid components (sender, path, interface, etc).
 */
class MatchString
{
public:
	MatchString() : _match("type='signal'") {}
	auto &add(const char *name, const char *value)
	{
		if (value[0] != 0) {
			_match += ",";
			_match += name;
			_match += "='";
			_match += value;
			_match += "'";
		}
		return *this;
	}
#define D_ADD(string, type) \
	auto &add(type value) \
	{ \
		return add(string, value()); \
	}
	D_ADD("sender", testlib::Name);
	D_ADD("path", testlib::Path);
	D_ADD("interface", testlib::Iface);
	D_ADD("member", testlib::Member);
	D_ADD("arg0", testlib::Arg0);
#undef D_ADD
	const char *operator()() const
	{
		return _match.c_str();
	}

private:
	std::string _match;
};

/*
 * **** Service interface for ImplDbus. ****
 * **** For use directly by test scenarios ****
 * Service is responsible for receiving messages and reacting on them. Methods:
 *		ServiceSetupData setup()
 *			prepares service for receiving messages
 *			returns ServiceSetupData filled with prepared values
 *		void tearDown()
 *			closes service, cleans up.
 */

/*
 * Service is a base class with common operations for implementations of Service interface.
 */
class Service
{
protected:
	NO_TEMPLATE void request_name(testlib::Name name, unsigned flags) {
		tassert(name);
		tinfo("requestName(", name(), ")");
		DBusError e;
		dbus_error_init(&e);
		auto ret = dbus_bus_request_name(_setup.conn, name(), flags, &e);
		if (0 > ret)
			tfail("Error - could not request name (", dbus_error_is_set(&e) ? e.message : "", ")");
		tinfo("Name (", name(), ") requestRes(", ret, ")");
	}
	NO_TEMPLATE void requestName(testlib::Name name)
	{
		request_name(name, DBUS_NAME_FLAG_DO_NOT_QUEUE);
	}
	NO_TEMPLATE void releaseName(testlib::Name name)
	{
		DBusError e;
		dbus_error_init(&e);
		tassert(dbus_bus_release_name(_setup.conn, name(), &e) == DBUS_RELEASE_NAME_REPLY_RELEASED);
		failIfErr(e, "release name failed");
	}
	NO_TEMPLATE void registerObjectPath(testlib::Path path, DBusObjectPathMessageFunction func)
	{
		DBusObjectPathVTable vtable;
		vtable.unregister_function = nullptr;
		vtable.message_function = func;
		if (!dbus_connection_register_object_path(_setup.conn, path(), &vtable, this))
			tfail("Error - could not register object path (", path(), ")");
		tinfo("registered path (", path(), ")");
	}
	NO_TEMPLATE void unregisterObjectPath(testlib::Path path)
	{
		tassert(dbus_connection_unregister_object_path(_setup.conn, path()));
	}
	NO_TEMPLATE void addMatch(const MatchString &match)
	{
		doMatch(dbus_bus_add_match, match);
	}
	NO_TEMPLATE void removeMatch(const MatchString &match)
	{
		doMatch(dbus_bus_remove_match, match);
	}

	ServiceSetupData _setup;

private:
	NO_TEMPLATE void doMatch(void (*op)(DBusConnection *, const char *, DBusError *),
			const MatchString &match)
	{
		DBusError dberr;
		dbus_error_init(&dberr);
		op(_setup.conn,	match(), &dberr);
		failIfErr(dberr, "match management");
		tinfo("adding/removing match (", match(), ")");
	}
};

/*
 * SingleMethodService is an implementation of Service interface.
 * It provides a service with given well-known name and object at given location,
 * and registers handlers for incoming messages.
 * The D-Bus object defines single interface and single method in this interface.
 * It is parameterized by Reaction and BusGetter template arguments.
 * Reaction must be an implementation of Reaction interface.
 * BusGetter must be an implementation of BusGetter interface.
 */
template <typename Reaction = RespondTimesSyncReaction,
		  typename BusGetter = BusGetterFromEnv>
class SingleMethodService : public Service
{
	BusGetter busGetter;
	Reaction reaction;
public:
	ServiceSetupData &setup(testlib::TestSpec &spec, unsigned served_messages = 1)
	{
		method = &spec;
		_setup.init(busGetter.bus());

		reaction.counter(served_messages);

		auto message_function = [] (DBusConnection *conn, DBusMessage *msg, void *t) {
			auto this_ = THIS(t);
			if(dbus_message_is_method_call(msg, this_->method->iface(), this_->method->member()))
			{
				this_->_setup.processed = true;
				this_->reaction.react(this_->_setup, msg, this_->method);
				return DBUS_HANDLER_RESULT_HANDLED;
			}
			return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
		};

		registerObjectPath(method->path, message_function);
		requestName(spec.name);
		return _setup;
	}
	void tearDown()
	{
		unregisterObjectPath(method->path);
		releaseName(method->name);
		busGetter.done(_setup.conn);
	}
private:
	testlib::TestSpec *method;
};

/*
 * ListenToSignalsService is an implementation of Service interface.
 * It registers filters for given signal.
 * It also provides a bus service with given well-known name for means of synchronization with Senders.
 * It is parameterized by Reaction and BusGetter template arguments.
 * Reaction must be an implementation of Reaction interface.
 * BusGetter must be an implementation of BusGetter interface.
 */
template <typename Reaction = CountReaction,
		  typename BusGetter = BusGetterFromEnv>
class ListenToSignalsService : public Service
{
	Reaction reaction;
	BusGetter busGetter;
public:
	ServiceSetupData &setup(testlib::TestSpec &spec, unsigned served_messages = 1)
	{
		_spec = &spec;
		_setup.init(busGetter.bus());

		reaction.counter(served_messages);

		auto signal_function = [] (DBusConnection *conn, DBusMessage *msg, void *t) {
			auto this_ = THIS(t);
			if (dbus_message_is_signal(msg, this_->_spec->iface(), this_->_spec->member()))
			{
				this_->_setup.processed = true;
				this_->reaction.react(this_->_setup, msg, this_->_spec);
				return DBUS_HANDLER_RESULT_HANDLED;
			}
			return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
		};

		// subscribe for signal
		addMatch(MatchString().add(_spec->iface).add(_spec->member));
		// add matches
		tassert(dbus_connection_add_filter(_setup.conn, signal_function, this, NULL));

		requestName(_spec->name);
		return _setup;
	}
	void tearDown()
	{
		removeMatch(MatchString().add(_spec->iface).add(_spec->member));
		releaseName(_spec->name);
		busGetter.done(_setup.conn);
	}
private:
	testlib::TestSpec *_spec;
};

/*
 * **** Dispatcher interface for ImplDbus. ****
 * **** For use directly by test scenarios ****
 * Dispatcher is responsible for controlling timelife of Services. Methods:
 *		unsigned dispatchOnce(setup)
 *			executes waiting for incoming messages, and handling them
 *			returns number of handled messages
 */
/*
 * Dispatcher is an implementation of Dispatcher interface.
 * It gives Service possibility to handle single incoming message.
 */
class Dispatcher
{
public:
	NO_TEMPLATE void dispatch(ServiceSetupData &setup)
	{
		while (!setup.finish && dbus_connection_read_write_dispatch(setup.conn, -1));
	}
	NO_TEMPLATE unsigned dispatchOnce(ServiceSetupData &setup)
	{
		setup.processed = false;
		while (!setup.processed) dbus_connection_read_write_dispatch(setup.conn, -1);
		return 1;
	}
};

/**************** Well-known names management ********************/
/*
 * NameOperations is a type container for set of NameOperations, specialized for
 * ImplDbus::ConnectionSetupData.
 */
template <typename Bus, class ...Args>
using NameOperations = ImplCommon::NameOperations<Bus, ConnectionSetupData, Args...>;

/*
 * **** NameOperation interface for ImplDbus. ****
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
 * RequestNameOperation is implementation of NameOperation interface.
 * It requests a given name from a bus and checks the result.
 * It is parameterized by Bus and NameOperationResult (see ImplCommon.h).
 * Bus must be an implementation of Bus interface.
 * NameOperationResult must be an implementation of NameOperationResult interface (see ImplCommon.h)
 */
template <typename Bus, typename NameOperationResult = ImplCommon::ExpectedSuccessNameOperationResult>
class RequestNameOperation
{
	Bus bus;
	NameOperationResult nameOperationResult;
public:
	void prepare(const testlib::Name name, ConnectionSetupData &setup)
	{
		_name = name;
		bus.prepare(setup);
	}
	void prepare(const testlib::Name name)
	{
		prepare(name, bus.bus());
	}
	void execute()
	{
		int result = dbus_bus_request_name(bus.bus().conn, _name(),
											DBUS_NAME_FLAG_DO_NOT_QUEUE, NULL);
		if (DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER == result) {
			nameOperationResult.nameAcquired(_name());
		} else {
			nameOperationResult.nameLost(_name());
		}
	}
	void finish()
	{
		bus.done();
	}
private:
	testlib::Name _name;
};

/*
 * ReleaseNameOperation is implementation of NameOperation interface
 * It releases previously requested bus name.
 * It is parameterized by Bus and integer, to allow multiple instances of the operation
 * to be specified.
 * Bus must be an implementation of Bus interface.
 */
template <typename Bus, int instance = 0>
class ReleaseNameOperation
{
	Bus bus;
public:
	void prepare(const testlib::Name name, ConnectionSetupData &setup)
	{
		_name = name;
		bus.prepare(setup);
	}
	void prepare(const testlib::Name name)
	{
		prepare(name, bus.bus());
	}
	void execute()
	{
		DBusError error;
		dbus_error_init(&error);
		dbus_bus_release_name(bus.bus().conn, _name(), &error);
		failIfErr(error, "Releasing name failed");
		tinfo("released name ", _name());
	}
	void finish() {}
private:
	testlib::Name _name;
};

/*
 * **** Waiter interface for ImplDbus. ****
 * **** For use directly by test scenarios ****
 * Waiter is responsible for subscribing to name signals and checking them. Methods:
 *		void setup(testlib::Name name)
 *			subscribes for proper signal
 *		void expectSignals()
 *			waits for signals in loop
 *		void tearDown()
 *			cleans up after waiting
 */
/*
 * NameSignalWaiter is an implementation of Waiter interface.
 * It subscribes to given signal, and checks if it receives expected signals.
 * It is parameterized by Bus and Signal.
 * Bus must be an implementation of Bus interface.
 * Signal must be an implementation of Signal interface (see ImplComon.h)
 */
template <typename Bus, typename Signal>
class NameSignalWaiter : private Service
{
	Bus bus;
	Signal signal;
public:
	void setup(testlib::Name name)
	{
		_name = name;
		bus.prepare();

		auto c = bus.bus().conn;
		matchString.add(signal.iface()).add(signal.name()).add(testlib::Arg0(_name()));

		DBusError error;
		dbus_error_init(&error);
		dbus_bus_add_match(c, matchString(), &error);
		failIfErr(error, "Adding match failed");
		tassert(dbus_connection_add_filter(c, signal_handler, this, NULL));
	}
	void expectSignals()
	{
		done = false;
		while (!done && dbus_connection_read_write_dispatch(bus.bus().conn, -1));
		tassert(done);
	}
	void tearDown()
	{
		auto c = bus.bus().conn;
		DBusError error;
		dbus_error_init(&error);
		dbus_bus_remove_match(c, matchString(), &error);
		failIfErr(error, "Adding match failed");
		dbus_connection_remove_filter(c, signal_handler, this);
		bus.done();
	}
private:
	testlib::Name _name;
	bool done;
	MatchString matchString;

	static const char *str(DBusMessage *m)
	{
		char *str;
		DBusError dberr;
		dbus_error_init(&dberr);
		tassert(dbus_message_get_args(m, &dberr, DBUS_TYPE_STRING, &str, DBUS_TYPE_INVALID));
		failIfErr(dberr, "Wrong arguments in message");
		return str;
	}

	static auto signal_handler(DBusConnection *c, DBusMessage *m, void *d) {
		auto this_ = reinterpret_cast<NameSignalWaiter *>(d);
		if (dbus_message_is_signal(m, "org.freedesktop.DBus", "NameAcquired")) {
			tinfo("skipping NameAcquired for ", str(m));
			return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
		}
		tinfo("got msg type: ", dbus_message_get_type(m),
				" path: ", dbus_message_get_path(m),
				" iface: ", dbus_message_get_interface(m),
				" member: ", dbus_message_get_member(m),
				" sender: ", dbus_message_get_sender(m),
				" destination: ", dbus_message_get_destination(m) ? : "none");
		tassert(dbus_message_is_signal(m, this_->signal.iface()(), this_->signal.name()()));

		tassert(strcmp(str(m), this_->_name()) == 0);
		this_->done = true;
		return DBUS_HANDLER_RESULT_HANDLED;
	};
};

/********************** HELPERS THAT ARE NOT USED IN TESTS ******************/
/*
 * getUnownedUniqueId looks for unused unique id on a bus.
 * The requirement is that function should clean after itself fully.
 */
NO_TEMPLATE wur std::string getUnownedUniqueId()
{
	PrivateBus<> bus;
	bus.prepare();

	uint64_t last = -2ULL; // -1 means broadcast
	gboolean found = TRUE;
	std::string uid;

	tinfo("searching for unowned unique id");
	do {
		uid = std::string(":1.") + std::to_string(last);

		DBusError error;
		dbus_error_init(&error);
		found = dbus_bus_name_has_owner(bus.bus().conn, uid.c_str(), &error);
		failIfErr(error, "failed searching for unowned unique id");

		tinfo("checking ", uid, " = ", found?"":"un", "owned");
		last--;
	} while (found);
	bus.done();
	return uid;
}

} // namespace ImplDbus

#endif // TESTDBUS_H
