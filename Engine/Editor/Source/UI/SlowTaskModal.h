#pragma once

namespace Lumina::SlowTaskModal
{
    // Blocking progress popup for live FScopedSlowTasks, called only from FEditorModalManager::DrawDialogue so it never competes as a sibling modal.
    void Render();
}
