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

#ifndef AUDIT_LOG_FILTER_H_INCLUDED
#define AUDIT_LOG_FILTER_H_INCLUDED

#include "components/audit_log_filter/audit_error_log.h"
#include "components/audit_log_filter/audit_event_class_internal.h"

#include <mysql/components/services/mysql_current_thread_reader.h>

#include "components/audit_log_filter/audit_record.h"
#include "components/audit_log_filter/component_registry_service.h"

#include <atomic>
#include <memory>

extern SERVICE_TYPE(log_builtins) * log_bi;
extern SERVICE_TYPE(log_builtins_string) * log_bs;

namespace audit_log_filter {
namespace log_writer {
class LogWriterBase;
}  // namespace log_writer

class AuditEventFilter;
class AuditRuleRegistry;
class AuditUdf;
class SysVars;

class AuditLogFilter {
 public:
  AuditLogFilter() = delete;
  AuditLogFilter(comp_registry_srv_container_t comp_registry_srv,
                 std::unique_ptr<AuditRuleRegistry> audit_rules_registry,
                 std::unique_ptr<AuditUdf> audit_udf,
                 std::shared_ptr<SysVars> sys_vars,
                 std::unique_ptr<log_writer::LogWriterBase> log_writer);

  /**
   * @brief Process audit event.
   *
   * @param event_class Event class
   * @param event Event info
   * @return Event processing status, 0 in case of success or non-zero code
   *         otherwise
   */
  int notify_event(audit_event_class_t event_class, const void *event);

  /**
   * @brief Get UDFs handler instance.
   * @return UDF handler instance
   */
  AuditUdf *get_udf() noexcept { return m_audit_udf.get(); }

 public:
  /**
   * @brief Handle filters flush request.
   */
  void on_audit_rule_flush_requested() noexcept;

  /**
   * @brief Handle log file flush request.
   */
  void on_audit_log_flush_requested() noexcept;

  /**
   * @brief Handle log files prunning request.
   */
  void on_audit_log_prune_requested() noexcept;

 private:
  void get_connection_attrs(MYSQL_THD thd, AuditRecordVariant &audit_record);

  /**
   * @brief Get user and host name from connection THD instance
   *
   * @param thd Server thread instance
   * @param user_name Returned user name
   * @param user_host Returned host name
   * @return true in case user and host name are fetched successfully,
   *         false otherwise
   */
  bool get_connection_user(MYSQL_THD thd, std::string &user_name,
                           std::string &user_host) noexcept;

 private:
  comp_registry_srv_container_t m_comp_registry_srv;
  std::unique_ptr<AuditRuleRegistry> m_audit_rules_registry;
  std::unique_ptr<AuditUdf> m_audit_udf;
  std::shared_ptr<SysVars> m_sys_vars;
  std::unique_ptr<log_writer::LogWriterBase> m_log_writer;
  std::unique_ptr<AuditEventFilter> m_filter;
};

/**
 * @brief Get audit log filter plugin instance.
 *
 * @return Audit log filter plugin instance
 */
AuditLogFilter *get_audit_log_filter_instance() noexcept;

}  // namespace audit_log_filter

#endif  // AUDIT_LOG_FILTER_H_INCLUDED
