// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// bbport: thread snapshots while no frame is shown (bbport_hang.cpp).
namespace BbHang {
/// VideoOut showed a frame.
void OnFlip();

/// The file of a ring of `files` to write after `last` (-1: none yet), never `kept` (-1: none).
/// The snapshot taken when frames stop keeps its file until frames stop again; the later ones of
/// that hang and the periodic ones take the other files in turn.
inline int NextSnapshotFile(int last, int kept, int files) {
    int next = (last + 1) % files;
    if (next == kept) {
        next = (next + 1) % files;
    }
    return next;
}
} // namespace BbHang
