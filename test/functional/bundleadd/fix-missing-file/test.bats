#!/usr/bin/env bats

load "../../swupdlib"

t1_hash="e6d85023c5e619eb43d5cfbfdbdec784afef5a82ffa54e8c93bda3e0883360a3"

setup() {
  clean_test_dir
  create_manifest_tar 10 MoM
  create_manifest_tar 10 os-core
  create_manifest_tar 10 test-bundle
  create_fullfile_tar 10 $t1_hash
}

teardown() {
  clean_tars 10
  clean_tars 10 files
  revert_chown_root "$DIR/web-dir/10/files/$t1_hash"
  sudo rm "$DIR/target-dir/foo"
}

@test "bundle-add verify_fix_path support" {
  run sudo sh -c "$SWUPD bundle-add $SWUPD_OPTS test-bundle"

  check_lines "$output"
  [ -f "$DIR/target-dir/foo" ]
}

# vi: ft=sh ts=8 sw=2 sts=2 et tw=80
