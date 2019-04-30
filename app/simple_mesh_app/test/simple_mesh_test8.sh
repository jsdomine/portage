#!/bin/bash
: <<'END'
This file is part of the Ristra portage project.
Please see the license file at the root of this repository, or at:
    https://github.com/laristra/portage/blob/master/LICENSE
END


# Exit on error
set -e
# Echo each command
set -x

# 3d 2nd order node-centered remap of quad func
${RUN_COMMAND} $TESTAPPDIR/simple_mesh_app 8 4 5

# Compare the values for the field
$CMPAPPDIR/apptest_cmp field_gold8.txt field8.txt 1e-12