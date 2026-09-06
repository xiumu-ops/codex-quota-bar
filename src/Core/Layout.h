#pragma once

#include "Core/Constants.h"
#include "Core/Models.h"

namespace CodexQuotaBar {

    inline int MiniQuotaSlotCount(const QuotaSnapshot& snapshot) {
        return snapshot.window.available && snapshot.weekly.available ? 2 : 1;
    }

    inline int MiniBarLogicalWidth(const QuotaSnapshot& snapshot) {
        return MiniQuotaSlotCount(snapshot) == 2
            ? MINI_DUAL_WIDTH
            : MINI_SINGLE_WIDTH;
    }

} // namespace CodexQuotaBar
