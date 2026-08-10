import Lake
open Lake DSL

package zone_server_h2o_verification

require plausible from git
  "https://github.com/leanprover-community/plausible.git" @ "main"

require PlausibleWitnessDag from git
  "https://github.com/fire/plausible-witness-dag.git" @ "main"

lean_lib ZoneVerification where
  roots := #[ZoneVerification]

lean_exe zone_verify where
  root := `Main
