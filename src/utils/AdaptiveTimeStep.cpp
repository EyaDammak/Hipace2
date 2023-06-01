/* Copyright 2020-2021
 *
 * This file is part of HiPACE++.
 *
 * Authors: AlexanderSinn, MaxThevenet, Severin Diederichs
 * License: BSD-3-Clause-LBNL
 */
#include "AdaptiveTimeStep.H"
#include "utils/DeprecatedInput.H"
#include "particles/pusher/GetAndSetPosition.H"
#include "particles/particles_utils/FieldGather.H"
#include "Hipace.H"
#include "HipaceProfilerWrapper.H"
#include "Constants.H"

/** \brief describes which double is used for the adaptive time step */
struct WhichDouble {
    enum Comp { Dt=0, MinUz, MinAcc, SumWeights, SumWeightsTimesUz, SumWeightsTimesUzSquared, N };
};

AdaptiveTimeStep::AdaptiveTimeStep (const int nbeams)
{
    amrex::ParmParse ppa("hipace");
    std::string str_dt = "";
    queryWithParser(ppa, "dt", str_dt);
    if (str_dt == "adaptive"){
        m_do_adaptive_time_step = true;
        queryWithParser(ppa, "nt_per_betatron", m_nt_per_betatron);
        queryWithParser(ppa, "dt_max", m_dt_max);
        queryWithParser(ppa, "adaptive_threshold_uz", m_threshold_uz);
        queryWithParser(ppa, "adaptive_phase_tolerance", m_adaptive_phase_tolerance);
        queryWithParser(ppa, "adaptive_predict_step", m_adaptive_predict_step);
        queryWithParser(ppa, "adaptive_control_phase_advance", m_adaptive_control_phase_advance);
        queryWithParser(ppa, "adaptive_phase_substeps", m_adaptive_phase_substeps);
        queryWithParser(ppa, "adaptive_gather_ez", m_adaptive_gather_ez);
    }
    DeprecatedInput("hipace", "do_adaptive_time_step", "dt = adaptive");

    if (m_adaptive_gather_ez) {
        amrex::Print()<<"WARNING: hipace.adaptive_gather_ez = 1 is buggy and NOT recommended";
    }

    // create time step data container per beam
    for (int ibeam = 0; ibeam < nbeams; ibeam++) {
        amrex::Vector<amrex::Real> ts_data;
        ts_data.resize(6, 0.);
        ts_data[1] = 1e30; // max possible uz be taken into account
        m_timestep_data.emplace_back(ts_data);
    }

    m_nbeams = nbeams;
}

#ifdef AMREX_USE_MPI
void
AdaptiveTimeStep::BroadcastTimeStep (amrex::Real& dt, MPI_Comm a_comm_z, int a_numprocs_z)
{
    if (m_do_adaptive_time_step == 0) return;

    // Broadcast time step
    MPI_Bcast(&dt, 1, amrex::ParallelDescriptor::Mpi_typemap<amrex::Real>::type(),
              a_numprocs_z - 1, a_comm_z);

    // Broadcast minimum uz
    MPI_Bcast(&m_min_uz, 1, amrex::ParallelDescriptor::Mpi_typemap<amrex::Real>::type(),
              a_numprocs_z - 1, a_comm_z);
}

#endif

