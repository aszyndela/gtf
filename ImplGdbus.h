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

#ifndef TEST_GDBUS_H
#define TEST_GDBUS_H

#include <cstdlib>
#include <gio/gio.h>
#include <glib.h>
#include <gio/gunixfdlist.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <map>
#include "../lib/testlib.h"
#include "ImplCommon.h"

namespace ImplGdbus {

/*
 * **** BusTypeGetter interface for ImplGdbus. ****
 * **** For use with Setups (client-side) and Services (server-side) ****
 * BusTypeGetter is responsible for determining bus types. Required methods are:
 * 		BusType getBusType()
 *			returns proper bus type
 *
 * Note: this is meant to be used with high-level gdbus interface, which accepts
 *			bus type as a parameter. For lower level interface, we will need
 *			to create connections.
 */

/*
 * BusTypeGetterFromEnv is an implementation of BusTypeGetter interface.
 * It takes data needed to determine the bus type from environment variables.
 */
class BusTypeGetterFromEnv
{
public:
	GBusType getBusType() {
		if (getenv("DBUS_STARTER_BUS_TYPE"))
			return G_BUS_TYPE_STARTER;

		char *busType = getenv("DBUS_BUS_TYPE");
		if (busType != nullptr) {
			if (strcmp(busType, "1") == 0)
				return G_BUS_TYPE_SYSTEM;
			else if (strcmp(busType, "0") == 0)
				return G_BUS_TYPE_SESSION;
		}

		busType = getenv("DBUS_SESSION_BUS_ADDRESS");
		if (busType != nullptr)
			return G_BUS_TYPE_SESSION;
		return G_BUS_TYPE_SYSTEM;
	}
};

NO_TEMPLATE void unref(GVariant *v) { g_variant_unref(v); }
NO_TEMPLATE void unref(GMainLoop *l) { g_main_loop_unref(l); }
NO_TEMPLATE void unref(GMainContext *c) { g_main_context_unref(c); }
NO_TEMPLATE void unref(GDBusProxy *p) { g_object_unref(p); }
NO_TEMPLATE void unref(GDBusNodeInfo *n) { g_dbus_node_info_unref(n); }
NO_TEMPLATE void unref(GDBusConnection *c) { g_object_unref(c); }
NO_TEMPLATE void unref(GUnixFDList *l) { g_object_unref(l); }

/******************************** CLIENT-SIDE ***************************/
/*
 * ClientSetupData is a wrapper for data needed to pass between Setup and SendMethod interfaces (see below).
 */
struct ClientSetupData
{
	GBusType busType;
};

/*
 * **** Setup interface for ImplGdbus. ****
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
 * GetBusTypeSetup is an implementation of Setup interface. Its single purpose
 * is the management of bus connection type.
 */
template <typename BusTypeGetter = BusTypeGetterFromEnv>
class GetBusTypeSetup
{
	BusTypeGetter busTypeGetter;
public:
	ClientSetupData &setup(const testlib::Name &name) {
		_setup.busType = busTypeGetter.getBusType();
		return _setup;
	}
	void tearDown() {}
protected:
	ClientSetupData _setup;
};

/*
 * SetupWaitForName is an implementation of Setup interface. It extends GetBusTypeSetup
 * with waiting for a given bus name on the bus.
 */
template <typename BusTypeGetter = BusTypeGetterFromEnv>
class SetupWaitForName : protected GetBusTypeSetup<BusTypeGetter>
{
public:
	ClientSetupData &setup(const testlib::Name &name) {
		GetBusTypeSetup<BusTypeGetter>::setup(name);

		GMainLoop *loop = g_main_loop_new(NULL, FALSE);
		g_assert_nonnull(loop);

		auto name_appeared_callback =
			[] (GDBusConnection *connection,
					const gchar *name,
					const gchar *name_owner,
					gpointer user_data) {
				g_main_loop_quit(reinterpret_cast<GMainLoop*>(user_data));
			};

		tinfo("waiting for name: ", name);
		guint watcher_id = g_bus_watch_name(GetBusTypeSetup<BusTypeGetter>::_setup.busType, name(),
											G_BUS_NAME_WATCHER_FLAGS_NONE,
											name_appeared_callback,
											NULL, loop,
											NULL);
		g_assert_cmpuint(watcher_id, !=, 0);

		g_main_loop_run(loop);
		tinfo("spotted name: ", name);
		g_bus_unwatch_name(watcher_id);
		unref(loop);

		return GetBusTypeSetup<BusTypeGetter>::_setup;
	}
};

/*
 * **** MessageFeeder interface for ImplGdbus. ****
 * **** For direct use by test scenarios ****
 * MessageFeeder is responsible for messages creation and disposal. Required methods:
 *		Message next()
 *			creates a message
 *			returns the created message
 *		void dispose(Message message)
 *			gets rid of the message
 */

/*
 * There are two types of Messages used in ImplGdbus. In fact they do not strictly represent
 * message types:
 * GVariant * - it represents simple contents of the message
 * VariantWithFds - see definition velow - it represents contents of the messsage along
 *					with list of unix file descriptors - for use with specialized tests
 */
typedef std::pair<GVariant *, GUnixFDList *> VariantWithFds;

/*
 * MessageFeeder is a base class with common operations for implementation of MessageFeeder interface.
 */
class MessageFeeder
{
public:
	void dispose(GVariant *v) {
		unref(v);
	}
	void dispose(VariantWithFds &v) {
		unref(v.first);
		unref(v.second);
	}
};

/*
 * MessageVariantFeeder is an implementation of MessageFeeder interface.
 * It provides simple constant content for messages: a hard-coded string.
 */
class MessageVariantFeeder : public MessageFeeder
{
public:
	GVariant *next(const testlib::TestSpec &) {
		return g_variant_ref_sink(g_variant_new("(s)", "This is message"));
	}
	void payload_size(uint64_t) {} // ignored
private:
};

/*
 * MessageVariantPayloadFeeder is an implementation of MessageFeeder interface.
 * It provides generated payload of given size: an array of bytes.
 */
class MessageVariantPayloadFeeder : public MessageFeeder
{
public:
	GVariant *next(const testlib::TestSpec &)
	{
		if (_payload_size == 0)
			return g_variant_ref_sink(g_variant_new("()"));

		auto array = new unsigned char[_payload_size];
		FORLLZ (k, _payload_size)
			array[k] = k % 10;

		GVariant *farray = g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE,
					array, _payload_size, sizeof(array[0]));
		GVariant *result = g_variant_new_tuple(&farray, 1);

