/* Copyright 2020-2022
 *
 * This file is part of HiPACE++.
 *
 * Authors: AlexanderSinn, MaxThevenet, Severin Diederichs, WeiqunZhang
 * coulibaly-mouhamed
 * License: BSD-3-Clause-LBNL
 */
#include "Fields.H"
#include "fft_poisson_solver/FFTPoissonSolverPeriodic.H"
#include "fft_poisson_solver/FFTPoissonSolverDirichlet.H"
#include "Hipace.H"
#include "OpenBoundary.H"
#include "utils/HipaceProfilerWrapper.H"
#include "utils/Constants.H"
#include "utils/GPUUtil.H"
#include "particles/particles_utils/ShapeFactors.H"

using namespace amrex::literals;

amrex::IntVect Fields::m_slices_nguards = {-1, -1, -1};
amrex::IntVect Fields::m_poisson_nguards = {-1, -1, -1};

Fields::Fields (const int nlev)
    : m_slices(nlev)
{
    amrex::ParmParse ppf("fields");
    queryWithParser(ppf, "do_dirichlet_poisson", m_do_dirichlet_poisson);
    queryWithParser(ppf, "extended_solve", m_extended_solve);
    queryWithParser(ppf, "open_boundary", m_open_boundary);
}

void
Fields::AllocData (
    int lev, amrex::Geometry const& geom, const amrex::BoxArray& slice_ba,
    const amrex::DistributionMapping& slice_dm, int bin_size)
{
    HIPACE_PROFILE("Fields::AllocData()");
    AMREX_ALWAYS_ASSERT_WITH_MESSAGE(slice_ba.size() == 1,
        "Parallel field solvers not supported yet");

    if (lev==0) {

        if (m_extended_solve) {
            // Need 1 extra guard cell transversally for transverse derivative
            int nguards_xy = (Hipace::m_depos_order_xy + 1) / 2 + 1;
            m_slices_nguards = {nguards_xy, nguards_xy, 0};
            // poisson solver same size as fields
            m_poisson_nguards = m_slices_nguards;
            // one cell less for transverse derivative
            m_exmby_eypbx_nguard = m_slices_nguards - amrex::IntVect{1, 1, 0};
            // cut off anything near edge of charge/current deposition
            m_source_nguard = -m_slices_nguards;
        } else {
            // Need 1 extra guard cell transversally for transverse derivative
            int nguards_xy = (Hipace::m_depos_order_xy + 1) / 2 + 1;
            m_slices_nguards = {nguards_xy, nguards_xy, 0};
            // Poisson solver same size as domain, no ghost cells
            m_poisson_nguards = {0, 0, 0};
            m_exmby_eypbx_nguard = m_slices_nguards - amrex::IntVect{1, 1, 0};
            m_source_nguard = {0, 0, 0};
        }

        m_explicit = Hipace::GetInstance().m_explicit;
        m_any_neutral_background = Hipace::GetInstance().m_multi_plasma.AnySpeciesNeutralizeBackground();
        const bool any_salame = Hipace::GetInstance().m_multi_beam.AnySpeciesSalame();

        if (m_explicit) {
            // explicit solver:
            // beams share rho_beam jx_beam jy_beam jz_beam
            // rho jx jy jz for all plasmas and beams

            int isl = WhichSlice::Next;
            Comps[isl].multi_emplace(N_Comps, "jx_beam", "jy_beam");

            isl = WhichSlice::This;
            // (Bx, By), (Sy, Sx) and (chi, chi2) adjacent for explicit solver
            Comps[isl].multi_emplace(N_Comps, "chi");
            if (Hipace::m_use_amrex_mlmg) {
                Comps[isl].multi_emplace(N_Comps, "chi2");
            }
            Comps[isl].multi_emplace(N_Comps, "Sy", "Sx", "ExmBy", "EypBx", "Ez",
                "Bx", "By", "Bz", "Psi",
                "jx_beam", "jy_beam", "jz_beam", "rho_beam", "jx", "jy", "jz", "rho");

            isl = WhichSlice::Previous1;
            Comps[isl].multi_emplace(N_Comps, "jx_beam", "jy_beam");

            isl = WhichSlice::Previous2;
            // empty

            isl = WhichSlice::RhoIons;
            if (m_any_neutral_background) {
                Comps[isl].multi_emplace(N_Comps, "rho");
            }

            isl = WhichSlice::Salame;
            if (any_salame) {
                Comps[isl].multi_emplace(N_Comps, "Ez_target", "Ez_no_salame", "Ez",
                    "jx", "jy", "jz_beam", "Bx", "By", "Sy", "Sx", "Sy_back", "Sx_back");
            }

        } else {
            // predictor-corrector:
            // all beams and plasmas share rho jx jy jz

            int isl = WhichSlice::Next;
            Comps[isl].multi_emplace(N_Comps, "jx", "jy");

            isl = WhichSlice::This;
            // Bx and By adjacent for explicit solver
            Comps[isl].multi_emplace(N_Comps, "ExmBy", "EypBx", "Ez", "Bx", "By", "Bz", "Psi",
                                              "jx", "jy", "jz", "rho");

            if (Hipace::m_use_laser) {
                Comps[isl].multi_emplace(N_Comps, "chi");
            }

            isl = WhichSlice::Previous1;
            Comps[isl].multi_emplace(N_Comps, "Bx", "By", "jx", "jy");

            isl = WhichSlice::Previous2;
            Comps[isl].multi_emplace(N_Comps, "Bx", "By");

            isl = WhichSlice::RhoIons;
            if (m_any_neutral_background) {
                Comps[isl].multi_emplace(N_Comps, "rho");
            }

            isl = WhichSlice::Salame;
            // empty, not compatible
        }
    }

    // allocate memory for fields
    if (N_Comps != 0) {
        m_slices[lev].define(
            slice_ba, slice_dm, N_Comps, m_slices_nguards,
            amrex::MFInfo().SetArena(amrex::The_Arena()));
        m_slices[lev].setVal(0._rt, m_slices_nguards);
    }

    // The Poisson solver operates on transverse slices only.
    // The constructor takes the BoxArray and the DistributionMap of a slice,
    // so the FFTPlans are built on a slice.
    if (m_do_dirichlet_poisson){
        m_poisson_solver.push_back(std::unique_ptr<FFTPoissonSolverDirichlet>(
            new FFTPoissonSolverDirichlet(getSlices(lev).boxArray(),
                                          getSlices(lev).DistributionMap(),
                                          geom)) );
    } else {
        m_poisson_solver.push_back(std::unique_ptr<FFTPoissonSolverPeriodic>(
            new FFTPoissonSolverPeriodic(getSlices(lev).boxArray(),
                                         getSlices(lev).DistributionMap(),
                                         geom))  );
    }
    int num_threads = 1;
#ifdef AMREX_USE_OMP
#pragma omp parallel
    {
        num_threads = omp_get_num_threads();
    }
#endif
    if (Hipace::m_do_tiling) {
        const amrex::Box dom_box = slice_ba[0];
        const amrex::IntVect ncell = dom_box.bigEnd() - dom_box.smallEnd() + 1;
        AMREX_ALWAYS_ASSERT(ncell[0] % bin_size == 0 && ncell[1] % bin_size == 0);

        m_tmp_densities.resize(num_threads);
        for (int i=0; i<num_threads; i++){
            amrex::Box bx = {{0, 0, 0}, {bin_size-1, bin_size-1, 0}};
            bx.grow(m_slices_nguards);
            // jx jy jz rho chi
            m_tmp_densities[i].resize(bx, 5);
        }
    }
}