void
AdaptiveTimeStep::Calculate (
    amrex::Real t, amrex::Real& dt, MultiBeam& beams, MultiPlasma& plasmas,
    const int it, const bool initial)
{
    using namespace amrex::literals;

    if (m_do_adaptive_time_step == 0) return;

    HIPACE_PROFILE("AdaptiveTimeStep::Calculate()");

    const PhysConst phys_const = get_phys_const();
    const amrex::Real c = phys_const.c;
    const amrex::Real q_e = phys_const.q_e;
    const amrex::Real m_e = phys_const.m_e;
    const amrex::Real ep0 = phys_const.ep0;
    const amrex::Real clightinv = 1._rt/c;
    amrex::Real plasma_density = plasmas.maxDensity(c * t);
    AMREX_ALWAYS_ASSERT_WITH_MESSAGE( plasma_density > 0.,
        "A >0 plasma density must be specified to use an adaptive time step.");

    // Extract properties associated with physical size of the box
    const int nbeams = beams.get_nbeams();
    const int numprocs_z = Hipace::GetInstance().m_numprocs_z;

    amrex::Vector<amrex::Real> new_dts;
    new_dts.resize(nbeams);
    amrex::Vector<amrex::Real> beams_min_uz;
    beams_min_uz.resize(nbeams, std::numeric_limits<amrex::Real>::max());

    for (int ibeam = 0; ibeam < nbeams; ibeam++) {
        if (!Hipace::HeadRank() && initial) break;
        const auto& beam = beams.getBeam(ibeam);

        // first box resets time step data
        if (it == amrex::ParallelDescriptor::NProcs()-1) {
            m_timestep_data[ibeam][WhichDouble::SumWeights] = 0.;
            m_timestep_data[ibeam][WhichDouble::SumWeightsTimesUz] = 0.;
            m_timestep_data[ibeam][WhichDouble::SumWeightsTimesUzSquared] = 0.;
            m_timestep_data[ibeam][WhichDouble::MinUz] = 1e30;
        }

        const uint64_t box_offset = initial ? 0 : beam.m_box_sorter.boxOffsetsPtr()[beam.m_ibox];
        const uint64_t numParticleOnTile = initial ? beam.numParticles()
                                                   : beam.m_box_sorter.boxCountsPtr()[beam.m_ibox];


        // Extract particle properties
        const auto& soa = beam.GetStructOfArrays(); // For momenta and weights
        const auto uzp = soa.GetRealData(BeamIdx::uz).data() + box_offset;
        const auto wp = soa.GetRealData(BeamIdx::w).data() + box_offset;
        const auto idp = soa.GetIntData(BeamIdx::id).data() + box_offset;

        amrex::ReduceOps<amrex::ReduceOpSum, amrex::ReduceOpSum,
                         amrex::ReduceOpSum, amrex::ReduceOpMin> reduce_op;
        amrex::ReduceData<amrex::Real, amrex::Real, amrex::Real, amrex::Real>
                        reduce_data(reduce_op);
        using ReduceTuple = typename decltype(reduce_data)::Type;

        reduce_op.eval(numParticleOnTile, reduce_data,
            [=] AMREX_GPU_DEVICE (long ip) noexcept -> ReduceTuple
            {
                if (idp[ip] < 0) return {
                    0._rt, 0._rt, 0._rt, std::numeric_limits<amrex::Real>::infinity()
                };
                return {
                    wp[ip],
                    wp[ip] * uzp[ip] * clightinv,
                    wp[ip] * uzp[ip] * uzp[ip] * clightinv * clightinv,
                    uzp[ip] * clightinv
                };
            });

        auto res = reduce_data.value(reduce_op);
        m_timestep_data[ibeam][WhichDouble::SumWeights] += amrex::get<0>(res);
        m_timestep_data[ibeam][WhichDouble::SumWeightsTimesUz] += amrex::get<1>(res);
        m_timestep_data[ibeam][WhichDouble::SumWeightsTimesUzSquared] += amrex::get<2>(res);
        m_timestep_data[ibeam][WhichDouble::MinUz] =
            std::min(m_timestep_data[ibeam][WhichDouble::MinUz], amrex::get<3>(res));
    }

    // only the last box or at initialization the adaptive time step is calculated
    // from the full beam information
    if (it == 0 || initial)
    {
        for (int ibeam = 0; ibeam < nbeams; ibeam++) {

            if (Hipace::HeadRank() || !initial) {
                const auto& beam = beams.getBeam(ibeam);

                AMREX_ALWAYS_ASSERT_WITH_MESSAGE(
                    m_timestep_data[ibeam][WhichDouble::SumWeights] != 0,
                    "The sum of all weights is 0! Probably no beam particles are initialized\n");
                const amrex::Real mean_uz = m_timestep_data[ibeam][WhichDouble::SumWeightsTimesUz]
                    /m_timestep_data[ibeam][WhichDouble::SumWeights];
                const amrex::Real sigma_uz =
                    std::sqrt(std::abs(m_timestep_data[ibeam][WhichDouble::SumWeightsTimesUzSquared]
                                       /m_timestep_data[ibeam][WhichDouble::SumWeights]
                                       - mean_uz*mean_uz));
                const amrex::Real sigma_uz_dev = mean_uz - 4.*sigma_uz;
                const amrex::Real max_supported_uz = 1.e30;
                amrex::Real chosen_min_uz =
                    std::min(std::max(sigma_uz_dev, m_timestep_data[ibeam][WhichDouble::MinUz]),
                             max_supported_uz);

                beams_min_uz[ibeam] = chosen_min_uz*beam.m_mass*beam.m_mass/m_e/m_e;

                if (Hipace::m_verbose >=2 ){
                    amrex::Print()<<"Minimum gamma of beam " << ibeam <<
                        " to calculate new time step: " << beams_min_uz[ibeam] << "\n";
                }

                if (beams_min_uz[ibeam] < m_threshold_uz) {
                    amrex::Print()<<"WARNING: beam particles of beam "<< ibeam <<
                        " have non-relativistic velocities!\n";
                }
                beams_min_uz[ibeam] = std::max(beams_min_uz[ibeam], m_threshold_uz);
                new_dts[ibeam] = dt;

                /*
                  Calculate the time step for this beam used in the next time iteration
                  of the current rank, to resolve the betatron period with m_nt_per_betatron
                  points per period, assuming full blowout regime. The z-dependence of the
                  plasma profile is considered. As this time step will start in numprocs_z
                  time steps, so the beam may have propagated a significant distance.
                  If m_adaptive_predict_step is true, we estimate this distance and use it
                  for a more accurate time step estimation.
                */
                amrex::Real new_dt = dt;
                amrex::Real new_time = t;
                amrex::Real min_uz = beams_min_uz[ibeam];
                const int niter = m_adaptive_predict_step ? numprocs_z : 1;
                for (int i = 0; i < niter; i++)
                {
                    plasma_density = plasmas.maxDensity(c * new_time);
                    min_uz += m_timestep_data[ibeam][WhichDouble::MinAcc] * new_dt;
                    // Just make sure min_uz is >0, to avoid nans below.
                    min_uz = std::max(min_uz, 0.001_rt*m_threshold_uz);
                    const amrex::Real omega_p = std::sqrt(plasma_density * q_e * q_e / (ep0 * m_e));
                    amrex::Real omega_b = omega_p / std::sqrt(2. * min_uz);
                    new_dt = 2. * MathConst::pi / omega_b / m_nt_per_betatron;
                    new_time += new_dt;
                    if (min_uz > m_threshold_uz) new_dts[ibeam] = new_dt;
                }
            }
        }
        // Store min uz across beams, used in the phase advance method
        if (Hipace::HeadRank() || !initial) {
            m_min_uz = *std::min_element(beams_min_uz.begin(), beams_min_uz.end());
            /* set the new time step */
            dt = *std::min_element(new_dts.begin(), new_dts.end());
            // Make sure the new time step is smaller than the upper bound
            dt = std::min(dt, m_dt_max);
        }
    }
}

