require "common"

MAX_CYCLE_COUNT = 1000

connect_and_load("selfmod")

expect_testpoints = {
	{ TP_SUCCESS, 0 }
}

return step_testpoints(expect_testpoints)