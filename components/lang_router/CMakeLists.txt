# Copyright (c) 2026 Percona LLC and/or its affiliates. All rights reserved.
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; version 2 of the License.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA

# Routes external stored programs to the language component that supports
# the routine's LANGUAGE, so several language components can be installed
# at the same time.
# Built with js_lang (to share LANGUAGE routing with language components
# built separately, such as Percona MLE) or with -DWITH_LANG_ROUTER=ON; off
# otherwise, so a default build stays as upstream.
IF(WITH_JS_LANG)
  SET(WITH_LANG_ROUTER_DEFAULT ON)
ELSE()
  SET(WITH_LANG_ROUTER_DEFAULT OFF)
ENDIF()
OPTION(WITH_LANG_ROUTER "Build the stored program language router component"
  ${WITH_LANG_ROUTER_DEFAULT})
IF(NOT WITH_LANG_ROUTER)
  RETURN()
ENDIF()

MYSQL_ADD_COMPONENT(lang_router
  lang_router.cc
  router.cc
  MODULE_ONLY
  )