void
AdaptiveTimeStep::GatherMinAccSlice (MultiBeam& beams, const amrex::Geometry& geom,
                                     const Fields& fields, const int islice)
{
    using namespace amrex::literals;

    if (m_do_adaptive_time_step == 0) return;
    if (m_adaptive_gather_ez == 0) return;

    HIPACE_PROFILE("AdaptiveTimeStep::Calculate()");

    const PhysConst phys_const = get_phys_const();
    const amrex::Real clightinv = 1._rt/phys_const.c;

    constexpr int lev = 0;

    // Extract properties associated with physical size of the box
    const int nbeams = beams.get_nbeams();

    for (int ibeam = 0; ibeam < nbeams; ibeam++) {
        const auto& beam = beams.getBeam(ibeam);
        const amrex::Real charge_mass_ratio = beam.m_charge / beam.m_mass;

        BeamBins::index_type const * const indices = beam.m_slice_bins.permutationPtr();
        BeamBins::index_type const * const offsets = beam.m_slice_bins.offsetsPtrCpu();
        BeamBins::index_type const cell_start = offsets[islice];
        BeamBins::index_type const cell_stop  = offsets[islice+1];
        amrex::Long const num_particles = cell_stop-cell_start;
        amrex::Long const offset = beam.m_box_sorter.boxOffsetsPtr()[beam.m_ibox];

        amrex::ReduceOps<amrex::ReduceOpMin> reduce_op;
        amrex::ReduceData<amrex::Real> reduce_data(reduce_op);
        using ReduceTuple = typename decltype(reduce_data)::Type;

        // Data required to gather the Ez field
        const amrex::FArrayBox& slice_fab = fields.getSlices(lev)[0];
        Array3<const amrex::Real> const slice_arr = slice_fab.const_array();
        const int ez_comp = Comps[WhichSlice::This]["Ez"];
        const amrex::Real dx_inv = geom.InvCellSize(0);
        const amrex::Real dy_inv = geom.InvCellSize(1);
        amrex::Real const x_pos_offset = GetPosOffset(0, geom, slice_fab.box());
        const amrex::Real y_pos_offset = GetPosOffset(1, geom, slice_fab.box());

        auto const& soa = beam.GetStructOfArrays();
        const auto pos_x = soa.GetRealData(BeamIdx::x).data() + offset;
        const auto pos_y = soa.GetRealData(BeamIdx::y).data() + offset;
        const auto idp = soa.GetIntData(BeamIdx::id).data() + offset;

        reduce_op.eval(num_particles, reduce_data,
            [=] AMREX_GPU_DEVICE (long idx) noexcept -> ReduceTuple
            {
                const amrex::Long ip = indices[cell_start+idx];
                if (idp[ip] < 0) return { 0._rt };
                const amrex::Real xp = pos_x[ip];
                const amrex::Real yp = pos_y[ip];

                amrex::Real Ezp = 0._rt;
                doGatherEz(xp, yp, Ezp, slice_arr, ez_comp,
                           dx_inv, dy_inv, x_pos_offset, y_pos_offset);
                return {
                    charge_mass_ratio * Ezp * clightinv
                };
            });

        auto res = reduce_data.value(reduce_op);
        m_timestep_data[ibeam][WhichDouble::MinAcc] =
            std::min(m_timestep_data[ibeam][WhichDouble::MinAcc], amrex::get<0>(res));
    }
}

