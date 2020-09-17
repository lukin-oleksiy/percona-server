/* Copyright (c) 2023 Percona LLC and/or its affiliates. All rights reserved.

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

#include <array>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>

#include <boost/preprocessor/stringize.hpp>

#include <mysql/components/services/component_sys_var_service.h>
#include <mysql/components/services/mysql_runtime_error_service.h>

#include <mysqlpp/udf_registration.hpp>
#include <mysqlpp/udf_wrappers.hpp>

#define ALLOW_COMPONENT_INCLUDE  // for my_io.h used by binlog includes
#include <sql/binlog.h>
#include <sql/binlog/decompressing_event_object_istream.h>
#include <sql/binlog_reader.h>

// defined as a macro because needed both raw and stringized
#define CURRENT_COMPONENT_NAME binlog_utils_udf
#define CURRENT_COMPONENT_NAME_STR BOOST_PP_STRINGIZE(CURRENT_COMPONENT_NAME)

REQUIRES_SERVICE_PLACEHOLDER(udf_registration);
REQUIRES_SERVICE_PLACEHOLDER(component_sys_variable_register);

class get_binlog_by_gtid_capsule {
 public:
  get_binlog_by_gtid_capsule(mysqlpp::udf_context &ctx) {
    DBUG_TRACE;

    if (ctx.get_number_of_args() != 1)
      throw std::invalid_argument(
          "GET_BINLOG_BY_GTID() requires exactly one argument");
    ctx.mark_result_const(false);
    ctx.mark_result_nullable(true);
    ctx.mark_arg_nullable(0, false);
    ctx.set_arg_type(0, STRING_RESULT);
  }
  ~get_binlog_by_gtid_capsule() { DBUG_TRACE; }

  mysqlpp::udf_result_t<STRING_RESULT> calculate(
      const mysqlpp::udf_context &args);

 private:
  using log_event_ptr = std::shared_ptr<Log_event>;
  log_event_ptr find_previous_gtids_event(std::string_view binlog_name);
};

mysqlpp::udf_result_t<STRING_RESULT> get_binlog_by_gtid_capsule::calculate(
    const mysqlpp::udf_context &ctx) {
  DBUG_TRACE;

  auto gtid_text = static_cast<std::string>(ctx.get_arg<STRING_RESULT>(0));
  Tsid_map tsid_map{nullptr};
  Gtid gtid;
  if (gtid.parse(&tsid_map, gtid_text.c_str()) !=
      mysql::utils::Return_status::ok)
    throw std::invalid_argument("Invalid GTID specified");

  Gtid_set covering_gtids{&tsid_map};

  {
    constexpr std::size_t initial_buffer_size{1024};
    using static_buffer_t = std::array<char, initial_buffer_size + 1>;
    static_buffer_t static_buffer{};

    using dynamic_buffer_t = std::vector<char>;
    dynamic_buffer_t dynamic_buffer{};
    void *ptr = static_buffer.data();
    std::size_t length = initial_buffer_size;

    if (mysql_service_component_sys_variable_register->get_variable(
            "mysql_server", "gtid_executed", &ptr, &length)) {
      dynamic_buffer.resize(length + 1);
      ptr = dynamic_buffer.data();
      if (mysql_service_component_sys_variable_register->get_variable(
              "mysql_server", "gtid_executed", &ptr, &length))
        throw std::runtime_error("Cannot get 'gtid_executed'");
    }

    auto gtid_set_parse_result =
        covering_gtids.add_gtid_text(static_cast<char *>(ptr));
    if (gtid_set_parse_result != RETURN_STATUS_OK)
      throw std::runtime_error("Cannot parse 'gtid_executed'");
  }

  auto log_index = mysql_bin_log.get_log_index(true /* need_lock_index */);
  if (log_index.first != LOG_INFO_EOF)
    throw std::runtime_error("Cannot read binary log index'");
  if (log_index.second.empty())
    throw std::runtime_error("Binary log index is empty'");
  auto it = log_index.second.crbegin();
  auto en = log_index.second.crend();
  bool found{false};
  do {
    auto ev = find_previous_gtids_event(*it);
    Gtid_set extracted_gtids{&tsid_map};
    if (!ev) {
      if (*it != log_index.second.front())
        throw std::runtime_error(
            "Encountered binary log without PREVIOUS_GTIDS_LOG_EVENT in the "
            "middle of log index'");
    } else {
      assert(ev->get_type_code() == binary_log::PREVIOUS_GTIDS_LOG_EVENT);
      auto *casted_ev = static_cast<Previous_gtids_log_event *>(ev.get());
      casted_ev->add_to_set(&extracted_gtids);
    }
    found = covering_gtids.contains_gtid(gtid) &&
            !extracted_gtids.contains_gtid(gtid);
    if (!found) {
      covering_gtids.clear();
      covering_gtids.add_gtid_set(&extracted_gtids);
      ++it;
    }
  } while (!found && it != en);
  if (!found) return {};

  return {*it};
}

