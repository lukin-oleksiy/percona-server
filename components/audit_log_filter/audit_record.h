/* Copyright (c) 2022 Percona LLC and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA */

#ifndef AUDIT_LOG_FILTER_RECORD_H_INCLUDED
#define AUDIT_LOG_FILTER_RECORD_H_INCLUDED

#include "components/audit_log_filter/audit_event_class_internal.h"

#include <string>
#include <string_view>
#include <variant>
#include <vector>

struct mysql_event_tracking_general_data;
struct mysql_event_tracking_connection_data;
struct mysql_event_tracking_table_access_data;
struct mysql_event_tracking_global_variable_data;
struct mysql_event_tracking_startup_data;
struct mysql_event_tracking_shutdown_data;
struct mysql_event_tracking_command_data;
struct mysql_event_tracking_query_data;
struct mysql_event_tracking_stored_program_data;
struct mysql_event_tracking_authentication_data;
struct mysql_event_tracking_message_data;
struct mysql_event_tracking_parse_data;

namespace audit_log_filter {

struct AuditRecordGeneral {
  std::string_view event_class_name;
  std::string_view event_subclass_name;
  audit_event_class_t event_class;
  const mysql_event_tracking_general_data *event;
};

struct AuditRecordConnection {
  std::string_view event_class_name;
  std::string_view event_subclass_name;
  audit_event_class_t event_class;
  const mysql_event_tracking_connection_data *event;
};

struct AuditRecordTableAccess {
  std::string_view event_class_name;
  std::string_view event_subclass_name;
  audit_event_class_t event_class;
  const mysql_event_tracking_table_access_data *event;
};

struct AuditRecordGlobalVariable {
  std::string_view event_class_name;
  std::string_view event_subclass_name;
  audit_event_class_t event_class;
  const mysql_event_tracking_global_variable_data *event;
};

struct AuditRecordServerStartup {
  std::string_view event_class_name;
  std::string_view event_subclass_name;
  audit_event_class_t event_class;
  const mysql_event_tracking_startup_data *event;
};

struct AuditRecordServerShutdown {
  std::string_view event_class_name;
  std::string_view event_subclass_name;
  audit_event_class_t event_class;
  const mysql_event_tracking_shutdown_data *event;
};

struct AuditRecordCommand {
  std::string_view event_class_name;
  std::string_view event_subclass_name;
  audit_event_class_t event_class;
  const mysql_event_tracking_command_data *event;
};

struct AuditRecordQuery {
  std::string_view event_class_name;
  std::string_view event_subclass_name;
  audit_event_class_t event_class;
  const mysql_event_tracking_query_data *event;
};

struct AuditRecordStoredProgram {
  std::string_view event_class_name;
  std::string_view event_subclass_name;
  audit_event_class_t event_class;
  const mysql_event_tracking_stored_program_data *event;
};

struct AuditRecordAuthentication {
  std::string_view event_class_name;
  std::string_view event_subclass_name;
  audit_event_class_t event_class;
  const mysql_event_tracking_authentication_data *event;
};

struct AuditRecordMessage {
  std::string_view event_class_name;
  std::string_view event_subclass_name;
  audit_event_class_t event_class;
  const mysql_event_tracking_message_data *event;
};

struct AuditRecordParse {
  std::string_view event_class_name;
  std::string_view event_subclass_name;
  audit_event_class_t event_class;
  const mysql_event_tracking_parse_data *event;
};

struct AuditRecordUnknown {
  std::string_view event_class_name;
  std::string_view event_subclass_name;
  audit_event_class_t event_class;
  const void *event;
};

using AuditRecordVariant =
    std::variant<AuditRecordGeneral, AuditRecordConnection,
                 AuditRecordTableAccess, AuditRecordGlobalVariable,
                 AuditRecordServerStartup, AuditRecordServerShutdown,
                 AuditRecordCommand, AuditRecordQuery, AuditRecordStoredProgram,
                 AuditRecordAuthentication, AuditRecordMessage,
                 AuditRecordParse, AuditRecordUnknown>;

/**
 * @brief Get AuditRecordVariant instance representing received audit event.
 *
 * @param event_class Received audit event class
 * @param event Received audit event
 * @return An instance of AuditRecordVariant representing audit event
 */
AuditRecordVariant get_audit_record(audit_event_class_t event_class,
                                    const void *event);

}  // namespace audit_log_filter

#endif  // AUDIT_LOG_FILTER_RECORD_H_INCLUDED
