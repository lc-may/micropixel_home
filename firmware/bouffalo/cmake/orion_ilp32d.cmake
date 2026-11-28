# SPDX-License-Identifier: Apache-2.0
# The pinned SDK's unsuffixed multimedia and PKA archives are ILP32F.
# Use the ILP32D archives for the Orion AP while keeping CPU_MODEL=b1.
get_property(_libraries GLOBAL PROPERTY SDK_LIBS)
list(TRANSFORM _libraries REPLACE "(/lib(multimedia|pka)_bl618dg)[.]a$" "\\1_b0.a")
set_property(GLOBAL PROPERTY SDK_LIBS ${_libraries})