/** \brief inner version of derivative */
template<int dir>
struct derivative_inner {
    // captured variables for GPU
    Array2<amrex::Real const> array;
    amrex::Real dx_inv;

    // derivative of field in dir direction (x or y)
    AMREX_GPU_DEVICE amrex::Real operator() (int i, int j) const noexcept {
        constexpr bool is_x_dir = dir == Direction::x;
        constexpr bool is_y_dir = dir == Direction::y;
        return (array(i+is_x_dir,j+is_y_dir) - array(i-is_x_dir,j-is_y_dir)) * dx_inv;
    }
};

/** \brief inner version of derivative */
template<>
struct derivative_inner<Direction::z> {
    // captured variables for GPU
    Array2<amrex::Real const> array1;
    Array2<amrex::Real const> array2;
    amrex::Real dz_inv;

    // derivative of field in z direction
    AMREX_GPU_DEVICE amrex::Real operator() (int i, int j) const noexcept {
        return (array1(i,j) - array2(i,j)) * dz_inv;
    }
};

/** \brief derivative in x or y direction */
template<int dir>
struct derivative {
    // use brace initialization as constructor
    amrex::MultiFab f_view; // field to calculate its derivative
    const amrex::Geometry& geom; // geometry of field

    // use .array(mfi) like with amrex::MultiFab
    derivative_inner<dir> array (amrex::MFIter& mfi) const {
        return derivative_inner<dir>{f_view.array(mfi), 1._rt/(2._rt*geom.CellSize(dir))};
    }
};

/** \brief derivative in z direction. Use fields from previous and next slice */
template<>
struct derivative<Direction::z> {
    // use brace initialization as constructor
    amrex::MultiFab f_view1; // field on previous slice to calculate its derivative
    amrex::MultiFab f_view2; // field on next slice to calculate its derivative
    const amrex::Geometry& geom; // geometry of field

    // use .array(mfi) like with amrex::MultiFab
    derivative_inner<Direction::z> array (amrex::MFIter& mfi) const {
        return derivative_inner<Direction::z>{f_view1.array(mfi), f_view2.array(mfi),
            1._rt/(2._rt*geom.CellSize(Direction::z))};
    }
};

/** \brief inner version of interpolated_field_xy */
template<int interp_order_xy, class ArrayType>
struct interpolated_field_xy_inner {
    // captured variables for GPU
    ArrayType array;
    amrex::Real dx_inv;
    amrex::Real dy_inv;
    amrex::Real offset0;
    amrex::Real offset1;

    // interpolate field in x, y with interp_order_xy order transversely,
    // x and y must be inside field box
    template<class...Args> AMREX_GPU_DEVICE
    amrex::Real operator() (amrex::Real x, amrex::Real y, Args...args) const noexcept {

        // x direction
        const amrex::Real xmid = (x - offset0)*dx_inv;
        amrex::Real sx_cell[interp_order_xy + 1];
        const int i_cell = compute_shape_factor<interp_order_xy>(sx_cell, xmid);

        // y direction
        const amrex::Real ymid = (y - offset1)*dy_inv;
        amrex::Real sy_cell[interp_order_xy + 1];
        const int j_cell = compute_shape_factor<interp_order_xy>(sy_cell, ymid);

        amrex::Real field_value = 0._rt;
        for (int iy=0; iy<=interp_order_xy; iy++){
            for (int ix=0; ix<=interp_order_xy; ix++){
                field_value += sx_cell[ix] * sy_cell[iy] * array(i_cell+ix, j_cell+iy, args...);
            }
        }
        return field_value;
    }
};

/** \brief interpolate field in x, y with interp_order_xy order transversely,
 * x and y must be inside field box */
template<int interp_order_xy, class MfabType>
struct interpolated_field_xy {
    // use brace initialization as constructor
    MfabType mfab; // MultiFab type object of the field
    amrex::Geometry geom; // geometry of field

    // use .array(mfi) like with amrex::MultiFab
    auto array (amrex::MFIter& mfi) const {
        auto mfab_array = to_array2(mfab.array(mfi));
        return interpolated_field_xy_inner<interp_order_xy, decltype(mfab_array)>{
            mfab_array, 1._rt/geom.CellSize(0), 1._rt/geom.CellSize(1),
            GetPosOffset(0, geom, geom.Domain()), GetPosOffset(1, geom, geom.Domain())};
    }
};

/** \brief inner version of guarded_field_xy */
struct guarded_field_xy_inner {
    // captured variables for GPU
    Array3<amrex::Real const> array;
    int lox;
    int hix;
    int loy;
    int hiy;

    AMREX_GPU_DEVICE amrex::Real operator() (int i, int j, int n) const noexcept {
        if (lox <= i && i <= hix && loy <= j && j <= hiy) {
            return array(i,j,n);
        } else return 0._rt;
    }
};

/** \brief if indices are outside of the fields box zero is returned */
struct guarded_field_xy {
    // use brace initialization as constructor
    amrex::MultiFab& mfab; // field to be guarded (zero extended)

