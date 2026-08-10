import ZoneVerification.Spsc

def main (_args : List String) : IO Unit := do
  IO.println "zone-server-h2o verification harness"
  IO.println "Ported from weftspun/h2o-bench-tpcc's SPSC ring proof (RFD 0008)."
  IO.println "Zonefabric-specific invariants (entity migration, ghost consistency,"
  IO.println "journal replay) land here as the corresponding features are built."
