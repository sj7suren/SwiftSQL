// SyncCrossEngineGate.cpp -- see header. Body moved verbatim from
// SyncWizardDialog.cpp's former anonymous-namespace definition.
#include "ui/SyncCrossEngineGate.h"

namespace ui {

bool CrossEngineSupported(db::Dialect a, db::Dialect b)
{
    if (a == b) return true;
    auto isMyOrPg = [](db::Dialect d) {
        return d == db::Dialect::MySQL || d == db::Dialect::Postgres;
    };
    return isMyOrPg(a) && isMyOrPg(b);
}

} // namespace ui
