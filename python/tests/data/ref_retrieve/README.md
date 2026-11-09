# Frozen reference: kimi-agent's removed `retrieve` surface

`src/builtin_tools/retrieve_tool.{h,cpp}` ports kimi-agent's `retrieve`
conversation-history recall tool, and the C++ CLI still wires it up
(`src/agent/soul.cpp` installs it with a `HistoryIndexView`), so the kernel is
live native code with its own Boost.UT target (`test_builtin_retrieve`).

Upstream deleted the Python side it was ported from:

```
commit 8a11f88b3f92d71d303061caec9b6a1762ac531f   "remove retrieve"
Date:   Fri Oct 9 10:29:51 2026 +0800

  deleted: kimi-cli/src/kimi_cli/tools/memory/__init__.py   (the `retrieve` tool)
           HistoryIndex.get_by_id
           HistoryIndex.search_with_recency
           context_prune's `prune_<n>` reference surface
           soul/dynamic_injections/context_meter.py
  README.md: "Auto History Retrieval + On-Demand Recall" -> "Auto History Retrieval"
  agents' YAML manifests: the `Retrieve` tool entry
```

The two files here are that revision's code, unmodified, so the differential
tests keep comparing the port against the body it was ported from instead of
comparing it against nothing:

* `memory_retrieve.py` — `…/tools/memory/__init__.py` at `8a11f88b^`
  (136 lines: `Params` + the `retrieve` CallableTool2).
* `history_index_methods.py` — the two `HistoryIndex` methods deleted by
  `8a11f88b`, verbatim from `…/soul/history_index.py` at `8a11f88b^`
  (lines 555-626), mixed onto the *current* `HistoryIndex` by `bind()`, so
  everything except the deleted surface is still upstream's current code.

Re-derive with:

```
git -C C:/dev/kimi-agent show 8a11f88b^:kimi-cli/src/kimi_cli/tools/memory/__init__.py
git -C C:/dev/kimi-agent show 8a11f88b^:kimi-cli/src/kimi_cli/soul/history_index.py
```

Nothing here is a substitute for a *live* reference: `python/tests/test_parity_retrieve.py`
still compares the BM25 kernels and the tokenizer against `kimix.retrieval`,
which upstream kept.
