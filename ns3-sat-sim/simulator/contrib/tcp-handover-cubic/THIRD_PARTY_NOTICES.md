# Third-party notices

This file records the third-party material vendored or extended by the
`tcp-handover-cubic` ns-3 module. It supplements, and does not replace, the
license files in the repository and upstream projects.

## ML-CUBIC v2-A

- Upstream project: <https://github.com/aa22aa2244/ml-cubic-random-loss>
- Imported commit: `da86c587a9c8428862d3af17ec619714552ef720`
- Imported upstream path: `ns3-module/hybrid-cubic`
- Upstream license: GNU General Public License version 2 (GPL-2.0)

The imported implementation consists of `tcp-hybrid-cubic.{cc,h}` and its
embedded model-weight headers. The weight values are unchanged. This
repository carries a local copy-constructor correction so ns-3 socket cloning
preserves the `GrowthTraceEnabled` setting; see `ML-CUBIC.md` for maintenance
and parity-test details.

## ns-3 and Hypatia integration

The surrounding ns-3 module and its HARP-Cubic extensions are distributed
under GNU General Public License version 2, consistent with ns-3 and the
`ns3-sat-sim` component of Hypatia. See the source-file SPDX identifiers and
the repository's license files for the applicable terms.
