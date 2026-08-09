//! weft data plane, native.
//!
//! This is the hot loop of a plane. It is a single-writer seqlock ring, the same layout
//! as `Weft.DataPlane.Ring` on the BEAM. One thread per core writes or reads. The code is
//! pure Rust plus `core_affinity`, so it builds and runs natively on Windows and Linux.
//!
//! iceoryx2 carries the ring between planes. The transport wiring lands with the native
//! store plane (task #48). See `docs/runtime-choice.md` for the runtime decision.

use std::sync::atomic::{fence, AtomicI32, AtomicU64, Ordering};

/// A single-writer seqlock ring. One writer stores a tick and a flat `[x, y, z, ...]`
/// snapshot. Any number of readers take a consistent copy without a lock.
pub struct Ring {
    seq: AtomicU64,
    tick: AtomicU64,
    data: Vec<AtomicI32>,
}

impl Ring {
    /// A ring for `max_entities` points, three coordinates each.
    pub fn new(max_entities: usize) -> Self {
        let mut data = Vec::with_capacity(max_entities * 3);
        for _ in 0..max_entities * 3 {
            data.push(AtomicI32::new(0));
        }
        Ring {
            seq: AtomicU64::new(0),
            tick: AtomicU64::new(0),
            data,
        }
    }

    /// The number of coordinate slots (`max_entities * 3`).
    pub fn slots(&self) -> usize {
        self.data.len()
    }

    /// Write one snapshot. The sequence is odd during the write and even after it.
    pub fn write(&self, tick: u64, coords: &[i32]) {
        let s = self.seq.load(Ordering::Relaxed);
        self.seq.store(s + 1, Ordering::Relaxed);
        fence(Ordering::Release);
        self.tick.store(tick, Ordering::Relaxed);
        for (slot, value) in self.data.iter().zip(coords.iter()) {
            slot.store(*value, Ordering::Relaxed);
        }
        fence(Ordering::Release);
        self.seq.store(s + 2, Ordering::Relaxed);
    }

    /// Read the latest snapshot into `out`. Retries while a write is in progress. Returns
    /// the tick of the snapshot that was read.
    pub fn read(&self, out: &mut [i32]) -> u64 {
        loop {
            let s1 = self.seq.load(Ordering::Relaxed);
            if s1 & 1 == 1 {
                std::hint::spin_loop();
                continue;
            }
            fence(Ordering::Acquire);
            let tick = self.tick.load(Ordering::Relaxed);
            for (slot, value) in self.data.iter().zip(out.iter_mut()) {
                *value = slot.load(Ordering::Relaxed);
            }
            fence(Ordering::Acquire);
            if self.seq.load(Ordering::Relaxed) == s1 {
                return tick;
            }
        }
    }
}

/// Pin the current thread to one core. Returns `true` when the pin is set.
pub fn pin_to_core(index: usize) -> bool {
    match core_affinity::get_core_ids() {
        Some(ids) if index < ids.len() => core_affinity::set_for_current(ids[index]),
        _ => false,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn read_returns_the_last_write() {
        let ring = Ring::new(8);
        let coords: Vec<i32> = (0..24).collect();
        ring.write(42, &coords);
        let mut out = vec![0i32; ring.slots()];
        let tick = ring.read(&mut out);
        assert_eq!(tick, 42);
        assert_eq!(out, coords);
    }

    #[test]
    fn a_reader_thread_sees_writes() {
        use std::sync::Arc;
        let ring = Arc::new(Ring::new(8));
        let writer = Arc::clone(&ring);
        let handle = std::thread::spawn(move || {
            for t in 1..=1000u64 {
                let coords = vec![t as i32; 24];
                writer.write(t, &coords);
            }
        });
        handle.join().unwrap();
        let mut out = vec![0i32; ring.slots()];
        let tick = ring.read(&mut out);
        assert_eq!(tick, 1000);
        assert!(out.iter().all(|&v| v == 1000));
    }
}
