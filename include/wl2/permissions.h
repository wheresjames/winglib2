#pragma once

/**
 * @file permissions.h
 * @brief Helpers for normalizing and comparing runtime permission sets.
 */

#include "wl2/runtime.h"

namespace wl2 {

/// Return a copy with stable ordering, duplicate entries removed, and paths normalized.
PermissionSet normalizePermissionSet(const PermissionSet& permissions);

/// True when every permission in requested is covered by approved.
bool permissionSetContains(const PermissionSet& approved, const PermissionSet& requested);

/// Return the permissions in requested that are not covered by approved.
PermissionSet permissionSetDelta(const PermissionSet& approved, const PermissionSet& requested);

} // namespace wl2
