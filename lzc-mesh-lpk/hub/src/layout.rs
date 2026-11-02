//! Layout geometry: snapping devices together and finding the neighbour
//! behind a screen edge. Units are logical points of each device.

use std::collections::BTreeMap;

use crate::protocol::Dir;
use crate::store::Device;

const EPS: f64 = 0.5;
/// Minimum shared edge length for two screens to count as neighbours.
const MIN_OVERLAP: f64 = 40.0;
/// Distance within which a dragged screen aligns its edges/centre to a neighbour.
const ALIGN: f64 = 48.0;

fn overlap(a0: f64, a1: f64, b0: f64, b1: f64) -> f64 {
    a1.min(b1) - a0.max(b0)
}

fn intersects(a: &Device, x: f64, y: f64, w: f64, h: f64) -> bool {
    overlap(a.x, a.right(), x, x + w) > EPS && overlap(a.y, a.bottom(), y, y + h) > EPS
}

/// Picks the free spot touching another screen that is closest to (x, y).
pub fn snap(devices: &BTreeMap<String, Device>, id: &str, x: f64, y: f64) -> (f64, f64) {
    let Some(me) = devices.get(id) else { return (x, y) };
    let (w, h) = (me.w, me.h);
    let others: Vec<&Device> = devices.values().filter(|d| d.id != id).collect();
    if others.is_empty() {
        return (0.0, 0.0);
    }
    let mut best: Option<(f64, f64, f64)> = None;
    for o in &others {
        let mut candidates = Vec::new();
        let along_x = |cx: f64| align(cx, w, o.x, o.w).clamp(o.x - w + MIN_OVERLAP, o.right() - MIN_OVERLAP);
        let along_y = |cy: f64| align(cy, h, o.y, o.h).clamp(o.y - h + MIN_OVERLAP, o.bottom() - MIN_OVERLAP);
        candidates.push((o.x - w, along_y(y)));
        candidates.push((o.right(), along_y(y)));
        candidates.push((along_x(x), o.y - h));
        candidates.push((along_x(x), o.bottom()));
        for (cx, cy) in candidates {
            if others.iter().any(|d| intersects(d, cx, cy, w, h)) {
                continue;
            }
            let dist = (cx - x).powi(2) + (cy - y).powi(2);
            if best.is_none_or(|(_, _, b)| dist < b) {
                best = Some((cx, cy, dist));
            }
        }
    }
    match best {
        Some((bx, by, _)) => (bx.round(), by.round()),
        None => {
            let right = others.iter().map(|d| d.right()).fold(f64::MIN, f64::max);
            let top = others.iter().map(|d| d.y).fold(f64::MAX, f64::min);
            (right, top)
        }
    }
}

/// Aligns start, centre or end of a span to the neighbour when close enough.
fn align(v: f64, len: f64, o0: f64, olen: f64) -> f64 {
    let options = [o0, o0 + (olen - len) / 2.0, o0 + olen - len];
    options
        .into_iter()
        .min_by(|a, b| (a - v).abs().total_cmp(&(b - v).abs()))
        .filter(|a| (a - v).abs() <= ALIGN)
        .unwrap_or(v)
}

/// Position for a device that has never been placed: right of everything.
pub fn place_new(devices: &BTreeMap<String, Device>) -> (f64, f64) {
    if devices.is_empty() {
        return (0.0, 0.0);
    }
    let right = devices.values().map(|d| d.right()).fold(f64::MIN, f64::max);
    let top = devices.values().map(|d| d.y).fold(f64::MAX, f64::min);
    (right, top)
}

/// Device whose opposite edge touches `from` on side `dir` at global point (gx, gy).
/// When `pos_exact` is false the closest neighbour along the edge is accepted.
pub fn neighbour<'a, I>(online: I, from: &Device, dir: Dir, gx: f64, gy: f64) -> Option<&'a Device>
where
    I: Iterator<Item = &'a Device>,
{
    online
        .filter(|d| d.id != from.id)
        .filter(|d| match dir {
            Dir::Left => (d.right() - from.x).abs() < EPS + 1.0 && gy >= d.y - EPS && gy <= d.bottom() + EPS,
            Dir::Right => (d.x - from.right()).abs() < EPS + 1.0 && gy >= d.y - EPS && gy <= d.bottom() + EPS,
            Dir::Up => (d.bottom() - from.y).abs() < EPS + 1.0 && gx >= d.x - EPS && gx <= d.right() + EPS,
            Dir::Down => (d.y - from.bottom()).abs() < EPS + 1.0 && gx >= d.x - EPS && gx <= d.right() + EPS,
        })
        .next()
}

/// Which sides of `from` have any online neighbour (used to arm edge detection).
pub fn edges<'a, I>(online: I, from: &Device) -> [bool; 4]
where
    I: Iterator<Item = &'a Device> + Clone,
{
    let side = |dir: Dir| {
        online.clone().filter(|d| d.id != from.id).any(|d| match dir {
            Dir::Left => (d.right() - from.x).abs() < EPS + 1.0 && overlap(d.y, d.bottom(), from.y, from.bottom()) > EPS,
            Dir::Right => (d.x - from.right()).abs() < EPS + 1.0 && overlap(d.y, d.bottom(), from.y, from.bottom()) > EPS,
            Dir::Up => (d.bottom() - from.y).abs() < EPS + 1.0 && overlap(d.x, d.right(), from.x, from.right()) > EPS,
            Dir::Down => (d.y - from.bottom()).abs() < EPS + 1.0 && overlap(d.x, d.right(), from.x, from.right()) > EPS,
        })
    };
    [side(Dir::Left), side(Dir::Right), side(Dir::Up), side(Dir::Down)]
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::protocol::Platform;

    fn dev(id: &str, x: f64, y: f64, w: f64, h: f64) -> Device {
        Device { id: id.into(), name: id.into(), platform: Platform::Linux, w, h, x, y, version: String::new(), enabled: true }
    }

    #[test]
    fn windows_above_mac_is_reached_through_top_edge() {
        let mac = dev("mac", 0.0, 1000.0, 1470.0, 956.0);
        let win = dev("win", -100.0, 40.0, 1707.0, 960.0);
        let all = [mac.clone(), win.clone()];
        let n = neighbour(all.iter(), &mac, Dir::Up, 700.0, 1000.0).unwrap();
        assert_eq!(n.id, "win");
        assert!(neighbour(all.iter(), &mac, Dir::Left, 0.0, 1200.0).is_none());
        assert_eq!(edges(all.iter(), &mac), [false, false, true, false]);
    }

    #[test]
    fn snap_attaches_dragged_screen_to_nearest_side() {
        let mut map = BTreeMap::new();
        map.insert("mac".to_string(), dev("mac", 0.0, 0.0, 1470.0, 956.0));
        map.insert("win".to_string(), dev("win", 3000.0, 3000.0, 1707.0, 960.0));
        let (x, y) = snap(&map, "win", -100.0, -900.0);
        assert_eq!(y, -960.0);
        assert!((-1707.0 + MIN_OVERLAP..=1470.0 - MIN_OVERLAP).contains(&x));
    }
}
