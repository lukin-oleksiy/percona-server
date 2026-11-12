--disable_query_log

#
# Check that the test language backends (component_test_lang_echo and
# component_test_lang_shout) are built: with WITH_JS_LANG or
# WITH_LANG_ROUTER in a build with tests.
#
if (!$TEST_LANG_ECHO_COMPONENT) {
  --skip component requires the environment variable \$TEST_LANG_ECHO_COMPONENT to be set (normally done by mtr), see the file plugin.defs
}

--enable_query_log