    // use .array(mfi) like with amrex::MultiFab
    guarded_field_xy_inner array (amrex::MFIter& mfi) const {
        const amrex::Box bx = mfab[mfi].box();
        return guarded_field_xy_inner{mfab.const_array(mfi), bx.smallEnd(Direction::x),
            bx.bigEnd(Direction::x), bx.smallEnd(Direction::y), bx.bigEnd(Direction::y)};
    }
};

/** \brief Calculates dst = factor_a*src_a + factor_b*src_b. src_a and src_b can be derivatives
 *
 * \param[in] box_grow how much the domain of dst should be grown
 * \param[in] dst destination
 * \param[in] factor_a factor before src_a
 * \param[in] src_a first source
 * \param[in] factor_b factor before src_b
 * \param[in] src_b second source
 * \param[in] do_add whether to overwrite (=) or to add (+=) dst.
 */
template<class FVA, class FVB>
void
LinCombination (const amrex::IntVect box_grow, amrex::MultiFab dst,
                const amrex::Real factor_a, const FVA& src_a,
                const amrex::Real factor_b, const FVB& src_b, const bool do_add=false)
{
    HIPACE_PROFILE("Fields::LinCombination()");

#ifdef AMREX_USE_OMP
#pragma omp parallel if (amrex::Gpu::notInLaunchRegion())
#endif
    for ( amrex::MFIter mfi(dst, DfltMfiTlng); mfi.isValid(); ++mfi ){
        const Array2<amrex::Real> dst_array = dst.array(mfi);
        const auto src_a_array = to_array2(src_a.array(mfi));
        const auto src_b_array = to_array2(src_b.array(mfi));
        const amrex::Box bx = mfi.growntilebox(box_grow);
        const int box_i_lo = bx.smallEnd(Direction::x);
        const int box_j_lo = bx.smallEnd(Direction::y);
        const int box_i_hi = bx.bigEnd(Direction::x);
        const int box_j_hi = bx.bigEnd(Direction::y);
        amrex::ParallelFor(mfi.growntilebox(),
            [=] AMREX_GPU_DEVICE(int i, int j, int) noexcept
            {
                const bool inside = box_i_lo<=i && i<=box_i_hi && box_j_lo<=j && j<=box_j_hi;
                const amrex::Real tmp =
                    inside ? factor_a * src_a_array(i,j) + factor_b * src_b_array(i,j) : 0._rt;
                if (do_add) {
                    dst_array(i,j) += tmp;
                } else {
                    dst_array(i,j) = tmp;
                }
            });
    }
}

void
Fields::Copy (const int lev, const int i_slice, const amrex::Geometry& diag_geom,
              amrex::FArrayBox& diag_fab, amrex::Box diag_box, const amrex::Geometry& calc_geom,
              const amrex::Gpu::DeviceVector<int>& diag_comps_vect, const int ncomp,
              bool do_laser, MultiLaser& multi_laser)
{
    HIPACE_PROFILE("Fields::Copy()");
    constexpr int depos_order_xy = 1;
    constexpr int depos_order_z = 1;
    constexpr int depos_order_offset = depos_order_z / 2 + 1;

    const amrex::Real poff_calc_z = GetPosOffset(2, calc_geom, calc_geom.Domain());
    const amrex::Real poff_diag_x = GetPosOffset(0, diag_geom, diag_geom.Domain());
    const amrex::Real poff_diag_y = GetPosOffset(1, diag_geom, diag_geom.Domain());
    const amrex::Real poff_diag_z = GetPosOffset(2, diag_geom, diag_geom.Domain());

    // Interpolation in z Direction, done as if looped over diag_fab not i_slice
    // Calculate to which diag_fab slices this slice could contribute
    const int i_slice_min = i_slice - depos_order_offset;
    const int i_slice_max = i_slice + depos_order_offset;
    const amrex::Real pos_slice_min = i_slice_min * calc_geom.CellSize(2) + poff_calc_z;
    const amrex::Real pos_slice_max = i_slice_max * calc_geom.CellSize(2) + poff_calc_z;
    const int k_min = static_cast<int>(amrex::Math::round((pos_slice_min - poff_diag_z)
                                                          / diag_geom.CellSize(2)));
    const int k_max = static_cast<int>(amrex::Math::round((pos_slice_max - poff_diag_z)
                                                          / diag_geom.CellSize(2)));

    // Put contributions from i_slice to different diag_fab slices in GPU vector
    m_rel_z_vec.resize(k_max+1-k_min);
    m_rel_z_vec_cpu.resize(k_max+1-k_min);
    for (int k=k_min; k<=k_max; ++k) {
        const amrex::Real pos = k * diag_geom.CellSize(2) + poff_diag_z;
        const amrex::Real mid_i_slice = (pos - poff_calc_z)/calc_geom.CellSize(2);
        amrex::Real sz_cell[depos_order_z + 1];
        const int k_cell = compute_shape_factor<depos_order_z>(sz_cell, mid_i_slice);
        m_rel_z_vec_cpu[k-k_min] = 0;
        for (int i=0; i<=depos_order_z; ++i) {
            if (k_cell+i == i_slice) {
                m_rel_z_vec_cpu[k-k_min] = sz_cell[i];
            }
        }
    }

    // Optimization: don’t loop over diag_fab slices with 0 contribution
    int k_start = k_min;
    int k_stop = k_max;
    for (int k=k_min; k<=k_max; ++k) {
        if (m_rel_z_vec_cpu[k-k_min] == 0) ++k_start;
        else break;
    }
    for (int k=k_max; k>=k_min; --k) {
        if (m_rel_z_vec_cpu[k-k_min] == 0) --k_stop;
        else break;
    }
    diag_box.setSmall(2, amrex::max(diag_box.smallEnd(2), k_start));
    diag_box.setBig(2, amrex::min(diag_box.bigEnd(2), k_stop));
    if (diag_box.isEmpty()) return;
    auto& slice_mf = m_slices[lev];
    auto slice_func = interpolated_field_xy<depos_order_xy, guarded_field_xy>{{slice_mf}, calc_geom};
    auto& laser_mf = multi_laser.getSlices();
    auto laser_func = interpolated_field_xy<depos_order_xy, guarded_field_xy>{{laser_mf}, calc_geom};

#ifdef AMREX_USE_GPU
    // This async copy happens on the same stream as the ParallelFor below, which uses the copied array.
    // Therefore, it is safe to do it async.
    amrex::Gpu::htod_memcpy_async(m_rel_z_vec.dataPtr(), m_rel_z_vec_cpu.dataPtr(),
                                  m_rel_z_vec_cpu.size() * sizeof(amrex::Real));
#else
    std::memcpy(m_rel_z_vec.dataPtr(), m_rel_z_vec_cpu.dataPtr(),
                m_rel_z_vec_cpu.size() * sizeof(amrex::Real));
#endif

    // Finally actual kernel: Interpolation in x, y, z of zero-extended fields
    for (amrex::MFIter mfi(slice_mf, DfltMfi); mfi.isValid(); ++mfi) {
        auto slice_array = slice_func.array(mfi);
        amrex::Array4<amrex::Real> diag_array = diag_fab.array();

        const int *diag_comps = diag_comps_vect.data();
        const amrex::Real *rel_z_data = m_rel_z_vec.data();
        const amrex::Real dx = diag_geom.CellSize(0);
        const amrex::Real dy = diag_geom.CellSize(1);

        amrex::ParallelFor(diag_box, ncomp,
            [=] AMREX_GPU_DEVICE(int i, int j, int k, int n) noexcept
            {
                const amrex::Real x = i * dx + poff_diag_x;
                const amrex::Real y = j * dy + poff_diag_y;
                const int m = n[diag_comps];
                diag_array(i,j,k,n) += rel_z_data[k-k_min] * slice_array(x,y,m);
            });

        if (!do_laser) continue;
        auto laser_array = laser_func.array(mfi);
        amrex::ParallelFor(diag_box,
            [=] AMREX_GPU_DEVICE(int i, int j, int k) noexcept
            {
                const amrex::Real x = i * dx + poff_diag_x;
                const amrex::Real y = j * dy + poff_diag_y;
                diag_array(i,j,k,ncomp) += rel_z_data[k-k_min] *
                    laser_array(x,y,WhichLaserSlice::n00j00_r);
                diag_array(i,j,k,ncomp+1) += rel_z_data[k-k_min] *
                    laser_array(x,y,WhichLaserSlice::n00j00_i);
            });
    }
}

