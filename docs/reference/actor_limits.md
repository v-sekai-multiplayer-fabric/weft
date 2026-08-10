# Actor limits

Goal: add a `Weft.Limits` module. Enforce the limits at the store, the gateway, and the
lifecycle.

State: not started. Values captured: 10 GiB storage, 2 KiB key, 128 KiB value, 60 s per
action, 1200 requests per minute per IP, 32 in-flight requests.

Next: add `Weft.Limits` with these values. Enforce them at the store put, the gateway
request path, and the actor lifecycle. This task is pure Elixir and buildable now.
