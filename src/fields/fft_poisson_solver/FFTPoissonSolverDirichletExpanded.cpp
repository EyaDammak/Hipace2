/* Copyright 2020-2022
 *
 * This file is part of HiPACE++.
 *
 * Authors: AlexanderSinn, Axel Huebl, MaxThevenet, Severin Diederichs
 *
 * License: BSD-3-Clause-LBNL
 */
#include "FFTPoissonSolverDirichletExpanded.H"
#include "fft/AnyFFT.H"
#include "fields/Fields.H"
#include "utils/Constants.H"
#include "utils/GPUUtil.H"
#include "utils/HipaceProfilerWrapper.H"

FFTPoissonSolverDirichletExpanded::FFTPoissonSolverDirichletExpanded (
    amrex::BoxArray const& realspace_ba,
    amrex::DistributionMapping const& dm,
    amrex::Geometry const& gm )
{
    define(realspace_ba, dm, gm);
}

void ExpandR2R (amrex::FArrayBox& dst, amrex::FArrayBox& src)
{
    const amrex::Box bx = src.box();
    const int nx = bx.length(0);
    const int ny = bx.length(1);
    const amrex::IntVect lo = bx.smallEnd();
    Array2<amrex::Real const> const src_array = src.const_array();
    Array2<amrex::Real> const dst_array = dst.array();

    amrex::ParallelFor(bx,
        [=] AMREX_GPU_DEVICE(int i, int j, int)
        {
            /* upper left quadrant */
            dst_array(i+1,j+1) = src_array(i, j);
            /* lower left quadrant */
            dst_array(i+1,j+ny+2) = -src_array(i, ny-1-j+2*lo[1]);
            /* upper right quadrant */
            dst_array(i+nx+2,j+1) = -src_array(nx-1-i+2*lo[0], j);
            /* lower right quadrant */
            dst_array(i+nx+2,j+ny+2) = src_array(nx-1-i+2*lo[0], ny-1-j+2*lo[1]);
        });
}

void ShrinkC2R (amrex::FArrayBox& dst, amrex::BaseFab<amrex::GpuComplex<amrex::Real>>& src)
{
    const amrex::Box bx = dst.box();
    Array2<amrex::GpuComplex<amrex::Real> const> const src_array = src.const_array();
    Array2<amrex::Real> const dst_array = dst.array();
    amrex::ParallelFor(bx,
        [=] AMREX_GPU_DEVICE(int i, int j, int)
        {
            /* upper left quadrant */
            dst_array(i,j) = -src_array(i+1, j+1).real();
        });
}