void
Fields::ShiftSlices (int lev)
{
    HIPACE_PROFILE("Fields::ShiftSlices()");

    const bool explicit_solve = Hipace::GetInstance().m_explicit;

    // only shift the slices that are allocated
    if (explicit_solve) {
        shift(lev, WhichSlice::Previous1, WhichSlice::This, "jx_beam", "jy_beam");
        duplicate(lev, WhichSlice::This, {"jx_beam", "jy_beam", "jx"     , "jy"     },
                       WhichSlice::Next, {"jx_beam", "jy_beam", "jx_beam", "jy_beam"});
    } else {
        shift(lev, WhichSlice::Previous2, WhichSlice::Previous1, "Bx", "By");
        shift(lev, WhichSlice::Previous1, WhichSlice::This, "Bx", "By", "jx", "jy");
    }
}

void
Fields::AddRhoIons (const int lev)
{
    if (!m_any_neutral_background) return;
    HIPACE_PROFILE("Fields::AddRhoIons()");
    add(lev, WhichSlice::This, {"rho"}, WhichSlice::RhoIons, {"rho"});
}

/** \brief Sets non zero Dirichlet Boundary conditions in RHS which is the source of the Poisson
 * equation: laplace LHS = RHS
 *
 * \param[in] RHS source of the Poisson equation: laplace LHS = RHS
 * \param[in] solver_size size of RHS/poisson solver (no tiling)
 * \param[in] geom geometry of of RHS/poisson solver
 * \param[in] offset shift boundary value by offset number of cells
 * \param[in] factor multiply the boundary_value by this factor
 * \param[in] boundary_value functional object (Real x, Real y) -> Real value_of_potential
 */
template<class Functional>
void
SetDirichletBoundaries (Array2<amrex::Real> RHS, const amrex::Box& solver_size,
                        const amrex::Geometry& geom, const amrex::Real offset,
                        const amrex::Real factor, const Functional& boundary_value)
{
    // To solve a Poisson equation with non-zero Dirichlet boundary conditions, the source term
    // must be corrected at the outmost grid points in x by -field_value_at_guard_cell / dx^2 and
    // in y by -field_value_at_guard_cell / dy^2, where dx and dy are those of the fine grid
    // This follows Van Loan, C. (1992). Computational frameworks for the fast Fourier transform.
    // Page 254 ff.
    // The interpolation is done in second order transversely and linearly in longitudinal direction
    const int box_len0 = solver_size.length(0);
    const int box_len1 = solver_size.length(1);
    const int box_lo0 = solver_size.smallEnd(0);
    const int box_lo1 = solver_size.smallEnd(1);
    const amrex::Real dx = geom.CellSize(0);
    const amrex::Real dy = geom.CellSize(1);
    const amrex::Real offset0 = GetPosOffset(0, geom, solver_size);
    const amrex::Real offset1 = GetPosOffset(1, geom, solver_size);

    const amrex::Box edge_box = {{0, 0, 0}, {box_len0 + box_len1 - 1, 1, 0}};

    // ParallelFor only over the edge of the box
    amrex::ParallelFor(edge_box,
        [=] AMREX_GPU_DEVICE (int i, int j, int) noexcept
        {
            const bool i_is_changing = (i < box_len0);
            const bool i_lo_edge = (!i_is_changing) && (!j);
            const bool i_hi_edge = (!i_is_changing) && j;
            const bool j_lo_edge = i_is_changing && (!j);
            const bool j_hi_edge = i_is_changing && j;

            const int i_idx = box_lo0 + i_hi_edge*(box_len0-1) + i_is_changing*i;
            const int j_idx = box_lo1 + j_hi_edge*(box_len1-1) + (!i_is_changing)*(i-box_len0);

            const amrex::Real i_idx_offset = i_idx + (- i_lo_edge + i_hi_edge) * offset;
            const amrex::Real j_idx_offset = j_idx + (- j_lo_edge + j_hi_edge) * offset;

            const amrex::Real x = i_idx_offset * dx + offset0;
            const amrex::Real y = j_idx_offset * dy + offset1;

            const amrex::Real dxdx = dx*dx*(!i_is_changing) + dy*dy*i_is_changing;

            // atomic add because the corners of RHS get two values
            amrex::Gpu::Atomic::AddNoRet(&(RHS(i_idx, j_idx)),
                                         - boundary_value(x, y) * factor / dxdx);
        });
}