		delete [] array;

		return g_variant_ref_sink(result);
	}
	void payload_size(uint64_t size) { _payload_size = size; }
private:
	uint64_t _payload_size;
};

/*
 * MessageVariantUnixFdsFeeder is an implementation of MessageFeeder interface.
 * It provides constant hard-coded string as a message content
 * as well as list of unix file descriptors array of given size.
 */
class MessageVariantUnixFdsFeeder : public MessageFeeder
{
public:
	VariantWithFds next(const testlib::TestSpec &)
	{
		GUnixFDList *fds = g_unix_fd_list_new();

		auto array = new int[_payload_size];
		FORLLZ (k, _payload_size) {
			int fd = open("/dev/zero", O_RDONLY|O_CLOEXEC);
			tassert(fd != -1);
			GError *error = nullptr;
			tassert(g_unix_fd_list_append(fds, fd, &error) != -1);
			g_assert_no_error(error);
			close(fd);
			array[k] = k;
		}

		GVariant *tuple[2];
		tuple[0] = g_variant_new_string("This is a message with fds");
		tuple[1] = g_variant_new_fixed_array(G_VARIANT_TYPE_HANDLE,
					array, _payload_size, sizeof(array[0]));
		GVariant *result = g_variant_new_tuple(tuple, 2);

		delete [] array;

		return VariantWithFds(g_variant_ref_sink(result), fds);
	}
	void payload_size(uint64_t size) { _payload_size = size; }
private:
	uint64_t _payload_size;
};

/*
 * **** ReplyHandlingMethod interface for ImplGdbus. ****
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
	void handleReply(GVariant *msg, GVariant *reply, GError *error)
	{
		g_assert_no_error(error);
		g_assert_nonnull(reply);
		g_assert_cmpstr(g_variant_get_type_string(msg), ==, g_variant_get_type_string(reply));
		g_assert_true(g_bytes_equal(g_variant_get_data_as_bytes(msg), g_variant_get_data_as_bytes(reply)));
		unref(reply);
	}
};

/*
 * CheckStringReplyHandlingMethod is an implementation of ReplyHandlingMethod interface.
 * It checks if a response contains single argument of string type.
 */
class CheckStringReplyHandlingMethod
{
public:
	void handleReply(GVariant *msg, GVariant *reply, GError *error)
	{
		g_assert_no_error(error);
		g_assert_nonnull(reply);
		g_assert_cmpstr(g_variant_get_type_string(reply), ==, "(s)");
		unref(reply);
	}
};

/*
 * ExpectDBusErrorReplyHandlingMethod is an implementation of ReplyHandlingMethod interface.
 * It checks if the method call failed with a specific error. It is parameterized by error.
 */
template <GDBusError err>
class ExpectDBusErrorReplyHandlingMethod
{
public:
	void handleReply(GVariant *msg, GVariant *reply, GError *error)
	{
		tassert(error->domain == G_DBUS_ERROR);
		tassert(error->code == err);
	}
};

/*
 * **** SendMethod interface for ImplGdbus. ****
 * **** For use directly by test scenarios ****
 * SendMethod is responsible for sending messages. Methods:
 *		void prepareForSending(setup)
 *			takes what it needs for further work
 *		void send(Message message)
 *			executes specified method call (sends message)
 */

/*
   Gdbus sending functions are:
    Generated by gdbus-codegen: names depend on input xml.
   	High-level - GDBusProxy
		g_dbus_proxy_call (proxy, method, params, flags, timeout)
		g_dbus_proxy_call_sync
		g_dbus_proxy_call_with_unix_fd_list
		g_dbus_proxy_call_with_unix_fd_list_sync
	Low-level - GDBusConnection
	- takes GVariant as a message
		g_dbus_connection_call (connection, name/object/iface/method, params, reply_type, flags, timeout)
		g_dbus_connection_call_sync
		g_dbus_connection_call_with_unix_fd_list
		g_dbus_connection_call_with_unix_fd_list_sync
		g_dbus_connection_emit_signal (connection, name/object/iface/signal, params)
	- takes GDBusMessage
		g_dbus_connection_send_message (connection, message, flags)
		g_dbus_connection_send_message_with_reply (connection, message, flags, timeout)
		g_dbus_connection_send_message_with_reply_sync
 */

