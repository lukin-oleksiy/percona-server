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
  @file components/lang_router/router.h

  Routing state of the language router component: which backend (language
  component) supports which language, and which backend owns each routine
  handle. See the design in 02-router.md.

  A backend is identified by its service implementation name, e.g. "js_lang"
  for "external_program_execution.js_lang".
*/

#ifndef COMPONENTS_LANG_ROUTER_ROUTER_H
#define COMPONENTS_LANG_ROUTER_ROUTER_H

#include <map>
#include <string>
#include <string_view>
#include <vector>

#include <mysql/components/services/language_service.h>

namespace lang_router {

/** Our own implementation name in the registry. */
constexpr const char kSelf[] = "lang_router";

constexpr const char kExecution[] = "external_program_execution";
constexpr const char kCapability[] = "external_program_capability_query";
constexpr const char kLibrary[] = "external_library";
constexpr const char kLibraryExt[] = "external_library_ext";

using Execution = SERVICE_TYPE_NO_CONST(external_program_execution);

/**
  Implementation names of a service, excluding the router itself.
  Safe to call without holding any lock (the registry iterator is released
  before returning).
*/
std::vector<std::string> implementations(const char *service);

/**
  The implementation that is currently the default for a service, or ""
  if none (or if it is the router itself).
*/
std::string current_default(const char *service);

/**
  Backend supporting a language (case-insensitive), or "" if no backend
  supports it. Positive results are cached.

  If several backends support the language, the language_map entry decides;
  without an entry the first one (in registry name order) wins and a warning
  goes to the error log. A language_map entry naming a backend that is not
  installed or does not support the language is ignored with a warning.
*/
std::string backend_for(std::string_view language);

/** Forget the cached backend of a language (e.g. backend uninstalled). */
void forget_language(std::string_view language);

/** Forget all cached languages. */
void clear_language_cache();

/*
  Routine handles created through the router are stored with the
  external_program_execution service of the backend that created them, so
  parse/execute/deinit call it directly (P2-2, 02-router.md 13.7, option B).
  No registry reference is held: the backend refuses UNINSTALL while it has
  live handles, which keeps both the handle and the service valid.
*/

/** Remember the backend service that created a routine handle. */
void remember_handle(external_program_handle handle, Execution *service);

/**
  The backend service that created a routine handle, or nullptr for a
  handle the router has never seen: it was created before the router was
  installed and belongs to the fallback backend (see fallback_for()).
*/
Execution *execution_of(external_program_handle handle);

/**
  Forget a routine handle and return its backend service (nullptr if
  unknown). Called before the backend's deinit, so a new handle that gets
  the same address after the backend freed this one is not forgotten.
*/
Execution *take_handle(external_program_handle handle);

/** Backend for a handle the router has never seen; counts the call. */
std::string fallback_for_unknown_handle();

/** Number of routine handles currently owned by backends via the router. */
size_t live_handles();

/** Backend that was the default before the router was installed, or "". */
void set_fallback_backend(const std::string &backend);
std::string fallback_backend();

/** How many calls were routed to the fallback backend. */
long fallback_routed();

/** Language (upper case) -> backend, from the sysvar language_map. */
using Language_map = std::map<std::string, std::string>;

/**
  Parse "LANG=impl[,LANG=impl...]" (names: letters, digits, '_'; spaces
  around names allowed; empty text = empty map). Language names are stored
  upper case, implementation names as given.
  @retval true  valid, *map is filled
  @retval false syntax error or duplicate language
*/
bool parse_language_map(std::string_view text, Language_map *map);

/** Replace the language map and clear the language cache. */
void set_language_map(Language_map map);

/** Number of language components found by the last discovery. */
long backends_found();

/** Discover the installed language components and count them. */
void count_backends();

/** Number of cached language -> backend entries. */
long cached_languages();

/** Error log messages (implemented in lang_router.cc). */
void log_info(const std::string &message);
void log_warning(const std::string &message);

}  // namespace lang_router

#endif /* COMPONENTS_LANG_ROUTER_ROUTER_H */
