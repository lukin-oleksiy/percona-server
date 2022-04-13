# Copyright (c) 2022 Percona LLC and/or its affiliates. All rights reserved.
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; version 2 of the License.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA

OPTION(WITH_COMPONENT_AUDIT_LOG_FILTER "Build with Percona Audit Log Filter component" ON)

IF(WITH_COMPONENT_AUDIT_LOG_FILTER)
  ADD_DEFINITIONS(-DLOG_COMPONENT_TAG="audit_log_filter")

  INCLUDE_DIRECTORIES(SYSTEM ${BOOST_PATCHES_DIR} ${BOOST_INCLUDE_DIR} ${ZLIB_INCLUDE_DIRS})

  SET(AUDIT_LOG_FILTER_SOURCES
      audit_log_filter.cc
      audit_record.cc
      audit_filter.cc
      audit_rule.cc
      audit_rule_registry.cc
      audit_psi_info.cc
      audit_udf.cc
      component_registry_service.cc
      sys_vars.cc
      audit_table/base.cc
      audit_table/audit_log_filter.cc
      audit_table/audit_log_user.cc
      log_record_formatter.cc
      log_record_formatter/base.cc
      log_record_formatter/new.cc
      log_record_formatter/json.cc
      log_writer.cc
      log_writer/base.cc
      log_writer/file.cc
      log_writer/syslog.cc
      log_writer/file_handle.cc
  )

  MYSQL_ADD_COMPONENT(audit_log_filter ${AUDIT_LOG_FILTER_SOURCES}
                      LINK_LIBRARIES OpenSSL::SSL OpenSSL::Crypto library_mysys extra::rapidjson ext::zlib
                      MODULE_ONLY)

  IF(APPLE)
    SET_TARGET_PROPERTIES(component_audit_log_filter PROPERTIES
      LINK_FLAGS "-undefined dynamic_lookup")
  ENDIF()

  IF(UNIX)
    IF(INSTALL_MYSQLSHAREDIR)
      INSTALL(FILES "audit_log_filter_linux_install.sql"
              DESTINATION ${INSTALL_MYSQLSHAREDIR}
              COMPONENT Server
              PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ)
    ENDIF()
  ENDIF()
ENDIF()
