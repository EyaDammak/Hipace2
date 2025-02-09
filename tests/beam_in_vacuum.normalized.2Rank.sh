#! /usr/bin/env bash

# Copyright 2020-2021
#
# This file is part of HiPACE++.
#
# Authors: MaxThevenet, Severin Diederichs
# License: BSD-3-Clause-LBNL


# This file is part of the HiPACE++ test suite.
# It runs a Hipace simulation for a can beam in vacuum in serial and parallel and
# checks that they give the same result

# abort on first encounted error
set -eu -o pipefail

# Read input parameters
HIPACE_EXECUTABLE=$1
HIPACE_SOURCE_DIR=$2

HIPACE_EXAMPLE_DIR=${HIPACE_SOURCE_DIR}/examples/beam_in_vacuum
HIPACE_TEST_DIR=${HIPACE_SOURCE_DIR}/tests

FILE_NAME=`basename "$0"`
TEST_NAME="${FILE_NAME%.*}"

mpiexec -n 1 $HIPACE_EXECUTABLE $HIPACE_EXAMPLE_DIR/inputs_normalized \
        hipace.tile_size = 8 \
        amr.n_cell=128 256 30 \
        beam.radius = 20. \
        diagnostic.field_data = all rho \
        hipace.file_prefix=REF_diags/hdf5 \
        max_step = 1

mpiexec -n 2 $HIPACE_EXECUTABLE $HIPACE_EXAMPLE_DIR/inputs_normalized \
        hipace.tile_size = 8 \
        amr.n_cell=128 256 30 \
        beam.radius = 20. \
        diagnostic.field_data = all rho \
        hipace.file_prefix=$TEST_NAME \
        max_step = 1

$HIPACE_EXAMPLE_DIR/analysis_2ranks.py --output-dir=$TEST_NAME
