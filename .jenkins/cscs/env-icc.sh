# Copyright (c) 2020 ETH Zurich
#
# SPDX-License-Identifier: BSL-1.0
# Distributed under the Boost Software License, Version 1.0. (See accompanying
# file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

source $SPACK_ROOT/share/spack/setup-env.sh

export CRAYPE_LINK_TYPE=dynamic

# This makes deprecated modules available. This should be removed when a newer
# Boost is available for CrayIntel-20.11.
module use /apps/daint/UES/jenkins/7.0.UP02/gpu/easybuild/modules/all

module load daint-gpu
module switch PrgEnv-cray PrgEnv-intel
module load Boost/1.75.0-CrayIntel-20.08
module load hwloc/.2.0.3
spack load cmake
spack load ninja

export CXX=`which CC`
export CC=`which cc`

configure_extra_options="-DCMAKE_BUILD_TYPE=Debug"
configure_extra_options+=" -DHPX_WITH_MAX_CPU_COUNT=128"
configure_extra_options+=" -DHPX_WITH_MALLOC=system"
configure_extra_options+=" -DHPX_WITH_COMPILER_WARNINGS=ON"
configure_extra_options+=" -DHPX_WITH_COMPILER_WARNINGS_AS_ERRORS=ON"
configure_extra_options+=" -DHPX_WITH_SPINLOCK_DEADLOCK_DETECTION=ON"
configure_extra_options+=" -DHWLOC_ROOT=${EBROOTHWLOC}"
