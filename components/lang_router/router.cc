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

#include "router.h"

#include <algorithm>
#include <atomic>
#include <set>
#include <cctype>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>

#include <mysql/components/component_implementation.h>
#include <mysql/components/services/registry.h>

extern REQUIRES_SERVICE_PLACEHOLDER(registry);
extern REQUIRES_SERVICE_PLACEHOLDER(registry_query);

namespace lang_router {

namespace {

/* Language (upper case) -> backend. Only positive results are cached, so a
   backend installed later is found for languages nobody supported before.
   The same lock protects the language map; a new map clears the cache and
   bumps the version, so a discovery that started with the old map does not
   cache its result. */
std::mutex g_language_lock;
std::unordered_map<std::string, std::string> g_languages;
Language_map g_language_map;
unsigned long g_map_version{0};
std::atomic<long> g_cached_languages{0};
std::atomic<long> g_backends_found{0};

/* Routine handle -> service of the backend that created it. */
std::shared_mutex g_handle_lock;
std::unordered_map<external_program_handle, Execution *> g_handles;

/* Protected by g_handle_lock: set in init() and changed in deinit(). */
std::string g_fallback;
std::atomic<long> g_fallback_routed{0};

bool is_name_char(char c) {
  return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

std::string_view trim(std::string_view s) {
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
    s.remove_prefix(1);
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
    s.remove_suffix(1);
  return s;
}

bool is_name(std::string_view s) {
  return !s.empty() && std::all_of(s.begin(), s.end(), is_name_char);
}

std::string join(const std::vector<std::string> &names) {
  std::string r;
  for (const auto &n : names) r += (r.empty() ? "" : ", ") + n;
  return r;
}

std::string upper(std::string_view s) {
  std::string r(s);
  std::transform(r.begin(), r.end(), r.begin(),
                 [](unsigned char c) { return std::toupper(c); });
  return r;
}

bool backend_supports(const std::string &backend, const std::string &language) {
  const std::string name = std::string(kCapability) + "." + backend;
  my_h_service h = nullptr;
  if (mysql_service_registry->acquire(name.c_str(), &h)) return false;
  auto *cap = reinterpret_cast<SERVICE_TYPE_NO_CONST(
      external_program_capability_query) *>(h);
  bool supported = false;
  const bool failed = cap->get(
      "supports_language", const_cast<char *>(language.c_str()), &supported);
  mysql_service_registry->release(h);
  return !failed && supported;
}

}  // namespace

std::vector<std::string> implementations(const char *service) {
  std::vector<std::string> result;
  const std::string prefix = std::string(service) + ".";
  my_h_service_iterator it = nullptr;
  /* The iterator holds the registry read lock until released: only collect
     names here, acquire services after release (S-7). */
  if (mysql_service_registry_query->create(service, &it)) return result;
  for (; !mysql_service_registry_query->is_valid(it);
       mysql_service_registry_query->next(it)) {
    const char *name = nullptr;
    if (mysql_service_registry_query->get(it, &name)) break;
    const std::string_view n(name);
    if (n.substr(0, prefix.size()) != prefix) break;  // past this service
    std::string impl(n.substr(prefix.size()));
    if (impl != kSelf &&
        std::find(result.begin(), result.end(), impl) == result.end())
      result.push_back(std::move(impl));
  }
  mysql_service_registry_query->release(it);
  return result;
}

std::string current_default(const char *service) {
  my_h_service def = nullptr;
  if (mysql_service_registry->acquire(service, &def)) return "";
  std::string found;
  for (const auto &impl : implementations(service)) {
    const std::string name = std::string(service) + "." + impl;
    my_h_service h = nullptr;
    if (mysql_service_registry->acquire(name.c_str(), &h)) continue;
    if (h == def) found = impl;
    mysql_service_registry->release(h);
    if (!found.empty()) break;
  }
  mysql_service_registry->release(def);
  return found;
}

namespace {
/** Languages warned about for the current map version (an unusable map entry
    is looked up on every CREATE or call: one warning, not one each time). */
std::set<std::string> g_warned;
unsigned long g_warned_version = 0;

bool warn_once(const std::string &key, unsigned long version) {
  std::lock_guard<std::mutex> lock(g_language_lock);
  if (g_warned_version != version) {
    g_warned.clear();
    g_warned_version = version;
  }
  return g_warned.insert(key).second;
}
}  // namespace

std::string backend_for(std::string_view language) {
  const std::string key = upper(language);
  std::string mapped;
  unsigned long version;
  {
    std::lock_guard<std::mutex> lock(g_language_lock);
    auto it = g_languages.find(key);
    if (it != g_languages.end()) return it->second;
    auto m = g_language_map.find(key);
    if (m != g_language_map.end()) mapped = m->second;
    version = g_map_version;
  }
  /* Discovery runs without our lock: it calls into the registry and into
     backends. Two threads may discover the same language at once; the first
     one to store the result wins and both return it. */
  const std::vector<std::string> all = implementations(kCapability);
  g_backends_found = static_cast<long>(all.size());
  std::vector<std::string> supporters;
  for (const auto &backend : all)
    if (backend_supports(backend, key)) supporters.push_back(backend);

  std::string chosen;
  if (!mapped.empty()) {
    if (std::find(supporters.begin(), supporters.end(), mapped) !=
        supporters.end())
      chosen = mapped;
    else if (warn_once(key, version))
      log_warning("lang_router.language_map entry " + key + "=" + mapped +
                  " is ignored: no installed language component " + mapped +
                  " supports LANGUAGE " + key + ".");
  }
  if (chosen.empty()) {
    if (supporters.empty()) return "";
    chosen = supporters.front();
    if (supporters.size() > 1)
      log_warning("LANGUAGE " + key + " is supported by " + join(supporters) +
                  "; using " + chosen +
                  ". Set lang_router.language_map to choose.");
  }

  // A map entry that could not be used (its component is not installed yet):
  // not cached, so that the component is used once it is installed.
  if (!mapped.empty() && chosen != mapped) return chosen;

  bool inserted = false;
  {
    std::lock_guard<std::mutex> lock(g_language_lock);
    if (version != g_map_version) return chosen;  // map changed: don't cache
    auto [it, ins] = g_languages.emplace(key, chosen);
    chosen = it->second;
    inserted = ins;
    g_cached_languages = static_cast<long>(g_languages.size());
  }
  if (inserted) log_info("LANGUAGE " + key + " is routed to " + chosen + ".");
  return chosen;
}

void forget_language(std::string_view language) {
  std::lock_guard<std::mutex> lock(g_language_lock);
  g_languages.erase(upper(language));
  g_cached_languages = static_cast<long>(g_languages.size());
}

void clear_language_cache() {
  std::lock_guard<std::mutex> lock(g_language_lock);
  g_languages.clear();
  g_cached_languages = 0;
}

bool parse_language_map(std::string_view text, Language_map *map) {
  map->clear();
  if (trim(text).empty()) return true;
  while (true) {
    const size_t comma = text.find(',');
    const std::string_view entry = text.substr(0, comma);
    const size_t eq = entry.find('=');
    if (eq == std::string_view::npos) return false;
    const std::string_view lang = trim(entry.substr(0, eq));
    const std::string_view impl = trim(entry.substr(eq + 1));
    if (!is_name(lang) || !is_name(impl)) return false;
    if (!map->emplace(upper(lang), std::string(impl)).second)
      return false;  // duplicate language
    if (comma == std::string_view::npos) return true;
    text.remove_prefix(comma + 1);
  }
}

void set_language_map(Language_map map) {
  std::lock_guard<std::mutex> lock(g_language_lock);
  g_language_map = std::move(map);
  ++g_map_version;
  g_languages.clear();
  g_cached_languages = 0;
}

long backends_found() { return g_backends_found.load(); }

void count_backends() {
  g_backends_found = static_cast<long>(implementations(kCapability).size());
}

long cached_languages() { return g_cached_languages.load(); }

void remember_handle(external_program_handle handle, Execution *service) {
  std::unique_lock<std::shared_mutex> lock(g_handle_lock);
  g_handles[handle] = service;
}

Execution *execution_of(external_program_handle handle) {
  std::shared_lock<std::shared_mutex> lock(g_handle_lock);
  auto it = g_handles.find(handle);
  return it == g_handles.end() ? nullptr : it->second;
}

Execution *take_handle(external_program_handle handle) {
  std::unique_lock<std::shared_mutex> lock(g_handle_lock);
  auto it = g_handles.find(handle);
  if (it == g_handles.end()) return nullptr;
  Execution *service = it->second;
  g_handles.erase(it);
  return service;
}

std::string fallback_for_unknown_handle() {
  ++g_fallback_routed;
  return fallback_backend();
}

size_t live_handles() {
  std::shared_lock<std::shared_mutex> lock(g_handle_lock);
  return g_handles.size();
}

void set_fallback_backend(const std::string &backend) {
  std::unique_lock<std::shared_mutex> lock(g_handle_lock);
  g_fallback = backend;
}

std::string fallback_backend() {
  std::shared_lock<std::shared_mutex> lock(g_handle_lock);
  return g_fallback;
}

long fallback_routed() { return g_fallback_routed.load(); }

}  // namespace lang_router