void
Fields::SetBoundaryCondition (amrex::Vector<amrex::Geometry> const& geom, const int lev,
                              std::string component, amrex::MultiFab&& staging_area)
{
    HIPACE_PROFILE("Fields::SetBoundaryCondition()");
    if (lev == 0 && m_open_boundary) {
        // Coarsest level: use Taylor expansion of the Green's function
        // to get Dirichlet boundary conditions

        AMREX_ALWAYS_ASSERT_WITH_MESSAGE(staging_area.size() == 1,
            "Open Boundaries only work for lev0 with everything in one box");
        amrex::FArrayBox& staging_area_fab = staging_area[0];

        const Array2<amrex::Real> arr_staging_area = staging_area_fab.array();
        const amrex::Box staging_box = staging_area_fab.box();

        const amrex::Real poff_x = GetPosOffset(0, geom[lev], staging_box);
        const amrex::Real poff_y = GetPosOffset(1, geom[lev], staging_box);
        const amrex::Real dx = geom[lev].CellSize(0);
        const amrex::Real dy = geom[lev].CellSize(1);
        // scale factor cancels out for all multipole coefficients except the 0th, for wich it adds
        // a constant term to the potential
        const amrex::Real scale = 3._rt/std::sqrt(
            pow<2>(geom[lev].ProbLength(0)) + pow<2>(geom[lev].ProbLength(1)));
        const amrex::Real radius = amrex::min(
            std::abs(geom[lev].ProbLo(0)), std::abs(geom[lev].ProbHi(0)),
            std::abs(geom[lev].ProbLo(1)), std::abs(geom[lev].ProbHi(1)));
        AMREX_ALWAYS_ASSERT_WITH_MESSAGE(radius > 0._rt, "The x=0, y=0 coordinate must be inside"
            "the simulation box as it is used as the point of expansion for open boundaries");
        // ignore everything outside of 95% the min radius as the Taylor expansion only converges
        // outside of a circular patch containing the sources, i.e. the sources can't be further
        // from the center than the closest boundary as it would be the case in the corners
        const amrex::Real cutoff_sq = pow<2>(0.95_rt * radius * scale);
        const amrex::Real dxdy_div_4pi = dx*dy/(4._rt * MathConst::pi);

        MultipoleTuple coeff_tuple =
        amrex::ParReduce(MultipoleReduceOpList{}, MultipoleReduceTypeList{},
                         staging_area, m_source_nguard,
            [=] AMREX_GPU_DEVICE (int /*box_num*/, int i, int j, int) noexcept
            {
                const amrex::Real x = (i * dx + poff_x) * scale;
                const amrex::Real y = (j * dy + poff_y) * scale;
                if (x*x + y*y > cutoff_sq)  {
                    return MultipoleTuple{0._rt,
                        0._rt, 0._rt, 0._rt, 0._rt, 0._rt, 0._rt, 0._rt, 0._rt, 0._rt,
                        0._rt, 0._rt, 0._rt, 0._rt, 0._rt, 0._rt, 0._rt, 0._rt, 0._rt,
                        0._rt, 0._rt, 0._rt, 0._rt, 0._rt, 0._rt, 0._rt, 0._rt, 0._rt,
                        0._rt, 0._rt, 0._rt, 0._rt, 0._rt, 0._rt, 0._rt, 0._rt, 0._rt
                    };
                }
                amrex::Real s_v = arr_staging_area(i, j);
                return GetMultipoleCoeffs(s_v, x, y);
            }
        );

        if (component == "Ez" || component == "Bz") {
            // Because Ez and Bz only have transverse derivatives of currents as sources, the
            // integral over the whole box is zero, meaning they have no physical monopole component
            amrex::get<0>(coeff_tuple) = 0._rt;
        }

        SetDirichletBoundaries(arr_staging_area, staging_box, geom[lev], 1, 1,
            [=] AMREX_GPU_DEVICE (amrex::Real x, amrex::Real y) noexcept
            {
                return dxdy_div_4pi*GetFieldMultipole(coeff_tuple, x*scale, y*scale);
            }
        );

    } else if (lev == 1) {
        // Fine level: interpolate solution from coarser level to get Dirichlet boundary conditions
        constexpr int interp_order = 2;

        auto solution_interp = interpolated_field_xy<interp_order, amrex::MultiFab>{
            getField(lev-1, WhichSlice::This, component), geom[lev-1]};

        for (amrex::MFIter mfi(staging_area, DfltMfi); mfi.isValid(); ++mfi)
        {
            const auto arr_solution_interp = solution_interp.array(mfi);
            const Array2<amrex::Real> arr_staging_area = staging_area.array(mfi);
            const amrex::Box fine_staging_box = getStagingArea(lev)[mfi].box();

            amrex::Real offset = 1;
            amrex::Real factor = 1;
            if ((component == "Bx" || component == "By") && Hipace::GetInstance().m_explicit &&
                (getSlices(lev).box(0).length(0) % 2 == 0)) {
                // hpmg has the boundary condition at a different place
                // compared to the fft poisson solver
                offset = 0.5;
                factor = 8./3.;
            }

            SetDirichletBoundaries(arr_staging_area, fine_staging_box, geom[lev],
                                   offset, factor, arr_solution_interp);
        }
    }
}