class SharedBusTypeProxyCreator
{
public:
	GDBusProxy *create(GBusType busType, const testlib::TestSpec &spec)
	{
		GError *error = nullptr;
		GDBusProxy *proxy = g_dbus_proxy_new_for_bus_sync(busType,
				G_DBUS_PROXY_FLAGS_NONE, NULL,
				spec.name(), spec.path(), spec.iface(), NULL, &error);
		g_assert_nonnull(proxy);
		g_assert_no_error(error);
		return proxy;
	}
};

class DefaultPoolSize {};

template <unsigned pool_size>
class SetPoolSize
{
	public:
		SetPoolSize() {
			setenv("KDBUS_MEMORY_POOL_SIZE", std::to_string(pool_size).c_str(), 1);
		}
};

template <typename PoolSize = DefaultPoolSize>
class PrivateBusTypeProxyCreator
{
public:
	GDBusProxy *create(GBusType busType, const testlib::TestSpec &spec)
	{
		// set the pool size
		PoolSize poolSize;

		GError *error = nullptr;
		this->_connection = g_dbus_connection_new_for_address_sync(
								g_dbus_address_get_for_bus_sync(busType, NULL, &error),
								static_cast<GDBusConnectionFlags>(
									G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION
									| G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT),
								NULL, NULL, NULL);
		tassert(this->_connection);
		g_assert_no_error(error);
		g_dbus_connection_set_exit_on_close(this->_connection, FALSE);

		GDBusProxy *proxy = g_dbus_proxy_new_sync(this->_connection,
				G_DBUS_PROXY_FLAGS_NONE, NULL,
				spec.name(), spec.path(), spec.iface(), NULL, &error);
		g_assert_nonnull(proxy);
		g_assert_no_error(error);
		return proxy;
	}
	~PrivateBusTypeProxyCreator()
	{
		unref(this->_connection);
	}
private:
	GDBusConnection *_connection;
};

/*
 * SimpleProxyMethod is a base abstract implementation of SendMethod interface.
 * It lacks actual sending method but prepares everything to be ready for sending
 * with GDBusProxy.
 * It is parameterized by its template argument ReplyHandlingMethod.
 * ReplyHandlingMethod must be an implementation of ReplyHandlingMethod interface.
 */
template <typename ReplyHandlingMethod = CheckStringReplyHandlingMethod, typename ProxyCreator = SharedBusTypeProxyCreator>
class SimpleProxySendMethod
{
protected:
	ReplyHandlingMethod replyhandlingMethod;
	ProxyCreator proxyCreator;
public:
	void prepareForSending(const ClientSetupData &setup, const testlib::TestSpec &spec)
	{
		proxy = proxyCreator.create(setup.busType, spec);

		member = spec.member;
		_flags = spec.no_auto_start() ? G_DBUS_CALL_FLAGS_NO_AUTO_START : G_DBUS_CALL_FLAGS_NONE;
	}
	~SimpleProxySendMethod()
	{
		unref(proxy);
	}
	void switchProxy(GDBusProxy *newproxy)
	{
		unref(proxy);
		proxy = newproxy;
	}
	GDBusProxy *getProxy() { return proxy; }
protected:
	GDBusProxy *proxy;
	testlib::Member member;
	GDBusCallFlags _flags;
};

/*
 * SimpleProxySyncSendMethod is an implementation of SendMethod interface.
 * It uses GDBusProxy to send passed messages synchronously and handle replies.
 * It is parameterized by its template argument ReplyHandlingMethod.
 * ReplyHandlingMethod must be an implementation of ReplyHandlingMethod interface.
 */
template <typename ReplyHandlingMethod = CheckStringReplyHandlingMethod, typename ProxyCreator = SharedBusTypeProxyCreator>
class SimpleProxySyncSendMethod : public SimpleProxySendMethod<ReplyHandlingMethod, ProxyCreator>
{
public:
	void send(GVariant *msg, GUnixFDList *fds = nullptr)
	{
		GError *error = nullptr;
		GVariant *reply = g_dbus_proxy_call_with_unix_fd_list_sync(this->proxy, this->member(),
				msg, this->_flags, -1, fds, NULL, NULL, &error);
		this->replyhandlingMethod.handleReply(msg, reply, error);
	}
	void send(const VariantWithFds &p)
	{
		send(p.first, p.second);
	}
};

/*
 * SimpleProxyAsyncSendMethod is an implementation of SendMethod interface.
 * It uses GDBusProxy to send passed messages synchronously and handle replies.
 * It is parameterized by its template argument ReplyHandlingMethod.
 * ReplyHandlingMethod must be an implementation of ReplyHandlingMethod interface.
 */
template <typename ReplyHandlingMethod = CheckStringReplyHandlingMethod, typename ProxyCreator = SharedBusTypeProxyCreator>
class SimpleProxyAsyncSendMethod : public SimpleProxySendMethod<ReplyHandlingMethod, ProxyCreator>
{
	GMainLoop *_loop;
	GMainContext *_context;
	GVariant *_currentMessage;
public:
	void prepareForSending(const ClientSetupData &setup,
			const testlib::TestSpec &spec)
	{
		SimpleProxySendMethod<ReplyHandlingMethod>::prepareForSending(setup, spec);

		_context = g_main_context_new();
		g_assert_nonnull(_context);

		_loop = g_main_loop_new(_context, FALSE);
		g_assert_nonnull(_loop);
	}

