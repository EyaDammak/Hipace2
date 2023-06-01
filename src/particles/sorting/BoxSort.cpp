/* Copyright 2021
 *
 * This file is part of HiPACE++.
 *
 * Authors: AlexanderSinn, MaxThevenet
 * License: BSD-3-Clause-LBNL
 */
#include "BoxSort.H"
#include "particles/beam/BeamParticleContainer.H"
#include "utils/HipaceProfilerWrapper.H"

#include <AMReX_ParticleTransformation.H>

void BoxSorter::sortParticlesByBox (BeamParticleContainer& a_beam,
                                    const amrex::BoxArray a_ba, const amrex::Geometry& a_geom)
{
    HIPACE_PROFILE("sortBeamParticlesByBox()");

    if (! m_particle_locator.isValid(a_ba)) m_particle_locator.build(a_ba, a_geom);
    auto assign_grid = m_particle_locator.getGridAssignor();

    int const np = a_beam.numParticles();
    BeamParticleContainer::ParticleTileDataType ptd = a_beam.getParticleTileData();

    int num_boxes = a_ba.size();
    m_box_counts.resize(num_boxes+1);
    m_box_counts.assign(num_boxes+1, 0);
    m_box_counts_cpu.resize(num_boxes+1);
    m_box_offsets.resize(num_boxes+1);
    m_box_offsets.assign(num_boxes+1, 0);
    m_box_offsets_cpu.resize(num_boxes+1);

    amrex::Gpu::DeviceVector<unsigned int> dst_indices(np);

    auto p_box_counts = m_box_counts.dataPtr();
    auto p_dst_indices = dst_indices.dataPtr();
    AMREX_FOR_1D ( np, i,
    {
        int dst_box = assign_grid(ptd[i]);
        if (ptd.id(i) < 0) dst_box = num_boxes; // if pid is invalid, remove particle
        if (dst_box < 0) {
            // particle has left domain transversely, stick it at the end and invalidate
            dst_box = num_boxes;
            ptd.id(i) = -std::abs(ptd.id(i));
        }
        unsigned int index = amrex::Gpu::Atomic::Add(
            &p_box_counts[dst_box], 1u);
        p_dst_indices[i] = index;
    });

    amrex::Gpu::exclusive_scan(m_box_counts.begin(), m_box_counts.end(), m_box_offsets.begin());

    BeamTile tmp{};
    tmp.resize(np);

    auto p_box_offsets = m_box_offsets.dataPtr();
    AMREX_FOR_1D ( np, i,
    {
        int dst_box = assign_grid(ptd[i]);
        if (ptd.id(i) < 0) dst_box = num_boxes; // if pid is invalid, remove particle
        if (dst_box < 0) dst_box = num_boxes;
        p_dst_indices[i] += p_box_offsets[dst_box];
    });

    amrex::scatterParticles<BeamTile>(tmp, a_beam, np, dst_indices.dataPtr());

    a_beam.swap(tmp);
#ifdef AMREX_USE_GPU
    amrex::Gpu::dtoh_memcpy_async(m_box_counts_cpu.dataPtr(), m_box_counts.dataPtr(),
                                  m_box_counts.size() * sizeof(index_type));

    amrex::Gpu::dtoh_memcpy_async(m_box_offsets_cpu.dataPtr(), m_box_offsets.dataPtr(),
                                  m_box_offsets.size() * sizeof(index_type));

    amrex::Gpu::streamSynchronize();
#else
    std::memcpy(m_box_counts_cpu.dataPtr(), m_box_counts.dataPtr(),
                m_box_counts.size() * sizeof(index_type));

    std::memcpy(m_box_offsets_cpu.dataPtr(), m_box_offsets.dataPtr(),
                m_box_offsets.size() * sizeof(index_type));
#endif
}

int
BoxSorter::leftmostBoxWithParticles () const
{
    int boxid = 0;
    while (m_box_counts_cpu[boxid]==0 && boxid<amrex::ParallelDescriptor::NProcs()-1){
        boxid++;
    }
    return boxid;
}