void
Fields::InterpolateFromLev0toLev1 (amrex::Vector<amrex::Geometry> const& geom, const int lev,
                                   std::string component, const amrex::IntVect outer_edge,
                                   const amrex::IntVect inner_edge)
{
    if (lev == 0) return; // only interpolate boundaries to lev 1
    if (outer_edge == inner_edge) return;
    HIPACE_PROFILE("Fields::InterpolateFromLev0toLev1()");
    constexpr int interp_order = 2;

    auto field_coarse_interp = interpolated_field_xy<interp_order, amrex::MultiFab>{
        getField(lev-1, WhichSlice::This, component), geom[lev-1]};
    amrex::MultiFab field_fine = getField(lev, WhichSlice::This, component);

    for (amrex::MFIter mfi( field_fine, DfltMfi); mfi.isValid(); ++mfi)
    {
        auto arr_field_coarse_interp = field_coarse_interp.array(mfi);
        const Array2<amrex::Real> arr_field_fine = field_fine.array(mfi);
        const amrex::Box fine_box_extended = mfi.growntilebox(outer_edge);
        const amrex::Box fine_box_narrow = mfi.growntilebox(inner_edge);

        const int narrow_i_lo = fine_box_narrow.smallEnd(0);
        const int narrow_i_hi = fine_box_narrow.bigEnd(0);
        const int narrow_j_lo = fine_box_narrow.smallEnd(1);
        const int narrow_j_hi = fine_box_narrow.bigEnd(1);

        const amrex::Real dx = geom[lev].CellSize(0);
        const amrex::Real dy = geom[lev].CellSize(1);
        const amrex::Real offset0 = GetPosOffset(0, geom[lev], fine_box_extended);
        const amrex::Real offset1 = GetPosOffset(1, geom[lev], fine_box_extended);

        amrex::ParallelFor(fine_box_extended,
            [=] AMREX_GPU_DEVICE (int i, int j , int) noexcept
            {
                // set interpolated values near edge of fine field between outer_edge and inner_edge
                // to compensate for incomplete charge/current deposition in those cells
                if(i<narrow_i_lo || i>narrow_i_hi || j<narrow_j_lo || j>narrow_j_hi) {
                    amrex::Real x = i * dx + offset0;
                    amrex::Real y = j * dy + offset1;
                    arr_field_fine(i,j) = arr_field_coarse_interp(x,y);
                }
            });
    }
}


void
Fields::LevelUp (amrex::Vector<amrex::Geometry> const& geom, const int lev,
                 const int which_slice, const std::string component)
{
    if (lev == 0) return; // only interpolate field to lev 1
    HIPACE_PROFILE("Fields::LevelUp()");
    constexpr int interp_order = 2;

    auto field_coarse_interp = interpolated_field_xy<interp_order, amrex::MultiFab>{
        getField(lev-1, which_slice, component), geom[lev-1]};
    amrex::MultiFab field_fine = getField(lev, which_slice, component);

    for (amrex::MFIter mfi( field_fine, DfltMfi); mfi.isValid(); ++mfi)
    {
        auto arr_field_coarse_interp = field_coarse_interp.array(mfi);
        const Array2<amrex::Real> arr_field_fine = field_fine.array(mfi);

        const amrex::Real dx = geom[lev].CellSize(0);
        const amrex::Real dy = geom[lev].CellSize(1);
        const amrex::Real offset0 = GetPosOffset(0, geom[lev], geom[lev].Domain());
        const amrex::Real offset1 = GetPosOffset(1, geom[lev], geom[lev].Domain());

        amrex::ParallelFor(field_fine[mfi].box(),
            [=] AMREX_GPU_DEVICE (int i, int j , int) noexcept
            {
                // interpolate the full field
                const amrex::Real x = i * dx + offset0;
                const amrex::Real y = j * dy + offset1;
                arr_field_fine(i,j) = arr_field_coarse_interp(x,y);
            });
    }
}


void
Fields::SolvePoissonExmByAndEypBx (amrex::Vector<amrex::Geometry> const& geom, const int lev)
{
    /* Solves Laplacian(Psi) =  1/epsilon0 * -(rho-Jz/c) and
     * calculates Ex-c By, Ey + c Bx from  grad(-Psi)
     */
    HIPACE_PROFILE("Fields::SolveExmByAndEypBx()");

    PhysConst phys_const = get_phys_const();

    // Left-Hand Side for Poisson equation is Psi in the slice MF
    amrex::MultiFab lhs(getSlices(lev), amrex::make_alias, Comps[WhichSlice::This]["Psi"], 1);

    // interpolate rho and jz to level 1 in the domain edges
    InterpolateFromLev0toLev1(geom, lev, "rho", m_poisson_nguards, -m_slices_nguards);
    InterpolateFromLev0toLev1(geom, lev, "jz", m_poisson_nguards, -m_slices_nguards);

    // calculating the right-hand side 1/episilon0 * -(rho-Jz/c)
    LinCombination(m_source_nguard, getStagingArea(lev),
        1._rt/(phys_const.c*phys_const.ep0), getField(lev, WhichSlice::This, "jz"),
        -1._rt/(phys_const.ep0), getField(lev, WhichSlice::This, "rho"), false);

    if (Hipace::m_do_beam_jz_minus_rho) {
        // interpolate rho_beam and jz_beam to level 1 in the domain edges
        InterpolateFromLev0toLev1(geom, lev, "rho_beam", m_poisson_nguards, -m_slices_nguards);
        InterpolateFromLev0toLev1(geom, lev, "jz_beam", m_poisson_nguards, -m_slices_nguards);

        LinCombination(m_source_nguard, getStagingArea(lev),
            1._rt/(phys_const.c*phys_const.ep0), getField(lev, WhichSlice::This, "jz_beam"),
            -1._rt/(phys_const.ep0), getField(lev, WhichSlice::This, "rho_beam"), true);
    }

    SetBoundaryCondition(geom, lev, "Psi", getStagingArea(lev));
    m_poisson_solver[lev]->SolvePoissonEquation(lhs);

    if (!m_extended_solve) {
        /* ---------- Transverse FillBoundary Psi ---------- */
        lhs.FillBoundary(geom[lev].periodicity());
    }

    // interpolate psi to level 1 in the ghost cells
    InterpolateFromLev0toLev1(geom, lev, "Psi", m_slices_nguards, m_poisson_nguards);

    // Compute ExmBy = -d/dx psi and EypBx = -d/dy psi
    amrex::MultiFab f_ExmBy = getField(lev, WhichSlice::This, "ExmBy");
    amrex::MultiFab f_EypBx = getField(lev, WhichSlice::This, "EypBx");
    amrex::MultiFab f_Psi = getField(lev, WhichSlice::This, "Psi");

#ifdef AMREX_USE_OMP
#pragma omp parallel if (amrex::Gpu::notInLaunchRegion())
#endif
    for ( amrex::MFIter mfi(f_ExmBy, DfltMfiTlng); mfi.isValid(); ++mfi ){
        const Array2<amrex::Real> array_ExmBy = f_ExmBy.array(mfi);
        const Array2<amrex::Real> array_EypBx = f_EypBx.array(mfi);
        const Array2<amrex::Real const> array_Psi = f_Psi.const_array(mfi);
        // number of ghost cells where ExmBy and EypBx are calculated is 0 for now
        const amrex::Box bx = mfi.growntilebox(m_exmby_eypbx_nguard);
        const amrex::Real dx_inv = 1._rt/(2._rt*geom[lev].CellSize(Direction::x));
        const amrex::Real dy_inv = 1._rt/(2._rt*geom[lev].CellSize(Direction::y));

        amrex::ParallelFor(bx,
            [=] AMREX_GPU_DEVICE(int i, int j, int)
            {
                // derivatives in x and y direction, no guards needed
                array_ExmBy(i,j) = - (array_Psi(i+1,j) - array_Psi(i-1,j))*dx_inv;
                array_EypBx(i,j) = - (array_Psi(i,j+1) - array_Psi(i,j-1))*dy_inv;
            });
    }
}


