#pragma once

// Mesh routing (spec docs/superpowers/specs/2026-11-03-mesh-routing-design.md): a custom OMR prefix, and the route
// into the mesh announced by DHCPv6-PD, RIPng or BGP on the backbone. Public interface of the component.

#include "routing_types.h"
#include "settings.h"
#include "announce.h"
#include "bgp_msg.h"
#include "ripng.h"
