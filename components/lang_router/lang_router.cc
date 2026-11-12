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
  @file components/lang_router/lang_router.cc

  Language router component.

  The SQL layer only ever calls the *default* implementation of the
  language services. The router becomes that default and forwards each call
  to the language component (backend) that supports the routine's LANGUAGE,
  so several language components can be installed at once (for example
  js_lang for LANGUAGE JS and another one for LANGUAGE JAVASCRIPT).

  CREATE LIBRARY is routed the same way, by the library's LANGUAGE. A
  backend without the library services (e.g. js_lang) keeps today's
  behaviour: the body is stored without validation, with a warning.

  If several backends support one language, the sysvar
  lang_router.language_map ("LANG=impl[,LANG=impl...]") chooses. Status
  variables Lang_router_* show the routing state; routing decisions,
  conflicts and the successor default go to the error log.

  Routine handles are passed through unchanged; the router remembers which
  backend created each handle. Backend services are acquired per call, so
  the router never prevents UNINSTALL of a backend.

  The router's services are registered manually in init() and unregistered
  in deinit(), not declared in PROVIDES: the dynamic loader registers
  PROVIDES services only after init(), too late to make them the default
  from init() (spike S-7). Design: 02-router.md.
*/

#define LOG_COMPONENT_TAG "lang_router"

#include <atomic>
#include <chrono>
#include <cstring>
#include <optional>
#include <string>
#include <thread>

#include <mysql/components/component_implementation.h>
#include <mysql/components/service_implementation.h>
#include <mysql/components/services/component_status_var_service.h>
#include <mysql/components/services/component_sys_var_service.h>
#include <mysql/components/services/language_service.h>
#include <mysql/components/services/log_builtins.h>
#include <mysql/components/services/mysql_current_thread_reader.h>
#include <mysql/components/services/mysql_runtime_error_service.h>
#include <mysql/components/services/mysql_runtime_warning.h>
#include <mysql/components/services/mysql_stored_program.h>
#include <mysql/components/services/registry.h>
#include <mysqld_error.h>

#include "router.h"

extern REQUIRES_SERVICE_PLACEHOLDER(registry);  // defined by REQUIRES below
REQUIRES_SERVICE_PLACEHOLDER(registry_query);
REQUIRES_SERVICE_PLACEHOLDER(registry_registration);
REQUIRES_SERVICE_PLACEHOLDER(mysql_stored_program_metadata_query);
REQUIRES_SERVICE_PLACEHOLDER(mysql_runtime_error);
REQUIRES_SERVICE_PLACEHOLDER(mysql_runtime_warning);
REQUIRES_SERVICE_PLACEHOLDER(mysql_current_thread_reader);
REQUIRES_SERVICE_PLACEHOLDER(component_sys_variable_register);
REQUIRES_SERVICE_PLACEHOLDER(component_sys_variable_unregister);
REQUIRES_SERVICE_PLACEHOLDER(status_variable_registration);
REQUIRES_SERVICE_PLACEHOLDER(log_builtins);
REQUIRES_SERVICE_PLACEHOLDER(log_builtins_string);

/* Used by the LogComponentErr macro. */
SERVICE_TYPE(log_builtins) *log_bi = nullptr;
SERVICE_TYPE(log_builtins_string) *log_bs = nullptr;

namespace lang_router {

void log_info(const std::string &message) {
  if (log_bi == nullptr) return;
  LogComponentErr(INFORMATION_LEVEL, ER_LANGUAGE_COMPONENT_INFO,
                  message.c_str());
}

void log_warning(const std::string &message) {
  if (log_bi == nullptr) return;
  LogComponentErr(WARNING_LEVEL, ER_LANGUAGE_COMPONENT_WARNING,
                  message.c_str());
}

}  // namespace lang_router