get_binlog_by_gtid_capsule::log_event_ptr
get_binlog_by_gtid_capsule::find_previous_gtids_event(
    std::string_view binlog_name) {
  DBUG_TRACE;

  std::string casted_binlog_name = static_cast<std::string>(binlog_name);

  char search_file_name[FN_REFLEN + 1];
  mysql_bin_log.make_log_name(search_file_name, casted_binlog_name.c_str());

  Binlog_file_reader reader(false /* do not verify checksum */);
  if (reader.open(search_file_name, 0))
    throw std::runtime_error(reader.get_error_str());

  // Here 'is_active()' is called after 'get_binlog_end_pos()' deliberately
  // to properly handle the situation when rotation happens between these
  // two calls
  my_off_t end_pos = mysql_bin_log.get_binlog_end_pos();
  if (!mysql_bin_log.is_active(search_file_name))
    end_pos = std::numeric_limits<my_off_t>::max();

  log_event_ptr ev;
  binlog::Decompressing_event_object_istream istream{reader};

  while (istream >> ev) {
    if (reader.has_fatal_error())
      throw std::runtime_error(reader.get_error_str());
    if (ev->get_type_code() == binary_log::PREVIOUS_GTIDS_LOG_EVENT) return ev;
    if (ev->common_header->log_pos >= end_pos) break;
  }
  if (istream.has_error()) throw std::runtime_error(istream.get_error_str());
  return {};
}

DECLARE_STRING_UDF(get_binlog_by_gtid_capsule, get_binlog_by_gtid)

class get_last_gtid_from_binlog_capsule {
 public:
  get_last_gtid_from_binlog_capsule(mysqlpp::udf_context &ctx) {
    DBUG_TRACE;

    if (ctx.get_number_of_args() != 1)
      throw std::invalid_argument(
          "GET_LAST_GTID_FROM_BINLOG() requires exactly one argument");
    ctx.mark_result_const(false);
    ctx.mark_result_nullable(true);
    ctx.mark_arg_nullable(0, false);
    ctx.set_arg_type(0, STRING_RESULT);
  }
  ~get_last_gtid_from_binlog_capsule() { DBUG_TRACE; }

  mysqlpp::udf_result_t<STRING_RESULT> calculate(
      const mysqlpp::udf_context &args);

 private:
  using log_event_ptr = std::shared_ptr<Log_event>;
  log_event_ptr find_last_gtid_event(std::string_view binlog_name);
};

mysqlpp::udf_result_t<STRING_RESULT>
get_last_gtid_from_binlog_capsule::calculate(const mysqlpp::udf_context &ctx) {
  DBUG_TRACE;

  Tsid_map tsid_map{nullptr};

  auto ev = find_last_gtid_event(ctx.get_arg<STRING_RESULT>(0));
  if (!ev) return {};

  assert(ev->get_type_code() == binary_log::GTID_LOG_EVENT);
  auto *casted_ev = static_cast<Gtid_log_event *>(ev.get());
  rpl_sidno sidno = casted_ev->get_sidno(&tsid_map);
  if (sidno < 0) throw std::runtime_error("Invalid GTID event encountered");
  Gtid gtid;
  gtid.set(sidno, casted_ev->get_gno());

  char buf[Gtid::MAX_TEXT_LENGTH + 1];
  auto length = static_cast<std::size_t>(gtid.to_string(&tsid_map, buf));

  return mysqlpp::udf_result_t<STRING_RESULT>{std::in_place, buf, length};
}

