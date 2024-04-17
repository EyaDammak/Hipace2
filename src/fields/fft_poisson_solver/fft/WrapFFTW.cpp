/* Copyright 2024
 *
 * This file is part of HiPACE++.
 *
 * Authors: AlexanderSinn
 *
 * License: BSD-3-Clause-LBNL
 */
#include "AnyFFT.H"

#include <AMReX_Config.H>

#include <fftw3.h>

#include <type_traits>

#ifdef AMREX_USE_FLOAT
static constexpr bool use_float = true;
#else
static constexpr bool use_float = false;
#endif

struct VendorPlan {
    fftwf_plan m_fftwf_plan;
    fftw_plan m_fftw_plan;
    FFTType m_type;
    int m_nx;
    int m_ny;
};

std::size_t AnyFFT::Initialize (FFTType type, int nx, int ny) {
    // https://www.fftw.org/fftw3_doc/FFTW-Reference.html
    m_plan = new VendorPlan;

    m_plan->m_type = type;
    m_plan->m_nx = nx;
    m_plan->m_ny = ny;
    return 0;
}

void AnyFFT::SetBuffers (void* in, void* out, [[maybe_unused]] void* work_area) {
    if constexpr (use_float) {
        switch (m_plan->m_type) {
            case FFTType::C2C_2D_fwd:
                m_plan->m_fftwf_plan = fftwf_plan_dft_2d(
                    m_plan->m_ny, m_plan->m_nx,
                    reinterpret_cast<fftwf_complex*>(in), reinterpret_cast<fftwf_complex*>(out),
                    FFTW_FORWARD, FFTW_MEASURE);
                break;
            case FFTType::C2C_2D_bkw:
                m_plan->m_fftwf_plan = fftwf_plan_dft_2d(
                    m_plan->m_ny, m_plan->m_nx,
                    reinterpret_cast<fftwf_complex*>(in), reinterpret_cast<fftwf_complex*>(out),
                    FFTW_BACKWARD, FFTW_MEASURE);
                break;
            case FFTType::C2R_2D:
                m_plan->m_fftwf_plan = fftwf_plan_dft_c2r_2d(
                    m_plan->m_ny, m_plan->m_nx,
                    reinterpret_cast<fftwf_complex*>(in), reinterpret_cast<float*>(out),
                    FFTW_MEASURE);
                break;
            case FFTType::R2C_2D:
                m_plan->m_fftwf_plan = fftwf_plan_dft_r2c_2d(
                    m_plan->m_ny, m_plan->m_nx,
                    reinterpret_cast<float*>(in), reinterpret_cast<fftwf_complex*>(out),
                    FFTW_MEASURE);
                break;
            case FFTType::R2R_2D:
                m_plan->m_fftwf_plan = fftwf_plan_r2r_2d(
                    m_plan->m_ny, m_plan->m_nx,
                    reinterpret_cast<float*>(in), reinterpret_cast<float*>(out),
                    FFTW_RODFT00, FFTW_RODFT00, FFTW_MEASURE);
                break;
            case FFTType::C2R_1D_batched:
                {
                    int n[1] = {m_plan->m_nx};
                    m_plan->m_fftwf_plan = fftwf_plan_many_dft_c2r(
                        1, n, m_plan->m_ny,
                        reinterpret_cast<fftwf_complex*>(in), nullptr, 1, m_plan->m_nx/2+1,
                        reinterpret_cast<float*>(out), nullptr, 1, m_plan->m_nx,
                        FFTW_MEASURE);
                }
                break;
            case FFTType::R2C_1D_batched:
                {
                    int n[1] = {m_plan->m_nx};
                    m_plan->m_fftwf_plan = fftwf_plan_many_dft_r2c(
                        1, n, m_plan->m_ny,
                        reinterpret_cast<float*>(in), nullptr, 1, m_plan->m_nx,
                        reinterpret_cast<fftwf_complex*>(out), nullptr, 1, m_plan->m_nx/2+1,
                        FFTW_MEASURE);
                }
                break;
        }
    } else {
        switch (m_plan->m_type) {
            case FFTType::C2C_2D_fwd:
                m_plan->m_fftw_plan = fftw_plan_dft_2d(
                    m_plan->m_ny, m_plan->m_nx,
                    reinterpret_cast<fftw_complex*>(in), reinterpret_cast<fftw_complex*>(out),
                    FFTW_FORWARD, FFTW_MEASURE);
                break;
            case FFTType::C2C_2D_bkw:
                m_plan->m_fftw_plan = fftw_plan_dft_2d(
                    m_plan->m_ny, m_plan->m_nx,
                    reinterpret_cast<fftw_complex*>(in), reinterpret_cast<fftw_complex*>(out),
                    FFTW_BACKWARD, FFTW_MEASURE);
                break;
            case FFTType::C2R_2D:
                m_plan->m_fftw_plan = fftw_plan_dft_c2r_2d(
                    m_plan->m_ny, m_plan->m_nx,
                    reinterpret_cast<fftw_complex*>(in), reinterpret_cast<double*>(out),
                    FFTW_MEASURE);
                break;
            case FFTType::R2C_2D:
                m_plan->m_fftw_plan = fftw_plan_dft_r2c_2d(
                    m_plan->m_ny, m_plan->m_nx,
                    reinterpret_cast<double*>(in), reinterpret_cast<fftw_complex*>(out),
                    FFTW_MEASURE);
                break;
            case FFTType::R2R_2D:
                m_plan->m_fftw_plan = fftw_plan_r2r_2d(
                    m_plan->m_ny, m_plan->m_nx,
                    reinterpret_cast<double*>(in), reinterpret_cast<double*>(out),
                    FFTW_RODFT00, FFTW_RODFT00, FFTW_MEASURE);
                break;
            case FFTType::C2R_1D_batched:
                {
                    int n[1] = {m_plan->m_nx};
                    m_plan->m_fftw_plan = fftw_plan_many_dft_c2r(
                        1, n, m_plan->m_ny,
                        reinterpret_cast<fftw_complex*>(in), nullptr, 1, m_plan->m_nx/2+1,
                        reinterpret_cast<double*>(out), nullptr, 1, m_plan->m_nx,
                        FFTW_MEASURE);
                }
                break;
            case FFTType::R2C_1D_batched:
                {
                    int n[1] = {m_plan->m_nx};
                    m_plan->m_fftw_plan = fftw_plan_many_dft_r2c(
                        1, n, m_plan->m_ny,
                        reinterpret_cast<double*>(in), nullptr, 1, m_plan->m_nx,
                        reinterpret_cast<fftw_complex*>(out), nullptr, 1, m_plan->m_nx/2+1,
                        FFTW_MEASURE);
                }
                break;
        }
    }
}

void AnyFFT::Execute () {
    if constexpr (use_float) {
        fftwf_execute(m_plan->m_fftwf_plan);
    } else {
        fftw_execute(m_plan->m_fftw_plan);
    }
}

AnyFFT::~AnyFFT () {
    if (m_plan) {
        if constexpr (use_float) {
            fftwf_destroy_plan(m_plan->m_fftwf_plan);
        } else {
            fftw_destroy_plan(m_plan->m_fftw_plan);
        }
        delete m_plan;
    }
}