namespace lang_router {
namespace {

using Library = SERVICE_TYPE_NO_CONST(external_library);
using Library_ext = SERVICE_TYPE_NO_CONST(external_library_ext);

/** One service of a backend, acquired for one call. */
template <typename Service>
class Backend_service {
 public:
  Backend_service(const char *service, const std::string &backend) {
    if (backend.empty()) return;
    const std::string name = std::string(service) + "." + backend;
    if (mysql_service_registry->acquire(name.c_str(), &m_handle))
      m_handle = nullptr;
  }
  ~Backend_service() {
    if (m_handle != nullptr) mysql_service_registry->release(m_handle);
  }
  Backend_service(const Backend_service &) = delete;
  Backend_service &operator=(const Backend_service &) = delete;

  bool valid() const { return m_handle != nullptr; }
  Service *get() const { return reinterpret_cast<Service *>(m_handle); }
  Service *operator->() const { return get(); }

 private:
  my_h_service m_handle{nullptr};
};

class Backend_execution : public Backend_service<Execution> {
 public:
  explicit Backend_execution(const std::string &backend)
      : Backend_service(kExecution, backend) {}
};

/**
  Runs use(service) with the backend's execution service; nullopt if the
  backend is not there. Retried for a moment: while a backend's UNINSTALL is
  being refused, the loader has unregistered its services and registers them
  again when its deinit refuses (dynamic_loader.cc), and calls meanwhile must
  not fail or pick another backend for good.
*/
template <typename F>
std::optional<bool> with_backend(const std::string &backend, F &&use) {
  if (backend.empty()) return std::nullopt;
  for (int attempt = 0; attempt < 200; attempt++) {
    {
      Backend_execution exec(backend);
      if (exec.valid()) return use(exec.get());
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return std::nullopt;
}

/**
  Backend for the language of a library, or "". A cached backend that was
  uninstalled is forgotten and discovery runs once more.
*/
std::string library_backend(std::string_view language) {
  std::string backend = backend_for(language);
  if (!backend.empty() && !Backend_execution(backend).valid()) {
    forget_language(language);
    backend = backend_for(language);
  }
  return backend;
}

/**
  Whether a backend supports libraries. Like the SQL layer
  (Sql_cmd_create_library), this needs both library services.
*/
bool has_libraries(const std::string &backend) {
  return Backend_service<Library>(kLibrary, backend).valid() &&
         Backend_service<Library_ext>(kLibraryExt, backend).valid();
}

/**
  The warning the SQL layer gives when no language component supports
  libraries: the library body is stored without validation (P2-1).
*/
void warn_libraries_not_validated() {
  MYSQL_THD thd = nullptr;
  if (mysql_service_mysql_current_thread_reader->get(&thd) || thd == nullptr)
    return;
  mysql_service_mysql_runtime_warning->emit(
      thd, ER_LANGUAGE_COMPONENT_NOT_AVAILABLE);
}

void report_libraries_not_supported(mysql_cstring_with_length language) {
  const std::string lang(language.str, language.length);
  my_error(ER_LIBRARIES_NOT_SUPPORTED, MYF(0), lang.c_str());
}

/*
  Uninstall protocol. The SQL layer acquires the *default* implementation
  separately for init, execute and deinit of a routine. So once the default
  moves to the successor, a handle created through the router would be
  executed and freed by the successor directly, i.e. possibly by the wrong
  backend. Hence, before the default moves, no router handle may exist and
  none may be in the middle of being created: deinit() sets g_closing, which
  makes init() refuse new routines, and waits until the init() calls in
  progress (g_inits_running) have finished. Both are sequentially consistent
  atomics, so init() and deinit() always see each other.
*/
std::atomic<bool> g_closing{false};
std::atomic<int> g_inits_running{0};

/** Counts an init() call in progress. */
class Init_guard {
 public:
  Init_guard() { ++g_inits_running; }
  ~Init_guard() { --g_inits_running; }
  Init_guard(const Init_guard &) = delete;
  Init_guard &operator=(const Init_guard &) = delete;
};

/** Backend init; remembers the new handle with the backend's service. */
bool init_with(Execution *service, stored_program_handle sp,
               stored_program_statement_handle stmt,
               external_program_handle *lang_sp) {
  if (service->init(sp, stmt, lang_sp)) return true;
  if (*lang_sp != nullptr) remember_handle(*lang_sp, service);
  return false;
}

void report_no_backend() {
  my_error(ER_LANGUAGE_COMPONENT, MYF(0),
           "lang_router: the language component that owns this routine is "
           "not available.");
}

class Router_services {
 public:
  /* external_program_capability_query */
  static DEFINE_BOOL_METHOD(get, (const char *capability, char *property,
                                  void *value)) {
    if (strcmp(capability, "supports_language") != 0) {
      my_error(ER_LANGUAGE_COMPONENT, MYF(0),
               "Unknown capability request from lang_router component.");
      return true;
    }
    *reinterpret_cast<bool *>(value) = !backend_for(property).empty();
    return false;
  }

  /* external_program_execution */
  static DEFINE_BOOL_METHOD(init, (stored_program_handle sp,
                                   stored_program_statement_handle stmt,
                                   external_program_handle *lang_sp)) {
    const Init_guard guard;
    if (g_closing) {
      my_error(ER_LANGUAGE_COMPONENT, MYF(0),
               "lang_router: the router is being uninstalled. Try again.");
      return true;
    }
    mysql_cstring_with_length language;
    if (mysql_service_mysql_stored_program_metadata_query->get(
            sp, "sp_language", &language))
      return true;
    const std::string_view lang(language.str, language.length);

    std::string backend = backend_for(lang);
    if (backend.empty()) return true;  // SQL layer reports the language
    auto use = [&](Execution *service) {
      return init_with(service, sp, stmt, lang_sp);
    };
    if (auto r = with_backend(backend, use)) return *r;
    /* Cached backend was uninstalled: discover again, once. */
    forget_language(lang);
    if (auto r = with_backend(backend_for(lang), use)) return *r;
    return true;
  }

  static DEFINE_BOOL_METHOD(deinit,
                            (MYSQL_THD thd, external_program_handle lang_sp,
                             stored_program_handle sp)) {
    /* Fast path: called for every SQL routine too, with a null handle. */
    if (lang_sp == nullptr) return false;
    if (Execution *service = take_handle(lang_sp))
      return service->deinit(thd, lang_sp, sp);
    return with_backend(fallback_for_unknown_handle(), [&](Execution *service) {
             return service->deinit(thd, lang_sp, sp);
           }).value_or(false);
  }

  static DEFINE_BOOL_METHOD(parse, (external_program_handle lang_sp,
                                    stored_program_statement_handle stmt)) {
    if (Execution *service = execution_of(lang_sp))
      return service->parse(lang_sp, stmt);
    if (auto r = with_backend(fallback_for_unknown_handle(),
                              [&](Execution *service) {
                                return service->parse(lang_sp, stmt);
                              }))
      return *r;
    report_no_backend();
    return true;
  }

  static DEFINE_BOOL_METHOD(execute, (external_program_handle lang_sp,
                                      stored_program_statement_handle stmt)) {
    if (Execution *service = execution_of(lang_sp))
      return service->execute(lang_sp, stmt);
    if (auto r = with_backend(fallback_for_unknown_handle(),
                              [&](Execution *service) {
                                return service->execute(lang_sp, stmt);
                              }))
      return *r;
    report_no_backend();
    return true;
  }

  /* external_library */
  static DEFINE_BOOL_METHOD(is_language_supported,
                            (mysql_cstring_with_length language,
                             bool *result)) {
    const std::string backend =
        library_backend({language.str, language.length});
    if (backend.empty()) {
      *result = false;  // SQL layer reports ER_LIBRARIES_NOT_SUPPORTED
      return false;
    }
    if (!has_libraries(backend)) {
      *result = true;  // P2-1: parse stores the body with a warning
      return false;
    }
    return Backend_service<Library>(kLibrary, backend)
        ->is_language_supported(language, result);
  }

  static DEFINE_BOOL_METHOD(library_parse, (mysql_cstring_with_length name,
                                            mysql_cstring_with_length language,
                                            mysql_cstring_with_length body)) {
    const std::string backend =
        library_backend({language.str, language.length});
    if (backend.empty()) {
      report_libraries_not_supported(language);
      return true;
    }
    if (!has_libraries(backend)) {
      warn_libraries_not_validated();
      return false;
    }
    return Backend_service<Library>(kLibrary, backend)
        ->parse(name, language, body);
  }

  /* external_library_ext */
  static DEFINE_BOOL_METHOD(library_parse_ext,
                            (mysql_cstring_with_length name,
                             mysql_cstring_with_length language,
                             mysql_cstring_with_length body, bool is_binary,
                             bool *result)) {
    *result = false;
    const std::string backend =
        library_backend({language.str, language.length});
    if (backend.empty()) {
      report_libraries_not_supported(language);
      return true;
    }
    if (!has_libraries(backend)) {
      warn_libraries_not_validated();
      *result = true;
      return false;
    }
    return Backend_service<Library_ext>(kLibraryExt, backend)
        ->parse(name, language, body, is_binary, result);
  }
};

}  // namespace
}  // namespace lang_router

// clang-format off
BEGIN_SERVICE_IMPLEMENTATION(lang_router, external_program_capability_query)
  lang_router::Router_services::get,
END_SERVICE_IMPLEMENTATION();

BEGIN_SERVICE_IMPLEMENTATION(lang_router, external_program_execution)
  lang_router::Router_services::init,
  lang_router::Router_services::deinit,
  lang_router::Router_services::parse,
  lang_router::Router_services::execute,
END_SERVICE_IMPLEMENTATION();

BEGIN_SERVICE_IMPLEMENTATION(lang_router, external_library)
  lang_router::Router_services::is_language_supported,
  lang_router::Router_services::library_parse,
END_SERVICE_IMPLEMENTATION();

BEGIN_SERVICE_IMPLEMENTATION(lang_router, external_library_ext)
  lang_router::Router_services::library_parse_ext,
END_SERVICE_IMPLEMENTATION();
// clang-format on

namespace lang_router {
namespace {

/** A service the router registers manually and makes the default. */
struct Routed_service {
  const char *service;
  const void *implementation;
  bool registered;
};

Routed_service g_services[] = {
    {kExecution,
     &SERVICE_IMPLEMENTATION(lang_router, external_program_execution), false},
    {kCapability,
     &SERVICE_IMPLEMENTATION(lang_router, external_program_capability_query),
     false},
    {kLibrary, &SERVICE_IMPLEMENTATION(lang_router, external_library), false},
    {kLibraryExt, &SERVICE_IMPLEMENTATION(lang_router, external_library_ext),
     false},
};

/* Sysvar lang_router.language_map. The value is owned by the server
   (PLUGIN_VAR_MEMALLOC); the router works on its parsed copy. */
constexpr const char kMapVar[] = "language_map";
char *g_language_map_value = nullptr;
bool g_map_var_registered = false;

int check_language_map(MYSQL_THD, SYS_VAR *, void *save,
                       struct st_mysql_value *value) {
  if (value == nullptr) return 1;
  int length = 0;
  /* With no buffer, the value is copied to the THD's memory and lives until
     the end of the statement (as in log_filter_dragnet). */
  const char *text = value->val_str(value, nullptr, &length);
  Language_map map;
  if (text != nullptr &&
      !parse_language_map({text, static_cast<size_t>(length)}, &map))
    return 1;
  *static_cast<const char **>(save) = text;
  return 0;
}

void apply_language_map(const char *text) {
  Language_map map;
  if (!parse_language_map(text == nullptr ? "" : text, &map))
    log_warning(std::string("lang_router.language_map value '") + text +
                "' is invalid and is ignored.");
  set_language_map(std::move(map));
}

void update_language_map(MYSQL_THD, SYS_VAR *, void *val_ptr,
                         const void *save) {
  char *text = *static_cast<char *const *>(save);
  *static_cast<char **>(val_ptr) = text;
  apply_language_map(text);
}

bool register_language_map() {
  STR_CHECK_ARG(str) arg;
  arg.def_val = nullptr;
  if (mysql_service_component_sys_variable_register->register_variable(
          kSelf, kMapVar, PLUGIN_VAR_STR | PLUGIN_VAR_MEMALLOC,
          "Which language component runs a LANGUAGE when several support it: "
          "LANG=component[,LANG=component...], e.g. "
          "JS=js_lang,JAVASCRIPT=mle.",
          check_language_map, update_language_map, &arg, &g_language_map_value))
    return true;
  g_map_var_registered = true;
  /* A value from the command line or SET PERSIST is not checked by the
     server: an invalid one is ignored with a warning. */
  apply_language_map(g_language_map_value);
  return false;
}

void unregister_language_map() {
  if (!g_map_var_registered) return;
  if (!mysql_service_component_sys_variable_unregister->unregister_variable(
          kSelf, kMapVar)) {
    g_map_var_registered = false;
    g_language_map_value = nullptr;  // freed by the server
  }
  set_language_map({});
}

/* Status variables. */
template <long (*getter)()>
int show_long(MYSQL_THD, SHOW_VAR *var, char *buff) {
  var->type = SHOW_LONG;
  var->value = buff;
  *reinterpret_cast<long *>(buff) = getter();
  return 0;
}

long live_handles_long() { return static_cast<long>(live_handles()); }

SHOW_VAR g_status_vars[] = {
    {"Lang_router_backends",
     reinterpret_cast<char *>(&show_long<backends_found>), SHOW_FUNC,
     SHOW_SCOPE_GLOBAL},
    {"Lang_router_languages",
     reinterpret_cast<char *>(&show_long<cached_languages>), SHOW_FUNC,
     SHOW_SCOPE_GLOBAL},
    {"Lang_router_live_handles",
     reinterpret_cast<char *>(&show_long<live_handles_long>), SHOW_FUNC,
     SHOW_SCOPE_GLOBAL},
    {"Lang_router_fallback_routed",
     reinterpret_cast<char *>(&show_long<fallback_routed>), SHOW_FUNC,
     SHOW_SCOPE_GLOBAL},
    {nullptr, nullptr, SHOW_UNDEF, SHOW_SCOPE_UNDEF}};
bool g_status_registered = false;

void unregister_variables() {
  if (g_status_registered &&
      !mysql_service_status_variable_registration->unregister_variable(
          g_status_vars))
    g_status_registered = false;
  unregister_language_map();
}

std::string own_name(const char *service) {
  return std::string(service) + "." + kSelf;
}

void unregister_all() {
  for (auto &s : g_services) {
    if (!s.registered) continue;
    if (!mysql_service_registry_registration->unregister(
            own_name(s.service).c_str()))
      s.registered = false;
  }
}

/**
  Backend to become the default when the router goes away: the backend that
  was the default before the router, else the one supporting JS (keeps
  LANGUAGE JS working as without the router), else none (the registry then
  picks one itself).
*/
std::string successor_backend() {
  const std::string fallback = fallback_backend();
  if (!fallback.empty() && Backend_execution(fallback).valid()) return fallback;
  return backend_for("JS");
}

/**
  Register the services that are not registered yet and make all of them
  the default.
*/
bool register_and_make_default() {
  for (auto &s : g_services) {
    if (s.registered) continue;
    if (mysql_service_registry_registration->register_service(
            own_name(s.service).c_str(),
            reinterpret_cast<my_h_service>(
                const_cast<void *>(s.implementation))))
      return true;
    s.registered = true;
  }
  for (const auto &s : g_services)
    if (mysql_service_registry_registration->set_default(
            own_name(s.service).c_str()))
      return true;
  return false;
}

mysql_service_status_t init() {
  log_bi = mysql_service_log_builtins;
  log_bs = mysql_service_log_builtins_string;
  /* The library may stay loaded between UNINSTALL and INSTALL. */
  g_closing = false;

  /* Before we become the default: handles that exist now were created by
     the current default backend. */
  set_fallback_backend(current_default(kExecution));
  clear_language_cache();
  count_backends();

  if (register_language_map()) return 1;
  if (mysql_service_status_variable_registration->register_variable(
          g_status_vars)) {
    unregister_variables();
    return 1;
  }
  g_status_registered = true;

  if (register_and_make_default()) {
    unregister_all();
    unregister_variables();
    return 1;
  }
  const std::string fallback = fallback_backend();
  log_info("Installed; it routes stored programs by LANGUAGE." +
           (fallback.empty() ? std::string()
                             : " Routines cached before the install run in " +
                                   fallback + "."));
  return 0;
}

/**
  Stop new routines from getting router handles and wait for the init()
  calls in progress. Returns false if they don't finish in time.
*/
bool close_and_drain() {
  g_closing = true;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(1);
  while (g_inits_running > 0) {
    if (std::chrono::steady_clock::now() > deadline) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return true;
}

mysql_service_status_t deinit() {
  /* Refuse while backends still own handles routed by us: after we are
     gone, their execute and deinit would reach the successor directly. No
     new router handle can appear after close_and_drain(), so the check is
     final. Nothing has been unregistered yet, so the router stays the
     default and keeps working. */
  const bool drained = close_and_drain();
  const long live = drained ? live_handles() : 0;
  if (!drained || live > 0) {
    g_closing = false;
    log_info(!drained
                 ? "Uninstall refused: a stored program was still being set up "
                   "after 1 second. Try again."
                 : "Uninstall refused: " + std::to_string(live) +
                       " stored programs routed by the router are cached in "
                       "sessions (they go when the sessions end, or after DROP "
                       "or ALTER of the programs).");
    return 1;
  }

  const std::string successor = successor_backend();
  /* From the switch below until we are unregistered, the successor may get
     calls directly and create handles we don't know. If we have to undo the
     uninstall, those handles must be routed to it. Normally the successor
     is the fallback backend already. */
  set_fallback_backend(successor);
  if (!successor.empty()) {
    for (const auto &s : g_services) {
      const std::string name = std::string(s.service) + "." + successor;
      /* Ignore failure: the successor may not provide every service
         (js_lang has no library services). The registry then picks a
         default among the remaining implementations itself. */
      (void)mysql_service_registry_registration->set_default(name.c_str());
    }
  }
  unregister_all();

  /* A call that acquired our services before the switch may still run
     (on a fallback handle, or a capability or library call): the registry
     then refuses to unregister them (references held). Undo: the router
     becomes the default again and stays. */
  bool in_use = false;
  for (const auto &s : g_services) in_use = in_use || s.registered;
  if (in_use) {
    g_closing = false;
    log_info(
        "Uninstall refused: stored programs used the router while it was "
        "being uninstalled. It stays the default language component.");
    if (register_and_make_default())
      log_warning(
          "Uninstall was refused, but the router could not become the "
          "default language component again.");
    return 1;
  }

  unregister_variables();
  clear_language_cache();
  if (successor.empty())
    log_info("Uninstalled; the server chooses the default language component.");
  else
    log_info("Uninstalled; " + successor +
             " is the default language component again.");
  log_bi = nullptr;
  log_bs = nullptr;
  return 0;
}

}  // namespace
}  // namespace lang_router

// clang-format off
BEGIN_COMPONENT_PROVIDES(lang_router)
END_COMPONENT_PROVIDES();

BEGIN_COMPONENT_REQUIRES(lang_router)
  REQUIRES_SERVICE(registry_query),
  REQUIRES_SERVICE(registry_registration),
  REQUIRES_SERVICE(mysql_stored_program_metadata_query),
  REQUIRES_SERVICE(mysql_runtime_error),
  REQUIRES_SERVICE(mysql_runtime_warning),
  REQUIRES_SERVICE(mysql_current_thread_reader),
  REQUIRES_SERVICE(component_sys_variable_register),
  REQUIRES_SERVICE(component_sys_variable_unregister),
  REQUIRES_SERVICE(status_variable_registration),
  REQUIRES_SERVICE(log_builtins),
  REQUIRES_SERVICE(log_builtins_string),
END_COMPONENT_REQUIRES();

BEGIN_COMPONENT_METADATA(lang_router)
  METADATA("mysql.author", "Percona Corporation"),
  METADATA("mysql.license", "GPL"),
END_COMPONENT_METADATA();

DECLARE_COMPONENT(lang_router, "lang_router")
  lang_router::init,
  lang_router::deinit,
END_DECLARE_COMPONENT();

DECLARE_LIBRARY_COMPONENTS
  &COMPONENT_REF(lang_router)
END_DECLARE_LIBRARY_COMPONENTS
    // clang-format on