	void send(GVariant *msg, GUnixFDList *fds = nullptr)
	{
		auto callback = [] (GObject *src, GAsyncResult *res, gpointer user_data) {
			auto this_ = THIS(user_data);
			GError *error = nullptr;

			auto *reply = g_dbus_proxy_call_finish(G_DBUS_PROXY(src), res, &error);
			if (!reply) {
				tfail("Error getting reply: ", error->message);
				g_error_free(error);
				return;
			}

			this_->replyhandlingMethod.handleReply(this_->_currentMessage, reply, error);

			g_main_loop_quit(this_->_loop);
		};

		g_main_context_push_thread_default (this->_context);

		this->_currentMessage = msg;

		g_dbus_proxy_call_with_unix_fd_list(this->proxy, this->member(),
				msg, this->_flags, -1, fds, NULL, callback, this);

		g_main_loop_run(this->_loop);

		g_main_context_pop_thread_default (this->_context);
	}
	void send(const VariantWithFds &p)
	{
		send(p.first, p.second);
	}
	~SimpleProxyAsyncSendMethod()
	{
		unref(this->_loop);
		unref(this->_context);
	}
};

/*
 * SimpleProxySyncUniqueIdSendMethod is an implementation of SendMethod interface.
 * It uses GDBusProxy to send passed messages synchronously and handle replies.
 * Destination for messages is set as unique id for the owner of the passed well-known name.
 * It is parameterized by its template argument ReplyHandlingMethod.
 * ReplyHandlingMethod must be an implementation of ReplyHandlingMethod interface.
 */
template <typename ReplyHandlingMethod = CheckStringReplyHandlingMethod>
class SimpleProxySyncUniqueIdSendMethod
{
	SimpleProxySyncSendMethod<ReplyHandlingMethod> sendMethod;
public:
	void prepareForSending(const ClientSetupData &setup,
			const testlib::TestSpec &spec)
	{
		sendMethod.prepareForSending(setup, spec);
		GError *error = nullptr;
		GDBusProxy *proxy = g_dbus_proxy_new_for_bus_sync(setup.busType,
				G_DBUS_PROXY_FLAGS_NONE, NULL,
				g_dbus_proxy_get_name_owner(sendMethod.getProxy()),
				spec.path(), spec.iface(), NULL, &error);
		g_assert_nonnull(proxy);
		g_assert_no_error(error);
		sendMethod.switchProxy(proxy);
	}
	void send(GVariant *msg, GUnixFDList *fds = nullptr)
	{
		sendMethod.send(msg, fds);
	}
};

/*
 * SignalSendMethod is an implementation of SendMethod interface.
 * It sends signals.
 */
class SignalSendMethod
{
public:
	void prepareForSending(const ClientSetupData &setup,
			const testlib::TestSpec &spec)
	{
		_conn = g_bus_get_sync(setup.busType, NULL, NULL);
		tassert(_conn);
		_spec = &spec;
	}
	void send(GVariant *msg)
	{
		GError *error = nullptr;
		tassert(g_dbus_connection_emit_signal(_conn, NULL, _spec->path(), _spec->iface(),
				_spec->member(), msg, &error));
		g_assert_no_error(error);
		tassert(g_dbus_connection_flush_sync(_conn, NULL, NULL));
	}
	~SignalSendMethod()
	{
		unref(_conn);
	}
private:
	GDBusConnection *_conn;
	const testlib::TestSpec *_spec;
};

/******************************** SERVER-SIDE ***************************/
/*
 * ServiceSetupData is a wrapper for data needed to pass between Service and Dispatcher
 * interfaces (see below).
 */
struct ServiceSetupData
{
	GMainLoop *loop;
	unsigned processed;
};

/*
 * **** Reaction interface for ImplGdbus. ****
 * **** For use by Services ****
 * Reaction is responsible for reacting on incoming messages. Methods:
 *		ReplyMessage react(Message message)
 *			acts appropriately for incoming message
 *			returns a reply message
 *
 * For gdbus, message's type is GDBusMethodInvocation, which contains
 * all the message data needed to react.
 * ReplyMessages type used is GVariant *.
 */
/*
 * RespondOnceReaction is an implementation of Reaction interface.
 * It accepts a message and provides constant hard-coded string content as a reply.
 * It also counts incoming messages.
 */
class RespondOnceReaction : public ImplCommon::Counter
{
public:
	GVariant *react(GDBusMethodInvocation *invocation, const testlib::TestSpec &spec)
	{
		updateCounter();
		return g_variant_new("(s)", "This is a reply");
	}
};

/*
 * JustCountReaction is an implementation of Reaction interface.
 * It counts incoming messages.
 */
