#pragma once

#ifndef SCARLEX_DIAGNOSTICS
#define SCARLEX_DIAGNOSTICS 0
#endif
#ifndef SCARLEX_PUBLIC_RELEASE
#define SCARLEX_PUBLIC_RELEASE 0
#endif

namespace scarlex::build {
inline constexpr bool diagnostics = SCARLEX_DIAGNOSTICS != 0;
inline constexpr bool public_release = SCARLEX_PUBLIC_RELEASE != 0;
static_assert(!public_release || !diagnostics, "Public releases cannot enable diagnostics");
}
