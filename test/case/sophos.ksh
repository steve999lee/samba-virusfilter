T_svf_module_name="svf-sophos"
T_scanner_name="savdid"
T_scanner_pid=""

. "$TEST_case_dir/common.ksh"

function tc_all
{
  tcs_common
  tcs_scanner_socket
}

