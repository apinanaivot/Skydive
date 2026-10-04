#pragma once

namespace menu {

// The options overlay (menu.cpp). Install at plugin load (needs the trampoline), Init once the
// renderer exists (SKSE's data loaded message).
void Install();
void Init();
bool IsOpen();

}  // namespace menu
