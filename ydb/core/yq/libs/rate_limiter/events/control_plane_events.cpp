#include "control_plane_events.h"

namespace NYq {

NActors::TActorId RateLimiterControlPlaneServiceId() {
    constexpr TStringBuf name = "RATE_LIM_CP";
    return NActors::TActorId(0, name);
}

} // namespace NYq
