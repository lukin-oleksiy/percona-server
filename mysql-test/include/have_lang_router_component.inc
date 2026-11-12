--disable_query_log

#
# Check if the variable LANG_ROUTER_COMPONENT is set (component_lang_router is
# built with -DWITH_LANG_ROUTER=ON, the default with WITH_JS_LANG)
#
if (!$LANG_ROUTER_COMPONENT) {
  --skip component requires the environment variable \$LANG_ROUTER_COMPONENT to be set (normally done by mtr), see the file plugin.defs
}

--enable_query_log
