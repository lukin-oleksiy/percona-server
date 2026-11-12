/* Copyright (c) 2026 Percona LLC and/or its affiliates. All rights
   reserved.

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License
   as published by the Free Software Foundation; version 2 of
   the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA */

/**
  @file components/test/test_lang_backend.h

  Trivial "language" backends used to test routing of external stored
  programs between several language components (see component_lang_router).

  Semantics of a routine in a test language:
    - FUNCTION with at least one argument returns its first argument as a
      string, transformed by the language (ECHO: as is, SHOUT: upper-cased).
      The first argument may be a string or a signed integer type.
      NULL argument gives NULL.
    - FUNCTION without arguments returns its own body, transformed.
    - PROCEDURE whose first parameter is OUT or INOUT (string type) sets it to
      its own body, transformed, so a test can see which backend ran the
      CALL. Any other PROCEDURE does nothing.
    - Body containing "SYNTAX_ERROR" fails at parse (CREATE) time.
    - Body containing "RUNTIME_ERROR" fails at execution time.
  Libraries (only in components that provide the external_library services)
  accept any body which does not contain "SYNTAX_ERROR".

  Every test backend also supports the shared language TWIN (with its own
  semantics), so tests can make two backends claim one language.

  The implementation is shared by all test languages; each component
  provides a Traits struct and its own service/component declarations.
*/

#ifndef COMPONENTS_TEST_TEST_LANG_BACKEND_H
#define COMPONENTS_TEST_TEST_LANG_BACKEND_H

#include <strings.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <string>

#include <mysql/components/component_implementation.h>
#include <mysql/components/service_implementation.h>
#include <mysql/components/services/bits/stored_program_bits.h>
#include <mysql/components/services/language_service.h>
#include <mysql/components/services/mysql_runtime_error_service.h>
#include <mysql/components/services/mysql_stored_program.h>
#include <mysqld_error.h>

REQUIRES_SERVICE_PLACEHOLDER(mysql_runtime_error);
REQUIRES_SERVICE_PLACEHOLDER(mysql_stored_program_metadata_query);
REQUIRES_SERVICE_PLACEHOLDER(mysql_stored_program_argument_metadata_query);
REQUIRES_SERVICE_PLACEHOLDER(mysql_stored_program_runtime_argument_string);
REQUIRES_SERVICE_PLACEHOLDER(mysql_stored_program_runtime_argument_int);
REQUIRES_SERVICE_PLACEHOLDER(mysql_stored_program_return_value_string);
REQUIRES_SERVICE_PLACEHOLDER(mysql_stored_program_return_value_null);

/* Services every test language component must list in its REQUIRES. */
#define TEST_LANG_BACKEND_REQUIRES                                    \
  REQUIRES_SERVICE(mysql_runtime_error),                              \
      REQUIRES_SERVICE(mysql_stored_program_metadata_query),          \
      REQUIRES_SERVICE(mysql_stored_program_argument_metadata_query), \
      REQUIRES_SERVICE(mysql_stored_program_runtime_argument_string), \
      REQUIRES_SERVICE(mysql_stored_program_runtime_argument_int),    \
      REQUIRES_SERVICE(mysql_stored_program_return_value_string),     \
      REQUIRES_SERVICE(mysql_stored_program_return_value_null)

template <typename Traits>
class Test_lang_backend {
  struct Routine {
    uint16_t type;
    uint32_t arg_count{0};
    bool first_is_int{false};  // first argument has a signed integer type
    bool first_is_out{false};  // first parameter is OUT or INOUT
    std::string body;
  };

  static bool is_int_type(uint64_t sql_type) {
    return (sql_type & (MYSQL_SP_ARG_TYPE_TINY | MYSQL_SP_ARG_TYPE_SHORT |
                        MYSQL_SP_ARG_TYPE_INT24 | MYSQL_SP_ARG_TYPE_LONG |
                        MYSQL_SP_ARG_TYPE_LONGLONG)) != 0;
  }

  /* Number of live routine handles; the component refuses UNINSTALL while
     it is non-zero, so no other component can be handed our handles. */
  static inline std::atomic<long> s_live_handles{0};

  static bool contains(const std::string &s, const char *what) {
    return s.find(what) != std::string::npos;
  }

  static void report(const char *what) {
    std::string msg = std::string(Traits::name) + "> " + what;
    my_error(ER_LANGUAGE_COMPONENT, MYF(0), msg.c_str());
  }

  static bool is_language(const char *str, size_t length, const char *name) {
    return length == strlen(name) && strncasecmp(str, name, length) == 0;
  }

  static bool is_our_language(const char *str, size_t length) {
    return is_language(str, length, Traits::name) ||
           is_language(str, length, "TWIN");
  }

 public:
  static long live_handles() { return s_live_handles.load(); }

  /* external_program_capability_query */
  static DEFINE_BOOL_METHOD(get, (const char *capability, char *property,
                                  void *value)) {
    if (strcmp(capability, "supports_language") != 0) {
      report("Unknown capability request.");
      return true;
    }
    *reinterpret_cast<bool *>(value) =
        is_our_language(property, strlen(property));
    return false;
  }