class JustCountReaction : public ImplCommon::Counter
{
public:
	GVariant *react(GDBusMethodInvocation *invocation, const testlib::TestSpec &spec)
	{
		updateCounter();
		return nullptr;
	}
	void react(GVariant *parameters, const testlib::TestSpec &spec)
	{
		updateCounter();
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
	GVariant *react(GDBusMethodInvocation *invocation, const testlib::TestSpec &spec)
	{
		tassert(0 && "Unexpected message received");
		return nullptr;
	}
};

/*
 * CheckUnixFdsAndRespondReaction is an implementation of Reaction interface.
 * It accepts a message, gets unix file descriptors from the message and checks
 * if they are valid.
 * It does not provide response and does not count incoming messages.
 * If needed, combine it with another Reaction using ReactionAnd.
 */
class CheckUnixFdsReaction
{
public:
	void react(GDBusMethodInvocation *invocation, const testlib::TestSpec &spec)
	{
		GDBusMessage *msg = g_dbus_method_invocation_get_message(invocation);
		g_assert_nonnull(msg);

		GUnixFDList *fdlist = g_dbus_message_get_unix_fd_list(msg);
		const gint *fds = nullptr;
		gint nfds = 0;
		if (spec.payload_size() > 0) {
			g_assert_nonnull(fdlist);
			fds = g_unix_fd_list_peek_fds(fdlist, &nfds);
		} else {
			g_assert_null(fdlist);
		}

		g_assert_cmpuint(g_dbus_message_get_num_unix_fds(msg), ==, (guint)nfds);

		FORZ(k, (unsigned)nfds) {
			char c;
			tassert(read(fds[k], &c, 1) == 1);	// read one byte from /dev/zero
			tassert(c == 0);				// check it is zero
		}
	}
};

/*
 * CheckPayloadReaction is an implementation of Reaction interface.
 * It accepts a message and checks if its payload is valid payload
 * as generated by MessageVariantPayloadFeeder.
 * It does not provide response and does not count incoming messages.
 * If needed, combine it with another Reaction using ReactionAnd.
 */
class CheckPayloadReaction
{
public:
	void react(GDBusMethodInvocation *invocation, const testlib::TestSpec &spec)
	{
		GVariant *params = g_dbus_method_invocation_get_parameters(invocation);
		g_assert_nonnull(params);
		react(params, spec);
	}
	void react(GVariant *params, const testlib::TestSpec &spec)
	{
		if (spec.payload_size() > 0) {
			gsize data_size;
			const gchar *data = reinterpret_cast<gchar*>(
					g_bytes_unref_to_data(g_variant_get_data_as_bytes(params), &data_size));
			g_assert_cmpint(spec.payload_size(), ==, data_size);

			FORZ(k, data_size) {
				g_assert_cmpint(data[k], ==, k % 10);
			}
		}
		else
		{
			g_assert_cmpstr(g_variant_get_type_string(params), ==, "()");
		}
	}
};

/*
 * ReactionAnd is an implementation of Reaction interface.
 * It is template class which makes composite reaction from two reactions.
 * It is parameterized by two reactions T1 and T2 (implementations of Reaction interface).
 * The return value is taken from T2.
 * Note: it may be extended to accept any number of reactions, if needed.
 */
template <typename T1, typename T2>
class ReactionAnd : public ImplCommon::Counter
{
	T1 t1;
	T2 t2;
public:
	GVariant *react(GDBusMethodInvocation *invocation, testlib::TestSpec &spec)
	{
		updateCounter();
		t1.react(invocation, spec);
		return t2.react(invocation, spec);
	}
	void react(GVariant *parameters, const testlib::TestSpec &spec)
	{
		updateCounter();
		t1.react(parameters, spec);
		t2.react(parameters, spec);
	}
};

/*
 * **** InterfaceCreator interface for ImplGdbus. ****
 * **** For use by SingleMethodService ****
 * InterfaceCreator is responsible for creating interface data (GDBusNodeInfo).
 *		InterfaceData create()
 *			creates interface data
 *			returns the data
 */
/*
 * SingleMethodIfaceCreator is a base class for common operations of InterfaceCreators.
 */
class SingleMethodIfaceCreator
{
public:
	template<class ...Args>
	auto create(const testlib::TestSpec &method, Args ...args)
	{
		std::string xml = "<node>"
				"  <interface name='" + std::string(method.iface()) + "'>"
				"	<method name='" + std::string(method.member()) + "'>";

		std::initializer_list<int>{((void)(
					xml += "<arg type='" + std::string(args) + "' name ='"
							+ std::string(args) + "_in"
							"' direction='in'/>"), 0)...};

		xml +=	"      <arg type='s' name='string_out' direction='out'/>"
				"    </method>"
				"  </interface>"
				"</node>";

		GDBusNodeInfo *introspection_data = g_dbus_node_info_new_for_xml(xml.c_str(), NULL);
		g_assert_nonnull(introspection_data);

		return introspection_data;
	}
};

/*
 * GeneratedPayloadIfaceCreator is an implementation of InterfaceCreator.
 * It provides interface data for a method accepting array of bytes - or nothing - depending
 * on given payload size.
 */
class GeneratedPayloadIfaceCreator : SingleMethodIfaceCreator
{
public:
	auto create(const testlib::TestSpec &method)
	{
		if (method.payload_size() > 0)
			return SingleMethodIfaceCreator::create(method, "ay");
		else
			return SingleMethodIfaceCreator::create(method);
	}
};

/*
 * SingleStringMethodIfaceCreator is an implementation of InterfaceCreator.
 * It provides interface data for a method accepting a single string.
 */
class SingleStringMethodIfaceCreator : SingleMethodIfaceCreator
{
public:
	auto create(const testlib::TestSpec &method)
	{
		return SingleMethodIfaceCreator::create(method, "s");
	}
};

/*
 * SingleStringMethodIfaceCreator is an implementation of InterfaceCreator.
 * It provides interface data for a method accepting two arguments: a single string,
 * and an array of handles (for passing unix file descriptors).
 */
class StringUnixFdsMethodIfaceCreator : SingleMethodIfaceCreator
{
public:
	auto create(const testlib::TestSpec &method)
	{
		return SingleMethodIfaceCreator::create(method, "s", "ah");
	}
};

/*
 * **** Service interface for ImplGdbus. ****
 * **** For use directly by test scenarios ****
 * Service is responsible for receiving messages and reacting on them. Methods:
 *		ServiceSetupData setup()
 *			prepares service for receiving messages
 *			returns ServiceSetupData filled with prepared values
 *		void tearDown()
 *			closes service, cleans up.
 */

/*
 * SingleMethodService is an implementation of Service interface.
 * It provides a service with given well-known name and object at given location,
 * and registers handlers for incoming messages.
 * It is parameterized by InterfaceCreator, Reaction and BusTypeGetter template arguments.
 * InterfaceCreator must be an implementation of InterfaceCreator interface.
 * Reaction must be an implementation of Reaction interface.
 * BusTypeGetter must be an implementation of BusTypeGetter interface.
 */
template <typename InterfaceCreator = SingleStringMethodIfaceCreator,
		  typename Reaction = RespondOnceReaction,
		  typename BusTypeGetter = BusTypeGetterFromEnv>
class SingleMethodService
{
	BusTypeGetter busTypeGetter;
	Reaction reaction;
	InterfaceCreator interfaceCreator;
public:
	ServiceSetupData &setup(testlib::TestSpec &spec, unsigned served_messages = 1)
	{
		reaction.counter(served_messages);

		method = &spec;

		registerNameAndObject();

		return _setup;
	}
	void tearDown()
	{
		g_bus_unown_name(name_owning_id);
		unref(_setup.loop);
	}
private:
	void registerNameAndObject()
	{
		_setup.loop = g_main_loop_new(NULL, FALSE);
		g_assert_nonnull(_setup.loop);

		auto bus_acquired_handler = [] (GDBusConnection *connection,
										const gchar *name,
										gpointer user_data) {
			THIS(user_data)->registerMethod(connection);
		};

		auto name_acquired_handler = [] (GDBusConnection *connection,
										const gchar *name,
										gpointer user_data) {
			g_main_loop_quit(THIS(user_data)->_setup.loop);
		};

		auto name_lost_handler = [] (GDBusConnection *connection,
										const gchar *name,
										gpointer user_data) {
			tfail("unexpected name loss:", name);
		};

		name_owning_id = g_bus_own_name(busTypeGetter.getBusType(),
				method->name(), G_BUS_NAME_OWNER_FLAGS_NONE,
				bus_acquired_handler,
				name_acquired_handler,
				name_lost_handler, this, NULL);

		g_main_loop_run(_setup.loop);
	}
	void registerMethod(GDBusConnection *connection)
	{
		auto introspection_data = interfaceCreator.create(*method);

		const GDBusInterfaceVTable vtable = {
			[] (GDBusConnection *connection,
					const gchar *sender,
					const gchar *object_path,
					const gchar *interface_name,
					const gchar *method_name,
					GVariant *parameters,
					GDBusMethodInvocation *invocation,
					gpointer user_data) {
				auto this_ = THIS(user_data);

				if (g_strcmp0(method_name, this_->method->member()) == 0)
				{
					this_->_setup.processed++;
					GVariant *reply = this_->reaction.react(invocation, *this_->method);
					// may return floating ref, will be consumed below
					if (reply != nullptr)
						g_dbus_method_invocation_return_value(invocation, reply);	// reply consumed
				}

				if (this_->reaction.finish())
					g_main_loop_quit(this_->_setup.loop);
			}
		};

		registration_id = g_dbus_connection_register_object(connection, method->path(),
					introspection_data->interfaces[0], &vtable, this, NULL, NULL);
		g_assert_cmpuint(registration_id, !=, 0);

		unref(introspection_data);
	}
	ServiceSetupData _setup;
	guint name_owning_id;
	guint registration_id;
	testlib::TestSpec *method;
};

/*
 * ListenToSignalsService is an implementation of Service interface.
 * It registers for receiving given signals.
 * It also provides a bus service with given well-known name for means of synchronization with Senders.
 * It is parameterized by Reaction and BusTypeGetter template arguments.
 * Reaction must be an implementation of Reaction interface.
 * BusTypeGetter must be an implementation of BusTypeGetter interface.
 */
// FIXME: This is not nice - it "shares" too much with previous class
template <typename Reaction = JustCountReaction,
		  typename BusTypeGetter = BusTypeGetterFromEnv>
class ListenToSignalService
{
	Reaction reaction;
	BusTypeGetter busTypeGetter;
public:
	ServiceSetupData &setup(const testlib::TestSpec &spec, unsigned served_messages = 1)
	{
		reaction.counter(served_messages);

		_spec = &spec;

		_setup.loop = g_main_loop_new(NULL, FALSE);
		g_assert_nonnull(_setup.loop);

		auto bus_acquired_handler = [] (GDBusConnection *connection,
										const gchar *name,
										gpointer user_data) {
			THIS(user_data)->subscribe(connection);
		};

		auto name_acquired_handler = [] (GDBusConnection *connection,
										const gchar *name,
										gpointer user_data) {
			g_main_loop_quit(THIS(user_data)->_setup.loop);
		};

		auto name_lost_handler = [] (GDBusConnection *connection,
										const gchar *name,
										gpointer user_data) {
			tfail("unexpected name loss:", name);
		};

		name_owning_id = g_bus_own_name(busTypeGetter.getBusType(),
				_spec->name(), G_BUS_NAME_OWNER_FLAGS_NONE,
				bus_acquired_handler,
				name_acquired_handler,
				name_lost_handler, this, NULL);

		g_main_loop_run(_setup.loop);

		return _setup;
	}
	void tearDown()
	{
		g_bus_unown_name(name_owning_id);
		unref(_setup.loop);
	}
private:
	void subscribe(GDBusConnection *connection)
	{
		auto signal_handler = [] (GDBusConnection *c, const gchar *sender_name,
									const gchar *path, const gchar *iface, const gchar *signal,
									GVariant *parameters,
									gpointer user_data)
								{
									auto this_ = THIS(user_data);
									if (g_strcmp0(signal, this_->_spec->member()) == 0)
									{
										this_->_setup.processed++;
										this_->reaction.react(parameters, *this_->_spec);
									}

									if (this_->reaction.finish()) {
										g_main_loop_quit(this_->_setup.loop);
										g_dbus_connection_signal_unsubscribe(c, this_->registration_id);
									}
								};

		registration_id = g_dbus_connection_signal_subscribe(connection, NULL,
				_spec->iface(), _spec->member(), NULL, _spec->arg0()(),
				G_DBUS_SIGNAL_FLAGS_NONE, signal_handler, this, NULL);
		g_assert_cmpuint(registration_id, !=, 0);
	}
	ServiceSetupData _setup;
	guint name_owning_id;
	guint registration_id;
	const testlib::TestSpec *_spec;
};

/*
 * **** Dispatcher interface for ImplGdbus. ****
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
	void dispatch(ServiceSetupData &setup)
	{
		g_main_loop_run(setup.loop);
	}
	unsigned dispatchOnce(ServiceSetupData &setup)
	{
		setup.processed = 0;
		while (0 == setup.processed) {
			g_main_context_iteration(g_main_loop_get_context(setup.loop), TRUE);
		}
		return setup.processed;
	}
};

/**************** Well-known names management ********************/
/*
 * ConnectionSetupData is a wrapper for data needed to keep
 * by NameOperations (see below).
 */
struct ConnectionSetupData
{
	GDBusConnection *conn;
	std::map<std::string, guint> own_registrations;
};

/*
 * **** Bus interface for ImplGdbus. ****
 * **** For use by NameOperations and Waiters ****
 * Bus is responsible for controlling timelife of bus connection. Methods:
 *		void prepare(setup)
 *			creates connection
 *		void done()
 *			closes connection
 */

/*
 * ABus is a base class for all the Gdbus implementations of Bus interface.
 * It is parameterized by BusTypeGetter, which selects type of bus and provides bus address.
 */
template <typename BusTypeGetter = BusTypeGetterFromEnv>
class ABus : public ImplCommon::ARealBus<ConnectionSetupData>
{
protected:
	BusTypeGetter busTypeGetter;
	using ARealBus<ConnectionSetupData>::prepare;
};

/*
 * SharedBus is a class that controls timelife of shared bus connection.
 * Shared bus connection is a connection that may be used by other users of glib.
 */
template <typename BusTypeGetter = BusTypeGetterFromEnv>
class SharedBus : public ABus<BusTypeGetter>
{
public:
	using ABus<BusTypeGetter>::prepare;
	void prepare(ConnectionSetupData &setup)
	{
		this->_setup.conn = g_bus_get_sync(this->busTypeGetter.getBusType(), NULL, NULL);
		tassert(this->_setup.conn);
		g_dbus_connection_set_exit_on_close(this->_setup.conn, FALSE);
	}
	void done()
	{
		unref(this->_setup.conn);
	}
};

/*
 * PrivateBus is a class that controls timelife of private bus connection.
 * Private bus connection is a connection that no one else can use.
 */
template <typename BusTypeGetter = BusTypeGetterFromEnv>
class PrivateBus : public ABus<BusTypeGetter>
{
public:
	using ABus<BusTypeGetter>::prepare;
	void prepare(ConnectionSetupData &setup)
	{
		GError *error = nullptr;
		tinfo("getting private bus");
		this->_setup.conn = g_dbus_connection_new_for_address_sync(
							g_dbus_address_get_for_bus_sync (this->busTypeGetter.getBusType(), NULL, &error),
							static_cast<GDBusConnectionFlags>(
								G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION
								| G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT),
							NULL, NULL, NULL);
		tassert(this->_setup.conn);
		g_assert_no_error(error);
		g_dbus_connection_set_exit_on_close(this->_setup.conn, FALSE);
	}
	void done()
	{
		tinfo("freeing private bus");
		tassert(g_dbus_connection_close_sync(this->_setup.conn, NULL, NULL));
		unref(this->_setup.conn);
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

/*
 * **** NameOperation interface for ImplGdbus. ****
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
		_loop = g_main_loop_new(NULL, FALSE);
		g_assert_nonnull(_loop);

		auto name_acquired_handler = [] (GDBusConnection *connection,
										const gchar *name,
										gpointer user_data) {
			THIS(user_data)->nameOperationResult.nameAcquired(name);
			g_main_loop_quit(THIS(user_data)->_loop);
		};

		auto name_lost_handler = [] (GDBusConnection *connection,
										const gchar *name,
										gpointer user_data) {
			THIS(user_data)->nameOperationResult.nameLost(name);
			g_main_loop_quit(THIS(user_data)->_loop);
		};

		bus.bus().own_registrations[_name()] = g_bus_own_name_on_connection(bus.bus().conn,
				_name(), G_BUS_NAME_OWNER_FLAGS_NONE,
				name_acquired_handler,
				name_lost_handler, this, NULL);

		g_main_loop_run(_loop);
	}
	void finish()
	{
		bus.done();
	}
private:
	testlib::Name _name;
	GMainLoop *_loop;
};

/*
 * ReleaseNameOperation is implementation of NameOperation interface
 * It releases previously requested bus name.
 * It is parameterized by integer, to allow multiple instances of the operation
 * to be specified.
 */
template <int instance = 0>
class ReleaseNameOperation
{
public:
	void prepare(const testlib::Name name, ConnectionSetupData &setup)
	{
		_name = name;
		_setup = &setup;
	}
	void execute()
	{
		auto it = _setup->own_registrations.find(_name());
		tassert(it != _setup->own_registrations.end());
		g_bus_unown_name(it->second);
		_setup->own_registrations.erase(it);
		tinfo("released name ", _name());
	}
	void finish() {}
private:
	testlib::Name _name;
	ConnectionSetupData *_setup;
};

/*
 * NameOperations is a type container for set of NameOperations, specialized for
 * ImplGdbus::ConnectionSetupData.
 */
template <typename Bus, class ...Args>
using NameOperations = ImplCommon::NameOperations<Bus, ConnectionSetupData, Args...>;

/*
 * **** Waiter interface for ImplGdbus. ****
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
class NameSignalWaiter
{
	Bus bus;
	Signal signal;
public:
	void setup(testlib::Name name)
	{
		_name = name;
		countReceived = 0;
		countExpected = 0;
		bus.prepare();
		loop = g_main_loop_new(NULL, FALSE);
		registration_id = g_dbus_connection_signal_subscribe(bus.bus().conn, NULL,
				signal.iface()(), signal.name()(), NULL, name(),
				G_DBUS_SIGNAL_FLAGS_NONE, signal_handler, this, NULL);
	}
	void expectSignals()
	{
		/* g_main_loop_run may dispatch more than one signal at once
		 * so we need to count signals, and run the loop only when we expect more signals
		 * than already received.
		 */
		if (countExpected >= countReceived)
			g_main_loop_run(loop);
		else
			tinfo("signals already received, just count");
		countExpected++;
	}
	void tearDown()
	{
		g_dbus_connection_signal_unsubscribe(bus.bus().conn, registration_id);
		unref(loop);
		bus.done();
	}
private:
	static void signal_handler (GDBusConnection *c, const gchar *sender_name,
			const gchar *path, const gchar *iface, const gchar *signal,
			GVariant *parameters,
			gpointer user_data)
	{
		auto this_ = reinterpret_cast<NameSignalWaiter<Bus,Signal>*>(user_data);
		tassert(g_strcmp0(signal, this_->signal.name()()) == 0);

		const gchar *value_str;
		g_variant_get (parameters, "(&sss)", &value_str, NULL, NULL);
		tassert(g_strcmp0(value_str, this_->_name()) == 0);

		g_main_loop_quit(this_->loop);
		this_->countReceived++;
	};

