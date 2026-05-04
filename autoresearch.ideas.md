# Autoresearch Ideas

- Add a prepared-statement cache inside `MemoryStore` if prepare/finalize overhead shows up after algorithmic wins.
- Consider storing/searching compact normalized vectors or precomputed norms to reduce per-row allocation and cosine work without changing memory semantics.