  /* external_program_execution */
  static DEFINE_BOOL_METHOD(init, (stored_program_handle sp,
                                   stored_program_statement_handle,
                                   external_program_handle *lang_sp)) {
    mysql_cstring_with_length language;
    if (mysql_service_mysql_stored_program_metadata_query->get(
            sp, "sp_language", &language) ||
        !is_our_language(language.str, language.length))
      return true;

    uint16_t type;
    if (mysql_service_mysql_stored_program_metadata_query->get(sp, "sp_type",
                                                               &type) ||
        (type != MYSQL_STORED_PROGRAM_DATA_QUERY_TYPE_FUNCTION &&
         type != MYSQL_STORED_PROGRAM_DATA_QUERY_TYPE_PROCEDURE))
      return true;

    auto *routine = new Routine();
    routine->type = type;
    mysql_cstring_with_length body;
    if (mysql_service_mysql_stored_program_metadata_query->get(sp, "sp_body",
                                                               &body) ||
        mysql_service_mysql_stored_program_metadata_query->get(
            sp, "argument_count", &routine->arg_count)) {
      delete routine;
      return true;
    }
    routine->body.assign(body.str, body.length);
    if (routine->arg_count > 0) {
      uint64_t sql_type = 0;
      bool is_out = false;
      if (mysql_service_mysql_stored_program_argument_metadata_query->get(
              sp, 0, "sql_type", &sql_type) ||
          mysql_service_mysql_stored_program_argument_metadata_query->get(
              sp, 0, "out_variable", &is_out)) {
        delete routine;
        return true;
      }
      routine->first_is_int = is_int_type(sql_type);
      routine->first_is_out = is_out;
    }
    ++s_live_handles;
    *lang_sp = reinterpret_cast<external_program_handle>(routine);
    return false;
  }

  static DEFINE_BOOL_METHOD(deinit, (MYSQL_THD, external_program_handle lang_sp,
                                     stored_program_handle)) {
    if (lang_sp == nullptr) return false;
    delete reinterpret_cast<Routine *>(lang_sp);
    --s_live_handles;
    return false;
  }

  static DEFINE_BOOL_METHOD(parse, (external_program_handle lang_sp,
                                    stored_program_statement_handle)) {
    auto *routine = reinterpret_cast<Routine *>(lang_sp);
    if (contains(routine->body, "SYNTAX_ERROR")) {
      report("Syntax error in routine body.");
      return true;
    }
    return false;
  }

  static DEFINE_BOOL_METHOD(execute, (external_program_handle lang_sp,
                                      stored_program_statement_handle)) {
    auto *routine = reinterpret_cast<Routine *>(lang_sp);
    if (contains(routine->body, "RUNTIME_ERROR")) {
      report("Runtime error requested by routine body.");
      return true;
    }
    if (routine->type == MYSQL_STORED_PROGRAM_DATA_QUERY_TYPE_PROCEDURE) {
      if (routine->arg_count == 0 || !routine->first_is_out) return false;
      std::string out = routine->body;
      Traits::transform(out);
      if (mysql_service_mysql_stored_program_runtime_argument_string->set(
              nullptr, 0, out.data(), out.size())) {
        report("First OUT parameter must be a string.");
        return true;
      }
      return false;
    }

    std::string result;
    if (routine->arg_count > 0 && routine->first_is_int) {
      int64_t value = 0;
      bool is_null = false;
      if (mysql_service_mysql_stored_program_runtime_argument_int->get(
              nullptr, 0, &value, &is_null)) {
        report("Cannot read integer argument.");
        return true;
      }
      if (is_null)
        return mysql_service_mysql_stored_program_return_value_null->set(
            nullptr);
      result = std::to_string(value);
    } else if (routine->arg_count > 0) {
      const char *value = nullptr;
      size_t length = 0;
      bool is_null = false;
      if (mysql_service_mysql_stored_program_runtime_argument_string->get(
              nullptr, 0, &value, &length, &is_null)) {
        report("First argument must be a string or an integer.");
        return true;
      }
      if (is_null)
        return mysql_service_mysql_stored_program_return_value_null->set(
            nullptr);
      result.assign(value, length);
    } else {
      result = routine->body;
    }
    Traits::transform(result);
    return mysql_service_mysql_stored_program_return_value_string->set(
        nullptr, result.data(), result.size());
  }

  /* external_library */
  static DEFINE_BOOL_METHOD(is_language_supported,
                            (mysql_cstring_with_length language,
                             bool *result)) {
    *result = is_our_language(language.str, language.length);
    return false;
  }

  static DEFINE_BOOL_METHOD(library_parse, (mysql_cstring_with_length,
                                            mysql_cstring_with_length language,
                                            mysql_cstring_with_length body)) {
    if (!is_our_language(language.str, language.length)) return true;
    if (contains(std::string(body.str, body.length), "SYNTAX_ERROR")) {
      report("Syntax error in library body.");
      return true;
    }
    return false;
  }

  /* external_library_ext */
  static DEFINE_BOOL_METHOD(library_parse_ext,
                            (mysql_cstring_with_length name,
                             mysql_cstring_with_length language,
                             mysql_cstring_with_length body, bool,
                             bool *result)) {
    // SQL layer contract: error => return true and *result == false,
    // success => return false and *result == true.
    *result = false;
    if (library_parse(name, language, body)) return true;
    *result = true;
    return false;
  }
};

#endif /* COMPONENTS_TEST_TEST_LANG_BACKEND_H */
