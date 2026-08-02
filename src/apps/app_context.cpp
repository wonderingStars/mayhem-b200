/* mayhem-b200 — shared application context. SPDX-License-Identifier: GPL-2.0-or-later */

#include "app_context.hpp"

namespace app {

Context& globals() {
    static Context ctx{};
    return ctx;
}

}  // namespace app
