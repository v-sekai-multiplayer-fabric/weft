//! Throughput smoke for the weft data-plane ring. It pins one core, writes many
//! snapshots, and reports the rate. This mirrors `bench/ring_native.c` and runs on both
//! Windows and Linux.

use weft_dataplane::{pin_to_core, Ring};

fn main() {
    let iterations: u64 = std::env::args()
        .nth(1)
        .and_then(|a| a.parse().ok())
        .unwrap_or(50_000_000);

    let pinned = pin_to_core(0);
    let ring = Ring::new(8);
    let mut coords: Vec<i32> = (0..24).collect();

    let start = std::time::Instant::now();
    for tick in 0..iterations {
        coords[0] = tick as i32;
        ring.write(tick, &coords);
    }
    let elapsed = start.elapsed();

    let per_op = elapsed.as_nanos() as f64 / iterations as f64;
    let rate = iterations as f64 / elapsed.as_secs_f64() / 1_000_000.0;
    println!("weft data plane: pinned={pinned}");
    println!(
        "wrote {iterations} snapshots in {:.3} s",
        elapsed.as_secs_f64()
    );
    println!("rate: {rate:.1} M snapshots/sec, {per_op:.1} ns/snapshot");
}
