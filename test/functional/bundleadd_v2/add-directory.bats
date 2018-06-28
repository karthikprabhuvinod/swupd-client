#!/usr/bin/env bats

load "../testlib"

setup() {

	create_test_environment "$TEST_NAME"
	create_bundle -n test-bundle -d /usr/bin/test "$TEST_NAME"

}

teardown() {

	destroy_test_environment "$TEST_NAME"

}

@test "bundle-add add bundle containing a directory" {

	run sudo sh -c "$SWUPD bundle-add $SWUPD_OPTS test-bundle"
	echo "Actual status: $status"
	echo "$output" >&3
	[ "$status" -eq 0 ]
	[ -d "$TEST_NAME/target-dir/usr/bin/test" ]
	# TODO(castulo): refactor and enable the check_lines function
	# check_lines "$output"

} 
