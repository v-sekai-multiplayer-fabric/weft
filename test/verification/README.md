# TPC-C verification harness

Uses [plausible-witness-dag](https://github.com/fire/plausible-witness-dag)
to search for TPC-C invariant violations via iterative-deepening property-based
testing.

## How it works

1. Plausible generates random transaction parameter combinations
2. The candidate predicate checks if those parameters produce a violation
3. The deterministic readback queries the database (via psql) to verify
4. The escalation ladder widens the search budget if lower levels find nothing

## Invariants checked

| Invariant | What it searches for |
|-----------|---------------------|
| NewOrder atomicity | OORDER exists but ORDER_LINE count ≠ ol_cnt |
| Delivery correctness | Delivered order still in NEW_ORDER table |
| Stock non-negative | Stock row with s_quantity < 0 |

## Running

```bash
# Start h2o-bench-tpcc first, then:
cd test/verification
lake build tpcc_verify
lake exe tpcc_verify
```

## Integration with plausible-witness-dag

This project depends on `plausible-witness-dag` as a Lake dependency. The
`resolve` function takes a candidate predicate and a deterministic readback
function, then escalates through the ladder (L0 → L1 → L2) until it finds
a witness or proves none exists.