get_last_gtid_from_binlog_capsule::log_event_ptr
get_last_gtid_from_binlog_capsule::find_last_gtid_event(
    std::string_view binlog_name) {
  DBUG_TRACE;

  std::string casted_binlog_name = static_cast<std::string>(binlog_name);

  char search_file_name[FN_REFLEN + 1];
  mysql_bin_log.make_log_name(search_file_name, casted_binlog_name.c_str());

  Binlog_file_reader reader(false /* do not verify checksum */);
  if (reader.open(search_file_name, 0))
    throw std::runtime_error(reader.get_error_str());

  // Here 'is_active()' is called after 'get_binlog_end_pos()' deliberately
  // to properly handle the situation when rotation happens between these
  // two calls
  my_off_t end_pos = mysql_bin_log.get_binlog_end_pos();
  if (!mysql_bin_log.is_active(search_file_name))
    end_pos = std::numeric_limits<my_off_t>::max();

  log_event_ptr ev, last_gtid_ev;
  binlog::Decompressing_event_object_istream istream{reader};

  while (istream >> ev) {
    if (reader.has_fatal_error())
      throw std::runtime_error(reader.get_error_str());
    auto ev_row = ev.get();
    if (ev_row->get_type_code() == binary_log::GTID_LOG_EVENT)
      last_gtid_ev = std::move(ev);
    if (ev_row->common_header->log_pos >= end_pos) break;
  }
  if (istream.has_error()) throw std::runtime_error(istream.get_error_str());
  return last_gtid_ev;
}

DECLARE_STRING_UDF(get_last_gtid_from_binlog_capsule, get_last_gtid_from_binlog)

static const std::array known_udfs{
    DECLARE_UDF_INFO(get_binlog_by_gtid, STRING_RESULT),
    DECLARE_UDF_INFO(get_last_gtid_from_binlog, STRING_RESULT)};

#undef DECLARE_UDF_INFO

static void binlog_utils_my_error(int error_id, myf flags, ...) {
  va_list args;
  va_start(args, flags);
  mysql_service_mysql_runtime_error->emit(error_id, flags, args);
  va_end(args);
}

using udf_bitset_type =
    std::bitset<std::tuple_size<decltype(known_udfs)>::value>;
static udf_bitset_type registered_udfs;

static mysql_service_status_t component_init() {
  // here we use a custom error reporting function
  // 'binlog_utils_my_error()' based on the
  // 'mysql_service_mysql_runtime_error' service instead of the standard
  // 'my_error()' from 'mysys' to get rid of the 'mysys' dependency for this
  // component
  mysqlpp::udf_error_reporter::instance() = &binlog_utils_my_error;

  mysqlpp::register_udfs(mysql_service_udf_registration, known_udfs,
                         registered_udfs);
  return registered_udfs.all() ? 0 : 1;
}

static mysql_service_status_t component_deinit() {
  mysqlpp::unregister_udfs(mysql_service_udf_registration, known_udfs,
                           registered_udfs);
  return registered_udfs.none() ? 0 : 1;
}

// clang-format off
BEGIN_COMPONENT_PROVIDES(CURRENT_COMPONENT_NAME)
END_COMPONENT_PROVIDES();

BEGIN_COMPONENT_REQUIRES(CURRENT_COMPONENT_NAME)
  REQUIRES_SERVICE(udf_registration),
  REQUIRES_SERVICE(component_sys_variable_register),
END_COMPONENT_REQUIRES();

BEGIN_COMPONENT_METADATA(CURRENT_COMPONENT_NAME)
  METADATA("mysql.author", "Percona Corporation"),
  METADATA("mysql.license", "GPL"),
END_COMPONENT_METADATA();

DECLARE_COMPONENT(CURRENT_COMPONENT_NAME, CURRENT_COMPONENT_NAME_STR)
  component_init,
  component_deinit,
END_DECLARE_COMPONENT();
// clang-format on

DECLARE_LIBRARY_COMPONENTS &COMPONENT_REF(CURRENT_COMPONENT_NAME)
    END_DECLARE_LIBRARY_COMPONENTS