void
Fields::SolvePoissonEz (amrex::Vector<amrex::Geometry> const& geom, const int lev,
                        const int which_slice)
{
    /* Solves Laplacian(Ez) =  1/(episilon0 *c0 )*(d_x(jx) + d_y(jy)) */
    HIPACE_PROFILE("Fields::SolvePoissonEz()");

    PhysConst phys_const = get_phys_const();
    // Left-Hand Side for Poisson equation is Bz in the slice MF
    amrex::MultiFab lhs(getSlices(lev), amrex::make_alias, Comps[which_slice]["Ez"], 1);

    // Right-Hand Side for Poisson equation: compute 1/(episilon0 *c0 )*(d_x(jx) + d_y(jy))
    // from the slice MF, and store in the staging area of poisson_solver
    LinCombination(m_source_nguard, getStagingArea(lev),
        1._rt/(phys_const.ep0*phys_const.c),
        derivative<Direction::x>{getField(lev, which_slice, "jx"), geom[lev]},
        1._rt/(phys_const.ep0*phys_const.c),
        derivative<Direction::y>{getField(lev, which_slice, "jy"), geom[lev]});

    SetBoundaryCondition(geom, lev, "Ez", getStagingArea(lev));
    // Solve Poisson equation.
    // The RHS is in the staging area of poisson_solver.
    // The LHS will be returned as lhs.
    m_poisson_solver[lev]->SolvePoissonEquation(lhs);

    // interpolate Ez to level 1 in the ghost cells
    InterpolateFromLev0toLev1(geom, lev, "Ez", m_slices_nguards, m_poisson_nguards);
}

void
Fields::SolvePoissonBx (amrex::MultiFab& Bx_iter, amrex::Vector<amrex::Geometry> const& geom,
                        const int lev)
{
    /* Solves Laplacian(Bx) = mu_0*(- d_y(jz) + d_z(jy) ) */
    HIPACE_PROFILE("Fields::SolvePoissonBx()");

    PhysConst phys_const = get_phys_const();

    // Right-Hand Side for Poisson equation: compute -mu_0*d_y(jz) from the slice MF,
    // and store in the staging area of poisson_solver
    // only used with predictor corrector solver
    LinCombination(m_source_nguard, getStagingArea(lev),
                   -phys_const.mu0,
                   derivative<Direction::y>{getField(lev, WhichSlice::This, "jz"), geom[lev]},
                   phys_const.mu0,
                   derivative<Direction::z>{getField(lev, WhichSlice::Previous1, "jy"),
                   getField(lev, WhichSlice::Next, "jy"), geom[lev]});

    SetBoundaryCondition(geom, lev, "Bx", getStagingArea(lev));
    // Solve Poisson equation.
    // The RHS is in the staging area of poisson_solver.
    // The LHS will be returned as Bx_iter.
    m_poisson_solver[lev]->SolvePoissonEquation(Bx_iter);
}

void
Fields::SolvePoissonBy (amrex::MultiFab& By_iter, amrex::Vector<amrex::Geometry> const& geom,
                        const int lev)
{
    /* Solves Laplacian(By) = mu_0*(d_x(jz) - d_z(jx) ) */
    HIPACE_PROFILE("Fields::SolvePoissonBy()");

    PhysConst phys_const = get_phys_const();

    // Right-Hand Side for Poisson equation: compute mu_0*d_x(jz) from the slice MF,
    // and store in the staging area of poisson_solver
    // only used with predictor corrector solver
    LinCombination(m_source_nguard, getStagingArea(lev),
                   phys_const.mu0,
                   derivative<Direction::x>{getField(lev, WhichSlice::This, "jz"), geom[lev]},
                   -phys_const.mu0,
                   derivative<Direction::z>{getField(lev, WhichSlice::Previous1, "jx"),
                   getField(lev, WhichSlice::Next, "jx"), geom[lev]});

    SetBoundaryCondition(geom, lev, "By", getStagingArea(lev));
    // Solve Poisson equation.
    // The RHS is in the staging area of poisson_solver.
    // The LHS will be returned as By_iter.
    m_poisson_solver[lev]->SolvePoissonEquation(By_iter);
}

void
Fields::SolvePoissonBz (amrex::Vector<amrex::Geometry> const& geom, const int lev)
{
    /* Solves Laplacian(Bz) = mu_0*(d_y(jx) - d_x(jy)) */
    HIPACE_PROFILE("Fields::SolvePoissonBz()");

    PhysConst phys_const = get_phys_const();
    // Left-Hand Side for Poisson equation is Bz in the slice MF
    amrex::MultiFab lhs(getSlices(lev), amrex::make_alias, Comps[WhichSlice::This]["Bz"], 1);

    // Right-Hand Side for Poisson equation: compute mu_0*(d_y(jx) - d_x(jy))
    // from the slice MF, and store in the staging area of m_poisson_solver
    LinCombination(m_source_nguard, getStagingArea(lev),
        phys_const.mu0,
        derivative<Direction::y>{getField(lev, WhichSlice::This, "jx"), geom[lev]},
        -phys_const.mu0,
        derivative<Direction::x>{getField(lev, WhichSlice::This, "jy"), geom[lev]});


    SetBoundaryCondition(geom, lev, "Bz", getStagingArea(lev));
    // Solve Poisson equation.
    // The RHS is in the staging area of m_poisson_solver.
    // The LHS will be returned as lhs.
    m_poisson_solver[lev]->SolvePoissonEquation(lhs);

    // interpolate Bz to level 1 in the ghost cells
    InterpolateFromLev0toLev1(geom, lev, "Bz", m_slices_nguards, m_poisson_nguards);
}

