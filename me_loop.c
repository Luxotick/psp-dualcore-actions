#include "common.h"
#include <me-core-mapper/me-core-mapper.h>

void me_loop(void *param)
{
    DualCoreBridge *bridge = (DualCoreBridge *)param;

    if (bridge == 0) {
        return;
    }

    meCoreDcacheInvalidateRange(bridge, sizeof *bridge);
    bridge->output = bridge->input + 5u;
    bridge->state = BRIDGE_STATE_DONE;
    meCoreDcacheWritebackRange(bridge, sizeof *bridge);
}
