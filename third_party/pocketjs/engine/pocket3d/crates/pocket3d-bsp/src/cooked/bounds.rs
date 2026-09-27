//! Bounds of the quantized vertices actually referenced by a cooked run.
use glam::Vec3;

use super::{CookedMap, FaceRun, VERTEX_STRIDE};
use crate::vis::Frustum;
/// Twelve bytes per run; constructed at load, without changing cooked files.
#[derive(Clone, Copy, Debug)]
pub struct RunBounds {
    mins: [i16; 3],
    maxs: [i16; 3],
}

impl RunBounds {
    pub fn new(map: &CookedMap<'_>, run: FaceRun) -> Self {
        if run.batch == u16::MAX {
            return Self::empty();
        }
        let batch = &map.batches[run.batch as usize];
        Self::from_indices(
            &map.verts[batch.vert_base as usize * VERTEX_STRIDE..],
            &map.indices
                [run.index_base as usize..run.index_base as usize + run.index_count as usize],
        )
    }

    fn empty() -> Self {
        Self {
            mins: [i16::MAX; 3],
            maxs: [i16::MIN; 3],
        }
    }

    fn from_indices(vertices: &[u8], indices: &[u16]) -> Self {
        let mut bounds = Self::empty();
        for &index in indices {
            let offset = index as usize * VERTEX_STRIDE + 12;
            for axis in 0..3 {
                let start = offset + axis * 2;
                let value = i16::from_le_bytes([vertices[start], vertices[start + 1]]);
                bounds.mins[axis] = bounds.mins[axis].min(value);
                bounds.maxs[axis] = bounds.maxs[axis].max(value);
            }
        }
        bounds
    }

    pub fn visible(&self, frustum: &Frustum) -> bool {
        self.mins[0] <= self.maxs[0]
            && frustum.intersects_aabb(
                Vec3::new(
                    self.mins[0] as f32,
                    self.mins[1] as f32,
                    self.mins[2] as f32,
                ),
                Vec3::new(
                    self.maxs[0] as f32,
                    self.maxs[1] as f32,
                    self.maxs[2] as f32,
                ),
            )
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use glam::Mat4;

    fn verts(points: &[[i16; 3]]) -> alloc::vec::Vec<u8> {
        let mut bytes = alloc::vec![0; points.len() * VERTEX_STRIDE];
        for (i, point) in points.iter().enumerate() {
            for (axis, value) in point.iter().enumerate() {
                bytes[i * VERTEX_STRIDE + 12 + axis * 2..i * VERTEX_STRIDE + 14 + axis * 2]
                    .copy_from_slice(&value.to_le_bytes());
            }
        }
        bytes
    }

    #[test]
    fn bounds_use_referenced_vertices_and_signed_quantized_coordinates() {
        let bytes = verts(&[[30000, 30000, 30000], [-3, -2, -10], [4, 5, -8], [0, 0, -9]]);
        let bounds = RunBounds::from_indices(&bytes, &[1, 2, 3, 2, 1, 3]);
        assert_eq!(bounds.mins, [-3, -2, -10]);
        assert_eq!(bounds.maxs, [4, 5, -8]);
        assert_eq!(core::mem::size_of::<RunBounds>(), 12);
    }

    #[test]
    fn conservative_frustum_keeps_crossing_faces_and_rejects_outside_faces() {
        for fov in [0.7, 1.4] {
            let frustum = Frustum::from_clip(Mat4::perspective_rh_gl(fov, 1.7, 1.0, 100.0), false);
            let inside = verts(&[[-1, -1, -10], [1, -1, -10], [0, 1, -10]]);
            let outside = verts(&[[200, -1, -10], [202, -1, -10], [201, 1, -10]]);
            let crossing = verts(&[[-300, -1, -10], [300, -1, -10], [0, 300, -10]]);
            assert!(RunBounds::from_indices(&inside, &[0, 1, 2]).visible(&frustum));
            assert!(!RunBounds::from_indices(&outside, &[0, 1, 2]).visible(&frustum));
            assert!(RunBounds::from_indices(&crossing, &[0, 1, 2]).visible(&frustum));
            assert!(!RunBounds::empty().visible(&frustum));
        }
    }
}
