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
#ifndef TESTLIB_SUGAR_H
#define TESTLIB_SUGAR_H

#include <cstdlib>
#include <cstring>
#include <assert.h>

#define UNPAREN(...) __VA_ARGS__

#define wur __attribute__((warn_unused_result))
#define noret __attribute__((noreturn))

#define STRINGIFY2(...) #__VA_ARGS__
#define STRINGIFY(...) STRINGIFY2(__VA_ARGS__)
#define CAT2(A,B) A##B
#define CAT(A,B) CAT2(A,B)

#define FORZ(VAR,COUNT) for (unsigned VAR = 0; VAR < (COUNT); ++VAR)
#define FORLLZ(VAR,COUNT) for (uint64_t VAR = 0; VAR < (COUNT); ++VAR)
#define TIMES(COUNT) FORZ(CAT(_timesVar_dummy_,__LINE__), COUNT)
#define TIMESLLZ(COUNT) FORLLZ(CAT(_timesVar_dummy_,__LINE__), COUNT)

#define NO_TEMPLATE template <class...DUMMIES_FOR_DUMMIES, class ENABLE_FOR_DUMMIES=typename ::std::enable_if<!sizeof...(DUMMIES_FOR_DUMMIES)>::type>
#define RETURN(...) -> decltype(__VA_ARGS__) { return (__VA_ARGS__); }
#define D_FUNC(NAME_AND_ARGS,...) constexpr auto NAME_AND_ARGS RETURN(__VA_ARGS__)

namespace testlibdetail { template <class T, size_t S> char (&ArraySizeHelper__(T const (&)[S]))[S]; }
#define TABSIZE(...) (sizeof(::testlibdetail::ArraySizeHelper__(__VA_ARGS__)))

#define D_STRINGTYPE(NAME) class NAME {\
	char const *d;\
public:\
	explicit NAME(char const *s = nullptr) : d(s) {}\
	explicit NAME(const std::string &s) : d(s.c_str()) {}\
	explicit operator bool() const { return d; }\
	char const *operator()() const { return d; }\
	bool operator==(NAME const &s) const { assert(d); assert(s.d); return !strcmp(d, s.d); }\
};\
NO_TEMPLATE void operator<< (std::ostream &stream, const NAME &s) {\
	stream << s(); \
}

#define D_STRINGVAR(TYPE,NAME,CONTENTS)\
	namespace detail { char CAT(STRINGVAR_,NAME)[] = CONTENTS; };\
	TYPE NAME(detail::CAT(STRINGVAR_,NAME));

#define THIS(x) reinterpret_cast<decltype(this)>(x)

#endif // TESTLIB_SUGAR_H
