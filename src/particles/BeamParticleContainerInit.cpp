#include "BeamParticleContainer.H"
#include "Constant.H"
#include "ParticleUtil.H"

using namespace amrex;

void
BeamParticleContainer::
InitParticles (const IntVect&  a_num_particles_per_cell,
               const Real      a_thermal_momentum_std,
               const Real      a_thermal_momentum_mean,
               const Real      a_density,
               const Geometry& a_geom,
               const RealBox&  a_bounds)
{
    BL_PROFILE("BeamParticleContainer::InitParticles");

    const int lev = 0;
    const auto dx = a_geom.CellSizeArray();
    const auto plo = a_geom.ProbLoArray();

    const int num_ppc = AMREX_D_TERM( a_num_particles_per_cell[0],
                                      *a_num_particles_per_cell[1],
                                      *a_num_particles_per_cell[2]);
    const Real scale_fac = dx[0]*dx[1]*dx[2]/num_ppc;

    for(MFIter mfi = MakeMFIter(lev); mfi.isValid(); ++mfi)
    {
        const Box& tile_box  = mfi.tilebox();

        const auto lo = amrex::lbound(tile_box);
        const auto hi = amrex::ubound(tile_box);

        Gpu::ManagedVector<unsigned int> counts(tile_box.numPts(), 0);
        unsigned int* pcount = counts.dataPtr();

        Gpu::ManagedVector<unsigned int> offsets(tile_box.numPts());
        unsigned int* poffset = offsets.dataPtr();

        amrex::ParallelFor(tile_box,
        [=] AMREX_GPU_DEVICE (int i, int j, int k) noexcept
        {
            for (int i_part=0; i_part<num_ppc;i_part++)
            {
                Real r[3];

                ParticleUtil::get_position_unit_cell(r, a_num_particles_per_cell, i_part);

                Real x = plo[0] + (i + r[0])*dx[0];
                Real y = plo[1] + (j + r[1])*dx[1];
                Real z = plo[2] + (k + r[2])*dx[2];

                if (x >= a_bounds.hi(0) || x < a_bounds.lo(0) ||
                    y >= a_bounds.hi(1) || y < a_bounds.lo(1) ||
                    z >= a_bounds.hi(2) || z < a_bounds.lo(2) ) continue;

                int ix = i - lo.x;
                int iy = j - lo.y;
                int iz = k - lo.z;
                int nx = hi.x-lo.x+1;
                int ny = hi.y-lo.y+1;
                int nz = hi.z-lo.z+1;
                unsigned int uix = amrex::min(nx-1,amrex::max(0,ix));
                unsigned int uiy = amrex::min(ny-1,amrex::max(0,iy));
                unsigned int uiz = amrex::min(nz-1,amrex::max(0,iz));
                unsigned int cellid = (uix * ny + uiy) * nz + uiz;
                pcount[cellid] += 1;
            }
        });

        Gpu::exclusive_scan(counts.begin(), counts.end(), offsets.begin());

        int num_to_add = offsets[tile_box.numPts()-1] + counts[tile_box.numPts()-1];

        auto& particles = GetParticles(lev);
        auto& particle_tile = particles[std::make_pair(mfi.index(), mfi.LocalTileIndex())];

        auto old_size = particle_tile.GetArrayOfStructs().size();
        auto new_size = old_size + num_to_add;
        particle_tile.resize(new_size);

        if (num_to_add == 0) continue;

        ParticleType* pstruct = particle_tile.GetArrayOfStructs()().data();

        auto arrdata = particle_tile.GetStructOfArrays().realarray();

        int procID = ParallelDescriptor::MyProc();
        int pid = ParticleType::NextID();
        ParticleType::NextID(pid + num_to_add);

        amrex::ParallelFor(tile_box,
        [=] AMREX_GPU_DEVICE (int i, int j, int k) noexcept
        {
            int ix = i - lo.x;
            int iy = j - lo.y;
            int iz = k - lo.z;
            int nx = hi.x-lo.x+1;
            int ny = hi.y-lo.y+1;
            int nz = hi.z-lo.z+1;
            unsigned int uix = amrex::min(nx-1,amrex::max(0,ix));
            unsigned int uiy = amrex::min(ny-1,amrex::max(0,iy));
            unsigned int uiz = amrex::min(nz-1,amrex::max(0,iz));
            unsigned int cellid = (uix * ny + uiy) * nz + uiz;

            int pidx = int(poffset[cellid] - poffset[0]);

            for (int i_part=0; i_part<num_ppc;i_part++)
            {
                Real r[3] = {0.,0.,0.};
                Real u[3] = {0.,0.,0.};

                ParticleUtil::get_position_unit_cell(r, a_num_particles_per_cell, i_part);

                Real x = plo[0] + (i + r[0])*dx[0];
                Real y = plo[1] + (j + r[1])*dx[1];
                Real z = plo[2] + (k + r[2])*dx[2];

                ParticleUtil::get_gaussian_random_momentum(u, a_thermal_momentum_mean,
                                                           a_thermal_momentum_std);

                if (x >= a_bounds.hi(0) || x < a_bounds.lo(0) ||
                    y >= a_bounds.hi(1) || y < a_bounds.lo(1) ||
                    z >= a_bounds.hi(2) || z < a_bounds.lo(2) ) continue;

                ParticleType& p = pstruct[pidx];
                p.id()   = pid + pidx;
                p.cpu()  = procID;
                p.pos(0) = x;
                p.pos(1) = y;
                p.pos(2) = z;

                arrdata[BeamIdx::ux  ][pidx] = u[0] * PhysConst::c;
                arrdata[BeamIdx::uy  ][pidx] = u[1] * PhysConst::c;
                arrdata[BeamIdx::uz  ][pidx] = u[2] * PhysConst::c;
                arrdata[BeamIdx::w   ][pidx] = a_density * scale_fac;
                ++pidx;
            }
        });
    }

    AMREX_ASSERT(OK());
}