void
AdaptiveTimeStep::CalculateFromDensity (amrex::Real t, amrex::Real& dt, MultiPlasma& plasmas)
{
    using namespace amrex::literals;

    if (m_do_adaptive_time_step == 0) return;
    if (m_adaptive_control_phase_advance == 0) return;

    HIPACE_PROFILE("AdaptiveTimeStep::CalculateFromDensity()");

    const PhysConst pc = get_phys_const();

    amrex::Real dt_sub = dt / m_adaptive_phase_substeps;
    amrex::Real phase_advance = 0.;
    amrex::Real phase_advance0 = 0.;

    // Get plasma density at beginning of step
    const amrex::Real plasma_density0 = plasmas.maxDensity(pc.c * t);
    const amrex::Real omgp0 = std::sqrt(plasma_density0 * pc.q_e * pc.q_e / (pc.ep0 * pc.m_e));
    amrex::Real omgb0 = omgp0 / std::sqrt(2. *m_min_uz);

    // Numerically integrate the phase advance from t to t+dt. The time step is reduced such that
    // the expected phase advance equals that of a uniform plasma up to a tolerance level.
    for (int i = 0; i < m_adaptive_phase_substeps; i++)
    {
        const amrex::Real plasma_density = plasmas.maxDensity(pc.c * (t+i*dt_sub));
        const amrex::Real omgp = std::sqrt(plasma_density * pc.q_e * pc.q_e / (pc.ep0 * pc.m_e));
        amrex::Real omgb = omgp / std::sqrt(2. *m_min_uz);
        phase_advance += omgb * dt_sub;
        phase_advance0 += omgb0 * dt_sub;
        if(std::abs(phase_advance - phase_advance0) >
           2.*MathConst::pi*m_adaptive_phase_tolerance/m_nt_per_betatron)
        {
            if (i==0) amrex::AllPrint()<<"WARNING: adaptive time step exits at first substep."<<
                                         " Consider increasing hipace.adaptive_phase_substeps!\n";
            dt = i*dt_sub;
            return;
        }
    }

    for (int ibeam = 0; ibeam < m_nbeams; ibeam++) {
        m_timestep_data[ibeam][WhichDouble::MinAcc] = 0.;
    }
}
