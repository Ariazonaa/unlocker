#pragma once

#include <windows.h>

#include <ole2.h>

#include <cstddef>

// IDropSource für DoDragDrop im Drag & Drop-Selbsttest: meldet, dass wir Dateien ziehen (DROPEFFECT_COPY),
// damit der OLE-Drag-Manager das Ziehen und das Drop zulässt. Das eigentliche Datenobjekt (CF_HDROP)
// kommt vom echten Shell-Dateidatenobjekt (IShellItem::BindToHandler(BHID_DataObject)).
namespace dropsrc {
IDropSource* make_drop_source();
// Anzahl der QueryContinueDrag-Aufrufe des OLE-Drag-Loops (Diagnose: 0 = Loop hat nie gepumpt).
std::size_t qcd_calls();
}  // namespace dropsrc
