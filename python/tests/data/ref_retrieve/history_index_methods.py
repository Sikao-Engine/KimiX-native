"""Frozen reference: the two ``HistoryIndex`` methods kimi-agent removed.

``retrieve`` was kimi-agent's on-demand conversation-history recall tool.  The
``retrieve_tool.cpp`` port in this repo is still live native code (the C++ CLI
wires it up in ``src/agent/soul.cpp``), so its Python ground truth has to stay
available even though the reference checkout no longer has it:

    commit 8a11f88b3f92d71d303061caec9b6a1762ac531f   "remove retrieve"
    Fri Oct 9 10:29:51 2026 +0800
    deletes  kimi-cli/src/kimi_cli/tools/memory/__init__.py        (the tool)
             HistoryIndex.get_by_id  / HistoryIndex.search_with_recency
             context_prune's ``prune_<n>`` reference surface
             soul/dynamic_injections/context_meter.py
    and rewrites README.md "Auto History Retrieval + On-Demand Recall"
    -> "Auto History Retrieval".

The two method bodies below are copied *verbatim* from

    git -C <kimi-agent> show 8a11f88b^:kimi-cli/src/kimi_cli/soul/history_index.py

(lines 555-626 of that revision) so the differential assertions keep comparing
the C++ kernels against the code they were ported from.  ``bind()`` mixes them
onto the *current* reference ``HistoryIndex`` -- the rest of the class (state,
``search``, ``_row_to_turn``, the legacy/``FTS5`` switches) is still upstream's
current code, so only the deleted surface is frozen.
"""

from __future__ import annotations

import math
import time
from typing import Any


class RemovedHistoryIndexMethods:
    """The reference bodies upstream deleted in 8a11f88b ("remove retrieve")."""

    def get_by_id(self, ref: str) -> dict[str, Any] | None:
        """Retrieve a turn by its reference ID (turn_id).

        The *ref* may be a plain integer string (``"42"``) or a prefixed
        reference like ``"prune_42"`` — the numeric suffix is tried.
        """
        ref_str = str(ref)
        if ref_str.startswith("prune_"):
            ref_str = ref_str[len("prune_"):]
        try:
            turn_id = int(ref_str)
        except ValueError:
            return None

        if self._use_fts:
            cursor = None
            try:
                conn = self._ensure_conn()
                cursor = conn.execute(
                    "SELECT turn_id, role, text, timestamp, is_compacted FROM turns WHERE turn_id = ?",
                    (turn_id,),
                )
                row = cursor.fetchone()
                if row is None:
                    return None
                return self._row_to_turn(row)
            except Exception:
                return None
            finally:
                if cursor is not None:
                    try:
                        cursor.close()
                    except Exception:
                        pass

        for turn in self._turns_list:
            if turn["turn_id"] == turn_id:
                return dict(turn)
        return None

    def search_with_recency(
        self,
        query: str,
        *,
        top_k: int = 3,
        recency_weight: float = 1.0,
    ) -> list[dict[str, Any]]:
        """BM25 search with recency boosting.

        boosted_score = bm25_score * (1 + recency_weight * exp(-hours_ago / 24.0))
        """
        turns = self._turns if self._use_fts else self._turns_list
        if not turns:
            return []

        # Fetch a larger candidate pool so recency re-ranking has room to work
        candidates = self.search(query, top_k=top_k * 3)
        if not candidates:
            return []

        now = time.time()
        scored: list[tuple[float, dict[str, Any]]] = []
        for turn in candidates:
            bm25_score = turn.get("score", 0.0)
            hours_ago = (now - turn["timestamp"]) / 3600.0
            boost = 1.0 + recency_weight * math.exp(-hours_ago / 24.0)
            boosted_score = bm25_score * boost
            scored.append((boosted_score, {**turn, "boosted_score": boosted_score}))

        scored.sort(key=lambda x: x[0], reverse=True)
        return [turn for _, turn in scored[:top_k]]


def bind(history_index_module):
    """*history_index_module* with the removed methods mixed back in.

    Returns a module-like object exposing ``HistoryIndex`` (the current class
    subclassed with the frozen methods) and the module's ``time`` -- the
    reference body reads ``time.time()`` through it, which the tests pin with
    ``mock.patch.object``.
    """
    import types

    mixed = types.SimpleNamespace()
    mixed.HistoryIndex = type(
        "HistoryIndex",
        (RemovedHistoryIndexMethods, history_index_module.HistoryIndex),
        {"__module__": history_index_module.__name__},
    )
    mixed.time = history_index_module.time
    return mixed
