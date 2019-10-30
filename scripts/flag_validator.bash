#!/bin/bash

# shellcheck source=scripts/flag_validator_common.bash
source $(dirname ${BASH_SOURCE[0]})/flag_validator_common.bash
conflict=0

usage() {

	cat <<-EOM

		Validates that the flags used in swupd are not duplicated

		Usage:
		    flag_validator.bash <command> <flag>

		Example:
		    flag_validator.bash bundle-add w

		Notes:
		    If run with no argument, it validates the existing flags
		    If run with the <command> <flag> arguments, it validate that <flag> is a valid option for <command>
		    If <flag> is a global option, use "global <flag>"

	EOM

}

# Entry point
arg=$1
flag=$2

if [ "$arg" == "--help" ] || [ "$arg" == "-h" ]; then
	usage
	exit
fi

# If run with no argument, it validates the existing flags
if [ -z "$arg" ]; then
	validate_existing_flags
	exit
fi

# If run with the <command> <flag> arguments, it validates
# that <flag> is a valid option for <command>
if [ -z "$flag" ]; then
	echo "Error: Mandatory argument missing"
	usage
	exit 1
fi

swupd_commands+=("global")
for val in "${swupd_commands[@]}"; do
	if [ "$val" == "$arg" ]; then
		# remove the '-' if it has one
		flag=$(echo "$flag" | tr --delete '-')
		validate_flag "$arg" "$flag"
		exit "$conflict"
	fi
done

echo "Error: The provided command '$arg' is invalid"
echo "Available commands:"
echo "${swupd_commands[@]}" | tr ' ' '\n'
exit 1