void
Fields::InitialBfieldGuess (const amrex::Real relative_Bfield_error,
                            const amrex::Real predcorr_B_error_tolerance, const int lev)
{
    /* Sets the initial guess of the B field from the two previous slices
     */
    HIPACE_PROFILE("Fields::InitialBfieldGuess()");

    const amrex::Real mix_factor_init_guess = exp(-0.5_rt * pow(relative_Bfield_error /
                                              ( 2.5_rt * predcorr_B_error_tolerance ), 2));

    amrex::MultiFab::LinComb(
        getSlices(lev),
        1._rt+mix_factor_init_guess, getSlices(lev), Comps[WhichSlice::Previous1]["Bx"],
        -mix_factor_init_guess, getSlices(lev), Comps[WhichSlice::Previous2]["Bx"],
        Comps[WhichSlice::This]["Bx"], 1, m_slices_nguards);

    amrex::MultiFab::LinComb(
        getSlices(lev),
        1._rt+mix_factor_init_guess, getSlices(lev), Comps[WhichSlice::Previous1]["By"],
        -mix_factor_init_guess, getSlices(lev), Comps[WhichSlice::Previous2]["By"],
        Comps[WhichSlice::This]["By"], 1, m_slices_nguards);
}

void
Fields::MixAndShiftBfields (const amrex::MultiFab& B_iter, amrex::MultiFab& B_prev_iter,
                            const int field_comp, const amrex::Real relative_Bfield_error,
                            const amrex::Real relative_Bfield_error_prev_iter,
                            const amrex::Real predcorr_B_mixing_factor, const int lev)
{
    /* Mixes the B field according to B = a*B + (1-a)*( c*B_iter + d*B_prev_iter),
     * with a,c,d mixing coefficients.
     */
    HIPACE_PROFILE("Fields::MixAndShiftBfields()");

    /* Mixing factors to mix the current and previous iteration of the B field */
    amrex::Real weight_B_iter;
    amrex::Real weight_B_prev_iter;
    /* calculating the weight for mixing the current and previous iteration based
     * on their respective errors. Large errors will induce a small weight of and vice-versa  */
    if (relative_Bfield_error != 0._rt || relative_Bfield_error_prev_iter != 0._rt)
    {
        weight_B_iter = relative_Bfield_error_prev_iter /
                        ( relative_Bfield_error + relative_Bfield_error_prev_iter );
        weight_B_prev_iter = relative_Bfield_error /
                             ( relative_Bfield_error + relative_Bfield_error_prev_iter );
    }
    else
    {
        weight_B_iter = 0.5_rt;
        weight_B_prev_iter = 0.5_rt;
    }

    /* calculating the mixed temporary B field  B_prev_iter = c*B_iter + d*B_prev_iter.
     * This is temporarily stored in B_prev_iter just to avoid additional memory allocation.
     * B_prev_iter is overwritten at the end of this function */
    amrex::MultiFab::LinComb(
        B_prev_iter,
        weight_B_iter, B_iter, 0,
        weight_B_prev_iter, B_prev_iter, 0,
        0, 1, m_slices_nguards);

    /* calculating the mixed B field  B = a*B + (1-a)*B_prev_iter */
    amrex::MultiFab::LinComb(
        getSlices(lev),
        1._rt-predcorr_B_mixing_factor, getSlices(lev), field_comp,
        predcorr_B_mixing_factor, B_prev_iter, 0,
        field_comp, 1, m_slices_nguards);

    /* Shifting the B field from the current iteration to the previous iteration */
    amrex::MultiFab::Copy(B_prev_iter, B_iter, 0, 0, 1, m_slices_nguards);

}

amrex::Real
Fields::ComputeRelBFieldError (
    const amrex::MultiFab& Bx, const amrex::MultiFab& By, const amrex::MultiFab& Bx_iter,
    const amrex::MultiFab& By_iter, const int Bx_comp, const int By_comp, const int Bx_iter_comp,
    const int By_iter_comp, const amrex::Geometry& geom)
{
    // calculates the relative B field error between two B fields
    // for both Bx and By simultaneously
    HIPACE_PROFILE("Fields::ComputeRelBFieldError()");

    amrex::Real norm_Bdiff = 0._rt;
    amrex::Gpu::DeviceScalar<amrex::Real> gpu_norm_Bdiff(norm_Bdiff);
    amrex::Real* p_norm_Bdiff = gpu_norm_Bdiff.dataPtr();

    amrex::Real norm_B = 0._rt;
    amrex::Gpu::DeviceScalar<amrex::Real> gpu_norm_B(norm_B);
    amrex::Real* p_norm_B = gpu_norm_B.dataPtr();

    for ( amrex::MFIter mfi(Bx, DfltMfiTlng); mfi.isValid(); ++mfi ){
        const amrex::Box& bx = mfi.tilebox();
        Array2<amrex::Real const> const Bx_array = Bx.array(mfi, Bx_comp);
        Array2<amrex::Real const> const Bx_iter_array = Bx_iter.array(mfi, Bx_iter_comp);
        Array2<amrex::Real const> const By_array = By.array(mfi, By_comp);
        Array2<amrex::Real const> const By_iter_array = By_iter.array(mfi, By_iter_comp);

        amrex::ParallelFor(amrex::Gpu::KernelInfo().setReduction(true), bx,
        [=] AMREX_GPU_DEVICE (int i, int j, int, amrex::Gpu::Handler const& handler) noexcept
        {
            amrex::Gpu::deviceReduceSum(p_norm_B, std::sqrt(
                                        Bx_array(i, j) * Bx_array(i, j) +
                                        By_array(i, j) * By_array(i, j)),
                                        handler);
            amrex::Gpu::deviceReduceSum(p_norm_Bdiff, std::sqrt(
                            ( Bx_array(i, j) - Bx_iter_array(i, j) ) *
                            ( Bx_array(i, j) - Bx_iter_array(i, j) ) +
                            ( By_array(i, j) - By_iter_array(i, j) ) *
                            ( By_array(i, j) - By_iter_array(i, j) )),
                            handler);
        }
        );
    }
    norm_Bdiff = gpu_norm_Bdiff.dataValue();
    norm_B = gpu_norm_B.dataValue();

    const int numPts_transverse = geom.Domain().length(0) * geom.Domain().length(1);

    // calculating the relative error
    // Warning: this test might be not working in SI units!
    const amrex::Real relative_Bfield_error = (norm_B/numPts_transverse > 1e-10_rt)
                                               ? norm_Bdiff/norm_B : 0._rt;

    return relative_Bfield_error;
}