	GMainLoop *loop;
	guint registration_id;
	testlib::Name _name;
	int countReceived;
	int countExpected;
};

/*
 * **** Synchronizer interface for ImplGdbus. ****
 * **** For use directly by test scenarios ****
 * Synchronizer is responsible for synchronizing two processes. Methods:
 *		void synchronizeClient(Name name)
 *			meant for use by clients; waits until service is ready, and signals
 *			that the client is ready
 *		void synchronizeService(Name name)
 *			meant for use by services; waits until client is ready
 */
/*
 * Synchronizer is an implementation of Synchronizer interface.
 * It generates well-known bus name based on a given name, and uses
 * it for synchronizing client and service.
 * It also uses hard-coded path, interface and member.
 * It is parameterized by its template argument BusTypeGetter.
 * BusTypeGetter must be an implementation of BusTypeGetter interface.
 */
template <class BusTypeGetter = BusTypeGetterFromEnv>
class Synchronizer
{
public:
	Synchronizer()
		: spec(name_, path_, iface_, member_),
		path_("/Test1/MainService/Sync"),
		iface_("com.samsung.TestService.Sync"),
		member_("SyncSignal")
	{}

	void synchronizeClient(testlib::Name name)
	{
		syncName(name);
		tinfo("synchronizing client on ", spec.name());

		// wait for synchronization name
		SetupWaitForName<BusTypeGetter> setupper;
		auto setup = setupper.setup(spec.name);

		// peer is present on the bus, let's give him a signal
		SignalSendMethod signalSendMethod;
		signalSendMethod.prepareForSending(setup, spec);

		MessageVariantFeeder feeder;
		auto msg = feeder.next(spec);

		signalSendMethod.send(msg);
		feeder.dispose(msg);
	}
	void synchronizeService(testlib::Name name)
	{
		syncName(name);
		tinfo("synchronizing service on ", spec.name());

		ListenToSignalService<JustCountReaction, BusTypeGetter> service;
		auto &setup = service.setup(spec);

		Dispatcher().dispatchOnce(setup);
		service.tearDown();
	}
private:
	std::string sn;
	testlib::TestSpec spec;
	testlib::Name name_;
	testlib::Path path_;
	testlib::Iface iface_;
	testlib::Member member_;

	inline void syncName(testlib::Name name)
	{
		sn = name();
		sn += ".sync";
		name_ = testlib::Name(sn);
	}
};

/*
 * SynchronizerConst is an implementation of Synchronizer interface.
 * It is an extension of Synchronizer implementation.
 * It uses hard-coded well-known name for synchronizing client and service.
 */
template <class BusTypeGetter = BusTypeGetterFromEnv>
class SynchronizerConst : private Synchronizer<BusTypeGetter>
{
	const testlib::Name constName;
public:
	SynchronizerConst()
		: constName("com.samsung.TestService.SynchronizationService")
	{}
	void synchronizeClient(testlib::Name name)
	{
		Synchronizer<BusTypeGetter>::synchronizeClient(constName);
	}
	void synchronizeService(testlib::Name name)
	{
		Synchronizer<BusTypeGetter>::synchronizeService(constName);
	}
};

} // namespace ImplGdbus

#endif // TEST_GDBUS_H

