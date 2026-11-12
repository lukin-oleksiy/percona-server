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
  @file components/test/test_lang_echo.cc

  Test language "ECHO": returns its argument (or body) unchanged.
  Supports libraries. See test_lang_backend.h.
*/

#include "test_lang_backend.h"

namespace {
struct Echo_traits {
  static constexpr const char *name = "ECHO";
  static void transform(std::string &) {}
};
using Imp = Test_lang_backend<Echo_traits>;

mysql_service_status_t init() { return 0; }
// Refuse UNINSTALL while stored programs still hold our handles.
mysql_service_status_t deinit() { return Imp::live_handles() > 0 ? 1 : 0; }
}  // namespace

// clang-format off
BEGIN_SERVICE_IMPLEMENTATION(test_lang_echo, external_program_capability_query)
  Imp::get,
END_SERVICE_IMPLEMENTATION();

BEGIN_SERVICE_IMPLEMENTATION(test_lang_echo, external_program_execution)
  Imp::init,
  Imp::deinit,
  Imp::parse,
  Imp::execute,
END_SERVICE_IMPLEMENTATION();

BEGIN_SERVICE_IMPLEMENTATION(test_lang_echo, external_library)
  Imp::is_language_supported,
  Imp::library_parse,
END_SERVICE_IMPLEMENTATION();

BEGIN_SERVICE_IMPLEMENTATION(test_lang_echo, external_library_ext)
  Imp::library_parse_ext,
END_SERVICE_IMPLEMENTATION();

BEGIN_COMPONENT_PROVIDES(test_lang_echo)
  PROVIDES_SERVICE(test_lang_echo, external_program_capability_query),
  PROVIDES_SERVICE(test_lang_echo, external_program_execution),
  PROVIDES_SERVICE(test_lang_echo, external_library),
  PROVIDES_SERVICE(test_lang_echo, external_library_ext),
END_COMPONENT_PROVIDES();

BEGIN_COMPONENT_REQUIRES(test_lang_echo)
  TEST_LANG_BACKEND_REQUIRES,
END_COMPONENT_REQUIRES();

BEGIN_COMPONENT_METADATA(test_lang_echo)
  METADATA("mysql.author", "Percona Corporation"),
  METADATA("mysql.license", "GPL"),
END_COMPONENT_METADATA();

DECLARE_COMPONENT(test_lang_echo, "test_lang_echo")
  init,
  deinit,
END_DECLARE_COMPONENT();

DECLARE_LIBRARY_COMPONENTS
  &COMPONENT_REF(test_lang_echo)
END_DECLARE_LIBRARY_COMPONENTS
// clang-format on