void
FFTPoissonSolverDirichletExpanded::define (amrex::BoxArray const& a_realspace_ba,
                                           amrex::DistributionMapping const& dm,
                                           amrex::Geometry const& gm )
{
    HIPACE_PROFILE("FFTPoissonSolverDirichletExpanded::define()");
    using namespace amrex::literals;

    // If we are going to support parallel FFT, the constructor needs to take a communicator.
    AMREX_ALWAYS_ASSERT_WITH_MESSAGE(a_realspace_ba.size() == 1, "Parallel FFT not supported yet");

    // Allocate temporary arrays - in real space and spectral space
    // These arrays will store the data just before/after the FFT
    // The stagingArea is also created from 0 to nx, because the real space array may have
    // an offset for levels > 0
    m_stagingArea = amrex::MultiFab(a_realspace_ba, dm, 1, Fields::m_poisson_nguards);
    m_tmpSpectralField = amrex::MultiFab(a_realspace_ba, dm, 1, Fields::m_poisson_nguards);
    m_eigenvalue_matrix = amrex::MultiFab(a_realspace_ba, dm, 1, Fields::m_poisson_nguards);
    m_stagingArea.setVal(0.0, Fields::m_poisson_nguards); // this is not required
    m_tmpSpectralField.setVal(0.0, Fields::m_poisson_nguards);

    // This must be true even for parallel FFT.
    AMREX_ALWAYS_ASSERT_WITH_MESSAGE(m_stagingArea.local_size() == 1,
                                     "There should be only one box locally.");
    AMREX_ALWAYS_ASSERT_WITH_MESSAGE(m_tmpSpectralField.local_size() == 1,
                                     "There should be only one box locally.");

    const amrex::Box fft_box = m_stagingArea[0].box();
    const amrex::IntVect fft_size = fft_box.length();
    const int nx = fft_size[0];
    const int ny = fft_size[1];
    const auto dx = gm.CellSizeArray();
    const amrex::Real dxsquared = dx[0]*dx[0];
    const amrex::Real dysquared = dx[1]*dx[1];
    const amrex::Real sine_x_factor = MathConst::pi / ( 2. * ( nx + 1 ));
    const amrex::Real sine_y_factor = MathConst::pi / ( 2. * ( ny + 1 ));

    // Normalization of FFTW's 'DST-I' discrete sine transform (FFTW_RODFT00)
    // This normalization is used regardless of the sine transform library
    const amrex::Real norm_fac = 0.5 / ( 2 * (( nx + 1 ) * ( ny + 1 )));

    // Calculate the array of m_eigenvalue_matrix
    for (amrex::MFIter mfi(m_eigenvalue_matrix, DfltMfi); mfi.isValid(); ++mfi ){
        Array2<amrex::Real> eigenvalue_matrix = m_eigenvalue_matrix.array(mfi);
        amrex::IntVect lo = fft_box.smallEnd();
        amrex::ParallelFor(
            fft_box, [=] AMREX_GPU_DEVICE (int i, int j, int /* k */) noexcept
                {
                    /* fast poisson solver diagonal x coeffs */
                    amrex::Real sinex_sq = std::sin(( i - lo[0] + 1 ) * sine_x_factor) * std::sin(( i - lo[0] + 1 ) * sine_x_factor);
                    /* fast poisson solver diagonal y coeffs */
                    amrex::Real siney_sq = std::sin(( j - lo[1] + 1 ) * sine_y_factor) * std::sin(( j - lo[1] + 1 ) * sine_y_factor);

                    if ((sinex_sq!=0) && (siney_sq!=0)) {
                        eigenvalue_matrix(i,j) = norm_fac / ( -4.0 * ( sinex_sq / dxsquared + siney_sq / dysquared ));
                    } else {
                        // Avoid division by 0
                        eigenvalue_matrix(i,j) = 0._rt;
                    }
                });
    }

    // Allocate expanded_position_array Real of size (2*nx+2, 2*ny+2)
    // Allocate expanded_fourier_array Complex of size (nx+2, 2*ny+2)
    amrex::Box expanded_position_box {{0, 0, 0}, {2*nx+1, 2*ny+1, 0}};
    amrex::Box expanded_fourier_box {{0, 0, 0}, {nx+1, 2*ny+1, 0}};
    // shift box to match rest of fields
    expanded_position_box += fft_box.smallEnd();
    expanded_fourier_box += fft_box.smallEnd();

    m_expanded_position_array.resize(expanded_position_box);
    m_expanded_fourier_array.resize(expanded_fourier_box);

    m_expanded_position_array.setVal<amrex::RunOn::Device>(0._rt);

    // Allocate and initialize the FFT plan
    std::size_t wrok_size = m_fft.Initialize(FFTType::R2C_2D, expanded_position_box.length(0),
                                             expanded_position_box.length(1));

    // Allocate work area for the FFT
    m_fft_work_area.resize(wrok_size);

    m_fft.SetBuffers(m_expanded_position_array.dataPtr(), m_expanded_fourier_array.dataPtr(),
                     m_fft_work_area.dataPtr());
}


void
FFTPoissonSolverDirichletExpanded::SolvePoissonEquation (amrex::MultiFab& lhs_mf)
{
    HIPACE_PROFILE("FFTPoissonSolverDirichletExpanded::SolvePoissonEquation()");

    ExpandR2R(m_expanded_position_array, m_stagingArea[0]);

    m_fft.Execute();

    ShrinkC2R(m_tmpSpectralField[0], m_expanded_fourier_array);

#ifdef AMREX_USE_OMP
#pragma omp parallel if (amrex::Gpu::notInLaunchRegion())
#endif
    for ( amrex::MFIter mfi(m_stagingArea, DfltMfiTlng); mfi.isValid(); ++mfi ){
        // Solve Poisson equation in Fourier space:
        // Multiply `tmpSpectralField` by eigenvalue_matrix
        Array2<amrex::Real> tmp_cmplx_arr = m_tmpSpectralField.array(mfi);
        Array2<amrex::Real> eigenvalue_matrix = m_eigenvalue_matrix.array(mfi);

        amrex::ParallelFor( mfi.growntilebox(),
            [=] AMREX_GPU_DEVICE(int i, int j, int) noexcept {
                tmp_cmplx_arr(i,j) *= eigenvalue_matrix(i,j);
            });
    }

    ExpandR2R(m_expanded_position_array, m_tmpSpectralField[0]);

    m_fft.Execute();

    ShrinkC2R(m_stagingArea[0], m_expanded_fourier_array);

#ifdef AMREX_USE_OMP
#pragma omp parallel if (amrex::Gpu::notInLaunchRegion())
#endif
    for ( amrex::MFIter mfi(m_stagingArea, DfltMfiTlng); mfi.isValid(); ++mfi ){
        // Copy from the staging area to output array (and normalize)
        Array2<amrex::Real> tmp_real_arr = m_stagingArea.array(mfi);
        Array2<amrex::Real> lhs_arr = lhs_mf.array(mfi);
        AMREX_ALWAYS_ASSERT_WITH_MESSAGE(lhs_mf.size() == 1,
                                         "Slice MFs must be defined on one box only");
        amrex::ParallelFor( lhs_mf[mfi].box() & mfi.growntilebox(),
            [=] AMREX_GPU_DEVICE(int i, int j, int) noexcept {
                // Copy field
                lhs_arr(i,j) = tmp_real_arr(i,j);
            });
    }
}
